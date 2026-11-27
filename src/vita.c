/*
 * vita.c -- PS Vita boot glue for the FlatOut 2 recompilation.
 *
 * Keeps game initialization separate from Win32 vocabulary that platform
 * headers may bring in (Thread, Event, ...).
 *
 *   logs      ux0:data/fo2x/fo2x_log.txt, one stdio stream (stderr, with
 *             stdout on the same file). vita_log() stamps each line with
 *             seconds since boot and hands it to stderr in 64 KB blocks.
 *   settings  ux0:data/fo2x/fo2x_env.txt, KEY=VALUE per line, '#'
 *             comments -- Vita has no environment
 *   crashes   prints guest state on exception
 */
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include <psp2/power.h>

#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include "apu_xaudio2.h"

#define FO2X_VITA_DIR "ux0:data/fo2x"
#define FO2X_VITA_LOG FO2X_VITA_DIR "/fo2x_log.txt"

/* Process settings read by the SDK's startup code, not by our code.
 *
 * sceLibcHeapSize is the newlib heap. The big blocks -- guest RAM (64 MB,
 * XBOX_TOTAL_RAM), the contiguous window (XBOX_CONTIG_SIZE, 64 MB) -- are
 * sceKernelAllocMemBlock memblocks (win32_compat.c vita_big_alloc), not heap,
 * so the heap only holds the small allocations and the kernel tables
 * (~24 MB, xbox_tables.c). taiHEN plugins take part of RAM, so the heap is
 * kept at 96 MB and the app takes the default memsize (build.sh sets no
 * EBOOT_APP_MEMSIZE). The boot log prints the free user memory
 * ("[vita] memory available").
 *
 * sceUserMainThreadStackSize: the lifted code keeps large frames (~20 KB
 * per routine), so the default main stack is too small. */
unsigned int sceLibcHeapSize = 96u * 1024u * 1024u;
unsigned int sceUserMainThreadStackSize = 2u * 1024u * 1024u;

extern ptrdiff_t g_xbox_mem_offset;
void print_guest_state(void);

static uint64_t s_t0;

/* Address of a local in vita_boot(), near the top of the main thread's stack.
 * A Vita fault has no handler and leaves no log line, so a dead app with a
 * deep host stack is the sign of an overflow of the 2 MB main stack (the
 * lifted routines carry ~20 KB frames). vita_host_stack_kb() is the depth
 * below that frame; ~0u when the caller is not on the boot thread's stack. */
static uintptr_t s_stack_ref;

unsigned vita_host_stack_kb(void)
{
    volatile char here = 0;
    uintptr_t sp = (uintptr_t)&here;

    if (!s_stack_ref || sp > s_stack_ref || s_stack_ref - sp > (2u << 20))
        return ~0u;
    return (unsigned)((s_stack_ref - sp) >> 10);
}

static double since_boot(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t now = (uint64_t)tv.tv_sec * 1000000ull + tv.tv_usec;
    return (double)(now - s_t0) / 1e6;
}

/* ── The log ─────────────────────────────────────────────────────
 *
 * One sink: fo2x_log.txt, written through stderr (stdout is appended to the
 * same file). vita_log() batches lines in lbuf and hands them to stderr when
 * the buffer is full, at the 500 ms flusher, and at shutdown; stderr is fully
 * buffered (64 KB) and flushed there, so a line costs no SD write of its own.
 *
 * This used to be a second handle, a sceIoOpen on the same file for
 * vita_log() alone. On the device that open failed (the log shows "[vita]
 * log device unavailable" at every boot and not one vita_log line), so every
 * vita_log() line was dropped: the [boot]/[main]/[file] markers never
 * reached the file. One stdio stream fixes that and removes the seek race
 * between two handles. */
static volatile int s_log_off;
static volatile int s_no_log = 1;

/* vita_log() lines wait here until a drain: the buffer is full, the 500 ms
 * flusher runs, or the process shuts down. A line logged and then followed by
 * silence used to stay here until the next vita_log() call, so the tail of
 * the log (what happened last) was lost. Caller holds s_log_lock. */
static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char lbuf[16384];
static int  lpos;

static void log_drain_locked(void)
{
    if (lpos > 0)
        fwrite(lbuf, 1, (size_t)lpos, stderr);
    lpos = 0;
}

static void log_drain(void)
{
    pthread_mutex_lock(&s_log_lock);
    log_drain_locked();
    pthread_mutex_unlock(&s_log_lock);
}

void vita_log(const char *fmt, ...)
{
    if (s_no_log) return;
    char buf[768];
    va_list ap;
    int n;

    n = snprintf(buf, sizeof buf, "[%9.3f] ", since_boot());
    va_start(ap, fmt);
    n += vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 2)
        n = (int)sizeof buf - 2;   /* truncated line: keep the newline */
    buf[n++] = '\n';

    if (strstr(buf, "[READ") || strstr(buf, "[ntavm]") || strstr(buf, "[HEAP") ||
        strstr(buf, "[KERNEL]   ordinal") || strstr(buf, "[GPU]   0x") ||
        strstr(buf, "[fb] step") || strstr(buf, "[IRQLHOLD]") || strstr(buf, "[apu-step]") || strstr(buf, "[GL] tex_get") || strstr(buf, "[NV2A] DMA_PUT") || strstr(buf, "[KERNEL] summary"))
        return;

    if (strstr(buf, "[READ") || strstr(buf, "[ntavm]") || strstr(buf, "[HEAP") ||
        strstr(buf, "[KERNEL]   ordinal") || strstr(buf, "[GPU]   0x") ||
        strstr(buf, "[fb] step") || strstr(buf, "[IRQLHOLD]") || strstr(buf, "[apu-step]") || strstr(buf, "[GL] tex_get") || strstr(buf, "[NV2A] DMA_PUT") || strstr(buf, "[KERNEL] summary"))
        return;

    pthread_mutex_lock(&s_log_lock);
    if (lpos + n >= (int)sizeof(lbuf))
        log_drain_locked();
    if (n < (int)sizeof(lbuf)) {
        memcpy(lbuf + lpos, buf, n);
        lpos += n;
    }
    pthread_mutex_unlock(&s_log_lock);
}

static void log_flush(void)
{
    fflush(stdout);
    fflush(stderr);
}

static void *log_flusher(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(500000);     /* 500 ms */
        if (s_log_off)
            break;
        log_drain();
        log_flush();
    }
    return NULL;
}

static int log_open(void)
{
    /* Truncate once, then append: stdout and stderr both open the file in
     * "a" mode (O_APPEND), so neither handle overwrites the other's bytes.
     * A "w" open of the file is the truncation; no second handle is kept. */
    FILE *t = fopen(FO2X_VITA_LOG, "w");
    if (!t)
        return 0;
    fclose(t);

    if (!freopen(FO2X_VITA_LOG, "a", stdout))
        return 0;
    if (!freopen(FO2X_VITA_LOG, "a", stderr))
        return 0;
    /* 64 KB blocks, written out by the 500 ms flusher (and by
     * vita_error_dialog / vita_shutdown). Before this stderr was unbuffered,
     * and every fprintf of a hot path -- and every fflush at vblank -- was a
     * separate SD write. */
    setvbuf(stdout, NULL, _IOFBF, 65536);
    setvbuf(stderr, NULL, _IOFBF, 65536);
    {
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);  /* 2 MB stack */
        if (pthread_create(&t, &attr, log_flusher, NULL) == 0)
            pthread_detach(t);
        pthread_attr_destroy(&attr);
    }
    return 1;
}

static void load_env(void)
{
    FILE *f = fopen(FO2X_VITA_DIR "/fo2x_env.txt", "r");
    char line[1024];

    if (!f) {
        vita_log("[vita] no " FO2X_VITA_DIR "/fo2x_env.txt: default settings");
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *eq, *end = line + strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
            *--end = 0;
        if (!line[0] || line[0] == '#' || !(eq = strchr(line, '=')) || eq == line)
            continue;
        *eq = 0;
        setenv(line, eq + 1, 1);
        vita_log("[vita] env %s=%s", line, eq + 1);
    }
    fclose(f);
}

/* Full clocks for the whole run, set before the game starts: CPU 444 MHz,
 * bus 222, GPU 222, GPU crossbar 166. The return codes go to the log, so a
 * rejected clock shows as a number there (0 = done). Set once here; before
 * this they were set on the first controller poll, after the game had
 * already started. */
static void set_max_clocks(void)
{
    int arm = scePowerSetArmClockFrequency(444);
    int bus = scePowerSetBusClockFrequency(222);
    int gpu = scePowerSetGpuClockFrequency(222);
    int xbar = scePowerSetGpuXbarClockFrequency(166);
    vita_log("[vita] clocks: arm 444 -> %d, bus 222 -> %d, gpu 222 -> %d, xbar 166 -> %d",
             arm, bus, gpu, xbar);
}

void vita_boot(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    s_t0 = (uint64_t)tv.tv_sec * 1000000ull + tv.tv_usec;
    {
        volatile char top = 0;
        s_stack_ref = (uintptr_t)&top;
    }

    /* Create data directories if they don't exist. The root goes first: the
     * log and fo2x_env.txt live in it. */
    sceIoMkdir(FO2X_VITA_DIR, 0777);
    sceIoMkdir(FO2X_VITA_DIR "/game", 0777);
    sceIoMkdir(FO2X_VITA_DIR "/save", 0777);

    /* Unbuffered before anything else prints, so the first lines are on disk. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IOFBF, 65536);

    sceKernelChangeThreadCpuAffinityMask(SCE_KERNEL_THREAD_ID_SELF, SCE_KERNEL_CPU_MASK_USER_0);
    sceKernelChangeThreadPriority(SCE_KERNEL_THREAD_ID_SELF, 0x60);
    set_max_clocks();
    load_env();
    {
        const char *en = getenv("NFSU2_LOG");
        const char *dis = getenv("NFSU2_NO_LOG");
        if (en && atoi(en) == 1 && !(dis && atoi(dis) == 1)) {
            s_no_log = 0;
            if (!log_open())
                fprintf(stderr, "[vita] log device unavailable\n");
            vita_log("[vita] boot: log %s", FO2X_VITA_LOG);
        }
    }

    {
        SceKernelFreeMemorySizeInfo meminfo = { .size = sizeof(SceKernelFreeMemorySizeInfo) };
        sceKernelGetFreeMemorySize(&meminfo);
        vita_log("[vita] memory available: %llu MB",
                 (unsigned long long)(meminfo.size_user >> 20));
    }
}

void vita_error_dialog(const char *msg, const char *details)
{
    vita_log("[ERROR] %s: %s", msg, details);
    log_flush();
    fprintf(stderr, "[ERROR] %s: %s\n", msg, details);
}

void vita_shutdown(void)
{
    vita_log("[vita] shutdown");
    s_log_off = 1;
    log_drain();
    log_flush();
}

#endif

#include <string.h>
void *sceDmacMemcpy(void *dst, const void *src, size_t size) { return memcpy(dst, src, size); }
