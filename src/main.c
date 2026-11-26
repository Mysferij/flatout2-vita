/**
 * FlatOut 2 (Xbox) - recompiled game entry point
 *
 * Boot order (same on every host):
 *   crash reporter -> load default.xbe -> xbox_MemoryLayoutInit ->
 *   xbox_kernel_init -> xbox_path_init -> xbox_kernel_bridge_init ->
 *   g_esp = XBOX_STACK_TOP -> recomp_dispatch_init -> xbe_entry_point()
 *
 * Game data is the extracted disc (default.xbe, flatout2.bfs, Data/). Where it is
 * looked for:
 *   Windows/Linux: $NFSU2_GAME_DIR, else ./game
 *   Switch:        sdmc:/switch/fo2x/game
 */

#ifdef _WIN32
#  include <windows.h>
#  include <dbghelp.h>
#else
#  include <signal.h>
#  include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include <xbox/xboxrecomp.h>

#ifdef __SWITCH__
/* libnx lives in switch_nx.c: <switch.h> and the Win32 vocabulary collide. */
void switch_boot(void);
void switch_shutdown(void);
void switch_error_dialog(const char *msg, const char *details);
#elif defined(__vita__)
/* vita.c: the log, the data directories, fo2x_env.txt. No message box: the
 * SceCommonDialog one brought the console down (C2-12828-1). */
void vita_boot(void);
void vita_shutdown(void);
void vita_error_dialog(const char *msg, const char *details);
void vita_log(const char *fmt, ...);   /* sceIoWrite to fo2x_log.txt, unbuffered */
#elif !defined(_WIN32)
int SDL_ShowSimpleMessageBox(uint32_t flags, const char *title, const char *message,
                             void *window);
#endif

/* Boot step markers. On Vita each one is a sceIoWrite to fo2x_log.txt, so the
 * last "[boot] ... begin" with no "done" after it names the step that died. */
#ifdef __vita__
#define BOOT_MARK(...) vita_log("[boot] " __VA_ARGS__)
#else
#define BOOT_MARK(...) ((void)0)
#endif

#include <xbox/recomp_regs.h>
extern ptrdiff_t g_xbox_mem_offset;
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);
extern void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val,
                                unsigned int size);

#define NFSU2_APU_BASE 0xFE800000u
#define NFSU2_APU_END  0xFE880000u

static uint32_t apu_mmio_rd(void *opaque, uint32_t off, unsigned size)
{
    return (uint32_t)mcpx_apu_mmio_read((MCPXAPUState *)opaque, off, size);
}

static void apu_mmio_wr(void *opaque, uint32_t off, uint32_t val, unsigned size)
{
    mcpx_apu_mmio_write((MCPXAPUState *)opaque, off, val, size);
}

extern void xbe_entry_point(void);
extern int recomp_dispatch_init(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);

#define NFSU2_ENTRY_POINT 0x0013216Du


#if defined(__SWITCH__)
#  define NFSU2_DEFAULT_GAME_DIR "sdmc:/switch/fo2x/game"
#  define NFSU2_DEFAULT_SAVE_DIR "sdmc:/switch/fo2x/save"
#elif defined(__vita__)
/* vita.c creates these directories at boot; the disc is unpacked into game/. */
#  define NFSU2_DEFAULT_GAME_DIR "ux0:data/fo2x/game"
#  define NFSU2_DEFAULT_SAVE_DIR NULL
/* The boot file on the Vita, by full path (no directory joining). */
#  define NFSU2_VITA_XBE      "ux0:data/fo2x/game/default.xbe"
#  define NFSU2_VITA_XBE_UC   "ux0:data/fo2x/game/DEFAULT.XBE"
#else
#  define NFSU2_DEFAULT_GAME_DIR "game"
#  define NFSU2_DEFAULT_SAVE_DIR NULL
#endif

/* ------------------------------------------------------------------ */
/* Crash reporting                                                     */
/* ------------------------------------------------------------------ */

/* What the guest was doing: its registers and the return addresses on its
 * stack. This is the useful half of any crash report -- the host PC names a
 * generated sub_XXXXXXXX at best, the guest stack names the whole chain. */
void print_guest_state(void)
{
    fprintf(stderr, "  guest: eax=%08X ecx=%08X edx=%08X ebx=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx);
    fprintf(stderr, "         esi=%08X edi=%08X esp=%08X\n",
            g_esi, g_edi, g_esp);

    if (g_xbox_mem_offset && g_esp >= XBOX_STACK_BASE
            && g_esp < XBOX_STACK_BASE + XBOX_STACK_SIZE) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 512 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (g_esp + (uint32_t)i * 4u >= XBOX_STACK_BASE + XBOX_STACK_SIZE)
                break;
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
    }
    {
        unsigned k;
        fprintf(stderr, "  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

#ifdef _WIN32
static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        DWORD64 disp = 0;

        /* The NV2A aperture belongs to the runtime's own handlers. */
        if (fault >= 0xFD000000 && fault < 0xFE000000)
            return EXCEPTION_CONTINUE_SEARCH;

        fprintf(stderr, "[CRASH] access violation at RIP=0x%llX, %s 0x%llX "
                        "(guest VA 0x%08X)\n",
                (unsigned long long)ep->ContextRecord->Rip,
                ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                (unsigned long long)fault,
                (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset));
        memset(buf, 0, sizeof(buf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        if (SymFromAddr(GetCurrentProcess(), ep->ContextRecord->Rip, &disp, sym))
            fprintf(stderr, "  in %s+0x%llX\n", sym->Name,
                    (unsigned long long)disp);
        print_guest_state();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void install_crash_reporter(void)
{
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
}
#elif defined(__SWITCH__)
/* The CPU exception handler is in switch_nx.c; it calls print_guest_state. */
static void install_crash_reporter(void) {}
#elif !defined(__vita__)
#include <execinfo.h>

static void crash_signal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    fprintf(stderr, "[CRASH] signal %d, fault addr %p (guest VA 0x%08X)\n",
            sig, si->si_addr,
            (uint32_t)((uintptr_t)si->si_addr - (uintptr_t)g_xbox_mem_offset));
    print_guest_state();
    {
        /* The host frames name the lifted function (sub_XXXXXXXX) that
         * faulted -- the guest stack alone does not on a worker thread. */
        void *frames[32];
        int n = backtrace(frames, 32);
        fprintf(stderr, "  host backtrace:\n");
        fflush(stderr);
        backtrace_symbols_fd(frames, n, 2);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_reporter(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    {
        static char altstack[64 * 1024];
        stack_t ss;
        ss.ss_sp = altstack;
        ss.ss_size = sizeof(altstack);
        ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
    }
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}
#else
/* Vita: crash handling is in vita.c */
static void install_crash_reporter(void) {}
#endif

/* ------------------------------------------------------------------ */

static void *load_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    void *data;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    data = malloc((size_t)size);
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data)
        *out_size = (size_t)size;
    return data;
}

static void fatal(const char *msg)
{
    fprintf(stderr, "[FATAL] %s\n", msg);
#if defined(_WIN32)
    MessageBoxA(NULL, msg, "FlatOut 2 recomp", MB_ICONERROR);
#elif defined(__SWITCH__)
    switch_error_dialog(msg, NULL);
#elif defined(__vita__)
    /* Log only: the error goes to fo2x_log.txt and the caller returns, so the
     * app exits cleanly instead of opening a system dialog. */
    vita_error_dialog(msg, "see fo2x_log.txt");
#else
    SDL_ShowSimpleMessageBox(0x10, "FlatOut 2 recomp", msg, NULL);
#endif
}

static int game_main(void);
void xbox_guest_pin(int interrupt);   /* win32_compat.c: the Xbox's one CPU */
void xbox_gil_enter(void);             /* kernel_bridge.c: the guest lock */
void xbox_gil_leave(void);
void xbox_gil_mark_main(void);
void nfsu2_text_patch_init(void);    /* text_patch.c: Switch button names */

#ifdef __SWITCH__
#include <pthread.h>
/* The first guest thread runs inline on whichever host thread boots the
 * title, and Horizon's main thread stack is small for recompiled code; boot
 * on a thread with a stack sized like a desktop main thread instead. */
void xbox_nx_spread_thread(void);   /* win32_compat.c: off core 0, all cores allowed */
void xbox_nx_track_thread(void *entry);   /* win32_compat.c: Switch [perf] report */

static void *game_thread(void *arg)
{
    xbox_nx_spread_thread();
    xbox_nx_track_thread(NULL);          /* "game:" in the [perf] report */
    *(int *)arg = game_main();
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_attr_t attr;
    pthread_t th;
    int rc = 1;

    (void)argc;
    (void)argv;
    switch_boot();
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u * 1024 * 1024);
    if (pthread_create(&th, &attr, game_thread, &rc) == 0)
        pthread_join(th, NULL);
    else
        fprintf(stderr, "[FATAL] cannot start the game thread\n");
    switch_shutdown();
    return rc;
}
#else
int main(int argc, char **argv)
{
#ifdef __vita__
    /* First thing: the log and the data directories exist before anything
     * else runs, so every later line (including a failed boot) is on disk. */
    vita_boot();
    vita_log("[main] start: vita_boot done, calling game_main; host frame %p "
             "(stack reference for [kdisp] frames)", __builtin_frame_address(0));
#endif
    (void)argc;
    (void)argv;
#ifdef __vita__
    {
        int rc = game_main();
        vita_log("[main] game_main returned %d", rc);
        return rc;
    }
#else
    return game_main();
#endif
}
#endif

static int game_main(void)
{
    extern void xbox_tables_alloc(void);   /* kernel/xbox_tables.c */
    xbox_tables_alloc();   /* the big tables: heap, not .bss (Vita C1-2613-2) */
    xbox_guest_pin(0);     /* the boot thread becomes the title's first thread */
    char xbe_path[512];
    const char *game_dir;
    void *xbe_data;
    size_t xbe_size = 0;

#ifndef __vita__
    /* Vita: vita_boot() already set both streams unbuffered on the log. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#endif

    printf("=== FlatOut 2 (Xbox) - static recompilation ===\n");
    install_crash_reporter();
    if (getenv("NFSU2_EXIT_TRACE"))
        atexit(print_guest_state);

    /* Runtime defaults this title needs; an explicit setting still wins.
     *   RECOMP_VBLANK      the linked D3D waits on its vblank ISR every frame
     *   RECOMP_AC97_READY  DirectSound needs the codec present to create; "plain"
     *                      is the variant that needs no fault handling */
#ifdef _WIN32
    if (!getenv("RECOMP_VBLANK"))     _putenv("RECOMP_VBLANK=1");
    if (!getenv("RECOMP_AC97_READY")) _putenv("RECOMP_AC97_READY=plain");
    if (!getenv("RECOMP_USB"))        _putenv("RECOMP_USB=1");
#else
    setenv("RECOMP_VBLANK", "1", 0);
    setenv("RECOMP_AC97_READY", "plain", 0);
    setenv("RECOMP_USB", "1", 0);       /* the pad is on the MCPX's OHCI */
    setenv("RECOMP_PB_EXEC", "1", 0);   /* the title draws through NV2A */
    /* The executor's off-screen batch cull (an NFSU2 speed-up) dropped draws
     * in Burnout 3; off until checked on this title. */
    setenv("RECOMP_CULL", "0", 0);
    /* FMOD sends every effect to mixbins 6/7 and the same signal at half
     * level to the reverb send 10, which on hardware its DSP image turns into
     * reverb. With the DSP stubbed, keep only a little of it (apu_dsp.c). */
    setenv("RECOMP_APU_DOWNMIX", "10:0.25:0.25", 0);
#endif
#if defined(__SWITCH__) || defined(__vita__)
    /* Every periodic log line flushes to the SD card; keep the log to what
     * matters on a console. RECOMP_QUIET=0 in fo2x_env.txt brings it back. */
    setenv("RECOMP_QUIET", "1", 0);
#endif

#ifdef __vita__
    /* Fixed on the Vita: the disc is ux0:data/fo2x/game, whatever the
     * environment (fo2x_env.txt is loaded into it) says. */
    game_dir = NFSU2_DEFAULT_GAME_DIR;
#else
    game_dir = getenv("NFSU2_GAME_DIR");
    if (!game_dir || !game_dir[0])
        game_dir = NFSU2_DEFAULT_GAME_DIR;
#endif
    /* The boot file: default.xbe as shipped; DEFAULT.XBE is tried too, for
     * copies whose names were upper-cased. Every full path is logged before
     * it is opened, so a missing file shows where the game looked. */
#ifdef __vita__
    static const char *const xbe_paths[] = { NFSU2_VITA_XBE, NFSU2_VITA_XBE_UC };
    const size_t xbe_count = sizeof(xbe_paths) / sizeof(xbe_paths[0]);
    vita_log("[boot] before load default.xbe: %s (or %s)", xbe_paths[0], xbe_paths[1]);
#else
    static const char *const xbe_names[] = { "default.xbe", "DEFAULT.XBE" };
    const size_t xbe_count = sizeof(xbe_names) / sizeof(xbe_names[0]);
#endif
    xbe_data = NULL;
    for (size_t i = 0; i < xbe_count && !xbe_data; i++) {
#ifdef __vita__
        snprintf(xbe_path, sizeof(xbe_path), "%s", xbe_paths[i]);
        printf("[boot] opening %s\n", xbe_path);
#else
        snprintf(xbe_path, sizeof(xbe_path), "%s/%s", game_dir, xbe_names[i]);
        printf("[boot] game dir %s, opening %s\n", game_dir, xbe_path);
#endif
        fflush(stdout);
        xbe_data = load_file(xbe_path, &xbe_size);
        if (!xbe_data)
            fprintf(stderr, "cannot read %s\n", xbe_path);
    }
#ifdef __vita__
    if (xbe_data)
        vita_log("[boot] after load default.xbe: %s, %u bytes", xbe_path, (unsigned)xbe_size);
    else
        vita_log("[boot] after load default.xbe: FAILED (last tried %s)", xbe_path);
#endif
    if (!xbe_data) {
        char msg[640];
#ifdef __vita__
        snprintf(msg, sizeof(msg),
                 "Failed to load default.xbe (tried default.xbe and DEFAULT.XBE in "
                 "%s). Copy the extracted disc to ux0:data/fo2x/game.", game_dir);
#else
        snprintf(msg, sizeof(msg),
                 "Failed to load default.xbe (tried default.xbe and DEFAULT.XBE in "
                 "%s). Put the extracted disc in the game directory (or set "
                 "NFSU2_GAME_DIR).", game_dir);
#endif
        fatal(msg);
        return 1;
    }
    printf("XBE %s: %u bytes\n", xbe_path, (unsigned)xbe_size);

    BOOT_MARK("xbox_MemoryLayoutInit: begin (xbe %u bytes)", (unsigned)xbe_size);
    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        BOOT_MARK("xbox_MemoryLayoutInit: FAILED");
        fatal("Failed to initialise the Xbox memory layout "
              "(the guest address range may be unavailable).");
        free(xbe_data);
        return 1;
    }
    BOOT_MARK("main: xbox_MemoryLayoutInit returned TRUE; reading host offset");
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    BOOT_MARK("xbox_MemoryLayoutInit: done (host offset 0x%llX)",
              (unsigned long long)g_xbox_mem_offset);
    printf("Xbox memory mapped at host offset 0x%llX\n",
           (unsigned long long)g_xbox_mem_offset);

    /* The emulated APU. DirectSound's accesses reach it through the MMIO
     * accessors the DSOUND section is lifted with (regen.sh passes
     * --mmio-sections DSOUND), so the APU registers stay plain memory and no
     * fault handling is needed -- the same on every host. NFSU2_APU=0 runs
     * without it (DirectSound then creates, but nothing ever plays).
     * Under RECOMP_AC97_READY=1 the runtime traps the APU instead (Windows). */
    if (getenv("RECOMP_AC97_READY")) {
        const char *apu = getenv("NFSU2_APU");
        int plain = strcmp(getenv("RECOMP_AC97_READY"), "plain") == 0;

        if (!plain || !apu || strcmp(apu, "0") != 0) {
            BOOT_MARK("APU: begin (mcpx_apu_init_standalone)");
            g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
            BOOT_MARK("APU: mcpx_apu_init_standalone returned %p", (void *)g_apu_state);
            fprintf(stderr, "[BOOT] emulated APU %s\n",
                    g_apu_state ? "up" : "FAILED to initialise");
            if (g_apu_state && plain) {
                BOOT_MARK("APU: registering MMIO window 0x%08X-0x%08X",
                          NFSU2_APU_BASE, NFSU2_APU_END);
                xbox_MmioRegister(NFSU2_APU_BASE, NFSU2_APU_END,
                                  apu_mmio_rd, apu_mmio_wr, g_apu_state);
                BOOT_MARK("APU: MMIO window registered");
            }
            BOOT_MARK("APU: done (%s)", g_apu_state ? "up" : "FAILED");
        } else {
            BOOT_MARK("APU: skipped (NFSU2_APU=0)");
        }
    } else {
        BOOT_MARK("APU: skipped (RECOMP_AC97_READY unset)");
    }

    /* The USB host controllers the title's XPP library drives to find its
     * gamepad (RECOMP_USB). On hosts without fault handling their registers
     * are reached through the MMIO accessors the XPP section is lifted with. */
    {
        extern void xbox_OhciInit(void);
        BOOT_MARK("OHCI: begin (xbox_OhciInit)");
        xbox_OhciInit();
        BOOT_MARK("OHCI: done");
    }

    /* The NIC the title's XNET library drives (src/nic.c). */
    {
        extern void nfsu2_nic_init(void);
        /* nic: NFSU2 only */
    }

#ifndef _WIN32
    /* The GPU renderer (NFSU2_GL=0 falls back to the executor's CPU one). */
    {
        const char *gl = getenv("NFSU2_GL");
        if (!gl || strcmp(gl, "0") != 0) {
            BOOT_MARK("GL/NV2A: begin (renderer install)");
#if defined(NFSU2_VULKAN)
            extern void nv2a_vk_install(void);
            nv2a_vk_install();
#else
            extern void nv2a_gl_install(void);
            nv2a_gl_install();
#endif
            BOOT_MARK("GL/NV2A: done");
        } else {
            BOOT_MARK("GL/NV2A: skipped (NFSU2_GL=0)");
        }
    }
#endif

    BOOT_MARK("xbox_kernel_init: begin");
    xbox_kernel_init();
    BOOT_MARK("xbox_kernel_init: done");
    /* text patch: NFSU2 only */
    BOOT_MARK("xbox_path_init: begin (game %s)", game_dir);
    xbox_path_init(game_dir, NFSU2_DEFAULT_SAVE_DIR);
    BOOT_MARK("xbox_path_init: done");
    BOOT_MARK("xbox_kernel_bridge_init: begin");
    xbox_kernel_bridge_init();
    BOOT_MARK("xbox_kernel_bridge_init: done");

    g_esp = XBOX_STACK_TOP;

    BOOT_MARK("recomp_dispatch_init: begin");
    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");
    BOOT_MARK("recomp_dispatch_init: done");

    BOOT_MARK("xbox_WatchdogStart: begin");
    xbox_WatchdogStart();
    BOOT_MARK("xbox_WatchdogStart: done");

    printf("Starting guest at 0x%08X (esp=0x%08X)\n", NFSU2_ENTRY_POINT, g_esp);
    BOOT_MARK("guest: all boot steps done; esp 0x%08X", g_esp);
    BOOT_MARK("gil: xbox_gil_mark_main");
    xbox_gil_mark_main();      /* the frame-rate thread (RECOMP_GIL_EAGER) */
    BOOT_MARK("gil: xbox_gil_enter");
    xbox_gil_enter();          /* guest code from here on (kernel_bridge.c) */
    BOOT_MARK("main thread: start guest entry 0x%08X", NFSU2_ENTRY_POINT);
    xbe_entry_point();
    BOOT_MARK("main thread: guest entry returned");
    BOOT_MARK("gil: xbox_gil_leave");
    xbox_gil_leave();

    printf("Guest returned. Cleaning up.\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);
    return 0;
}
