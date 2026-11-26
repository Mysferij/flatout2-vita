#include <zlib.h>
#include <SDL2/SDL.h>
/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>

/* The generated register model (g_eax/g_esp are thread-local there) and the
 * XBOX_PTR/MEM32 accessors; the register names below are its macros. */
#define RECOMP_GENERATED_CODE
#include "recomp_funcs.h"

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

extern ptrdiff_t g_xbox_mem_offset;

/* ── CRT memmove/memcpy ───────────────────────────────────────
 *
 * Two copies of the MSVC CRT memmove are linked (0x00218550 and 0x0021BFD0;
 * memcpy is the same body). Their tail and backward-copy paths dispatch
 * through UnwindDown-style vectors indexed with a *negative* register -- the
 * table address in the instruction is the end of the table -- which the
 * lifter's forward table reader cannot follow, so those jumps were lowered
 * to indirect tail calls into the middle of the function and failed to
 * resolve: every copy whose length was not a multiple of 4, and every
 * overlapping backward copy, silently lost its tail.
 *
 * Replaced by the host memmove. cdecl (dst, src, count), returns dst, and
 * esi/edi are preserved, as the original restores them. Generated bodies are
 * skipped by tools.recomp --exclude-manual reading this file. */
static void crt_memmove(void)
{
    uint32_t dst = MEM32(esp + 4);
    uint32_t src = MEM32(esp + 8);
    uint32_t n   = MEM32(esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), n);
    eax = dst;
    esp += 4;   /* return address; cdecl, the caller pops the arguments */
}

void sub_00218550(void) { crt_memmove(); }

/* Safe native replacement for zlib adler32() (sub_00211EC0): pure math
 * over guest buffer, touches no internal z_stream state. */
void sub_00211EC0(void)
{
    uint32_t adler = MEM32(g_esp + 4u);
    uint32_t buf_va = MEM32(g_esp + 8u);
    uint32_t len = MEM32(g_esp + 12u);
    const Bytef *buf = buf_va ? (const Bytef *)XBOX_PTR(buf_va) : Z_NULL;
    g_eax = (uint32_t)adler32((uLong)adler, buf, (uInt)len);
    g_esp += 4u;
}

/* Fast native replacements for MSVC CRT floating-point string parsers
 * (_atodbl, _atoflt, _fltin) that otherwise emulate 96-bit/80-bit x87
 * arithmetic digit-by-digit across thousands of config/Lua floats on LOADING. */
void sub_002219BC(void) /* _atodbl(double *out, const char *str) */
{
    uint32_t out_va = MEM32(g_esp + 4u);
    uint32_t str_va = MEM32(g_esp + 8u);
    const char *s = (const char *)XBOX_PTR(str_va);
    char *endp = NULL;
    double val = strtod(s, &endp);
    uint64_t bits = 0;
    memcpy(&bits, &val, sizeof(bits));
    MEM32(out_va) = (uint32_t)(bits & 0xFFFFFFFFull);
    MEM32(out_va + 4u) = (uint32_t)(bits >> 32);
    g_eax = 0u;
    g_esp += 4u;
}

void sub_002219E9(void) /* _atoflt(float *out, const char *str) */
{
    uint32_t out_va = MEM32(g_esp + 4u);
    uint32_t str_va = MEM32(g_esp + 8u);
    const char *s = (const char *)XBOX_PTR(str_va);
    char *endp = NULL;
    float val = strtof(s, &endp);
    uint32_t bits = 0;
    memcpy(&bits, &val, sizeof(bits));
    MEM32(out_va) = bits;
    g_eax = 0u;
    g_esp += 4u;
}

void sub_0021EDFC(void) /* _fltin(FLT *flt, const char *str, ...) */
{
    uint32_t flt_va = MEM32(g_esp + 4u);
    uint32_t str_va = MEM32(g_esp + 8u);
    const char *s = (const char *)XBOX_PTR(str_va);
    char *endp = NULL;
    double val = strtod(s, &endp);
    uint32_t nbytes = (endp && endp > s) ? (uint32_t)(endp - s) : 0u;
    uint64_t bits = 0;
    memcpy(&bits, &val, sizeof(bits));
    MEM32(flt_va + 0x00u) = 0u;
    MEM32(flt_va + 0x04u) = nbytes;
    MEM32(flt_va + 0x10u) = (uint32_t)(bits & 0xFFFFFFFFull);
    MEM32(flt_va + 0x14u) = (uint32_t)(bits >> 32);
    g_eax = flt_va;
    g_esp += 4u;
}
void sub_0021BFD0(void) { crt_memmove(); }

/* The title's own SSE memcpy (0x001C8380), cdecl (dst, src, count), chosen
 * by the CPU-feature dispatcher at 0x001C8560 and called through the pointer
 * at 0x0032DA78 (Lua chunk reads, file streams). It aligns the destination
 * with a computed jump into a run of movsb and copies 64 bytes a step with
 * movntps; the computed jumps cannot be lifted. Same contract as above. */
void sub_001C8380(void) { crt_memmove(); }

/* ── D3D fence wait, D3D_BlockOnTime (0x00240240) ────────────
 *
 * stdcall (fence, flags), ret 8. The XDK 5849 D3D device (NFSU2/Carbon's
 * code, byte for byte) is reached through the global at 0x0024C9D8:
 *   +0x2C  write sequence, bumped as fences are inserted
 *   +0x30  pointer to the semaphore the GPU releases as it completes work
 * and this spins until *(+0x30) catches up with the fence it was asked for.
 *
 * Without the pushbuffer executor nothing ever releases the semaphore, so the
 * GPU is reported caught up. With it (RECOMP_PB_EXEC) the real semaphore is
 * handed to the executor once and the original wait runs against it. */
#define FO2_D3D_DEVICE_PTR 0x0024C9D8u
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void sub_00240240_gen(void);
extern void sub_002D7308_gen(void);     /* XInitDevices: lifted body (non-Vita) */

/* RECOMP_LAT_TRACE: the fence the renderer put at the end of its last frame
 * (two slots at 0x42C3DC, next one written at index [0x42C3E4]). */
extern uint32_t (*recomp_lat_game_seq)(void);
static uint32_t fo2_fence_seq(void)
{
    return MEM32(0x42C3DCu + ((MEM32(0x42C3E4u) + 1u) & 1u) * 4u);
}

#include <unistd.h>
extern int xbox_gil_suspend(void);
extern void xbox_gil_resume(int depth);

void sub_00240240(void)
{
    static int exec = -1;
    uint32_t dev = MEM32(FO2_D3D_DEVICE_PTR);
    uint32_t sem = dev ? MEM32(dev + 0x30) : 0;

    if (exec < 0)
        exec = getenv("RECOMP_PB_EXEC") != NULL;
    if (exec) {
        static uint32_t registered;
        if (sem && sem != registered) {
            recomp_lat_game_seq = fo2_fence_seq;
            nv2a_pb_set_semaphore_target(sem);
            fprintf(stderr, "[D3D] GPU semaphore at 0x%08X\n", sem);
            registered = sem;
        }
        {
            /* Low latency (default; FO2_LOW_LATENCY=0 for the title's
             * own): the frame-start wait (0x1BEFD1 -> 0x23B890 -> here)
             * waits for the last frame's fence instead of the one before
             * it, so the title builds a frame only once the executor has
             * taken the previous one -- one frame less between reading the
             * pad and the screen (~60 ms at 18 fps on Linux), less overlap
             * of game and executor (race 20 -> 18 fps on lavapipe). */
            static int low = -1;
            if (low < 0) {
                const char *e = getenv("FO2_LOW_LATENCY");
                low = !(e && *e == '0');
                fprintf(stderr, "[D3D] low latency %s\n", low
                        ? "on: frame start waits on the last frame (FO2_LOW_LATENCY=0 off)"
                        : "off (title's two frames in flight)");
            }
            if (low && MEM32(esp) == 0x0023B89Cu && MEM32(esp + 12) == 0x001BEFD6u) {
                uint32_t last = fo2_fence_seq();
                if ((int32_t)(last - MEM32(esp + 4)) > 0)
                    MEM32(esp + 4) = last;
            }
        }
        {
            /* FO2_FENCE_STATS=1: who waits on fences, how often, how long. */
            static int st = -1;
            if (st < 0) { const char *e = getenv("FO2_FENCE_STATS"); st = e && *e == '1'; }
            if (st) {
                enum { N = 16 };
                static uint32_t ra[N], ra2[N]; static unsigned cnt[N]; static double ms[N];
                static uint64_t last; struct timespec t0, t1; int k;
                uint32_t r = MEM32(esp), r2 = r == 0x0023B89Cu ? MEM32(esp + 12) : 0;
                clock_gettime(CLOCK_MONOTONIC, &t0);
                sub_00240240_gen();
                clock_gettime(CLOCK_MONOTONIC, &t1);
                for (k = 0; k < N && ra[k] && (ra[k] != r || ra2[k] != r2); k++) ;
                if (k < N) { ra[k] = r; ra2[k] = r2; cnt[k]++;
                    ms[k] += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6; }
                if (!last) last = t1.tv_sec;
                if ((uint64_t)t1.tv_sec - last >= 10) {
                    last = t1.tv_sec;
                    for (k = 0; k < N && ra[k]; k++)
                        fprintf(stderr, "[fence] ret %08X/%08X: %u waits, %.0f ms\n", ra[k], ra2[k], cnt[k], ms[k]);
                    memset(cnt, 0, sizeof cnt); memset(ms, 0, sizeof ms);
                }
                return;
            }
        }
        if (sem && dev) {
            uint32_t want = MEM32(esp + 4u);
            uint32_t cur = MEM32(sem);
            if ((int32_t)(want - cur) > 0) {
                struct timespec ts0, ts1;
                clock_gettime(CLOCK_MONOTONIC, &ts0);
                (void)ts0; (void)ts1;
                if ((int32_t)(want - cur) > 2) {
                    int gd = xbox_gil_suspend();
                    usleep(1000);
                    xbox_gil_resume(gd);
                }
                if ((int32_t)(want - MEM32(sem)) > 0) {
                    MEM32(sem) = want;
                }
            }
#if defined(__vita__)
            eax = 0;
            esp += 4 + 8;
            return;
#endif
        }
        sub_00240240_gen();
        return;
    }
    if (sem)
        MEM32(sem) = MEM32(dev + 0x2C);     /* "GPU" is always caught up */
    eax = 0;
    esp += 4 + 8;                           /* return address, 2 stdcall args */
}


/* ── D3D interrupt handlers at DISPATCH_LEVEL ────────────────
 *
 * D3D services the GPU from its DPC (0x(DPC)) and, with the interrupt
 * masked, from its own busy-waits (BlockUntilIdle and friends) on the game
 * thread: both call the vblank handler (0x00242AF0) and the PGRAPH handler
 * (0x00243060), which queues flips. On the Xbox the two never interleave --
 * one is a DPC, the other runs with the interrupt off. Here the DPC runs on
 * the kernel timer thread, and lifted code hands over the guest lock at any
 * function entry, so the vblank count could move between D3D computing a
 * flip's target vblank and checking it. D3D retires a flip only when the
 * count *equals* its target, so that flip stayed pending forever and
 * PersistDisplay, which waits for pending flips, never returned: the hang
 * starting a Quick Race or Career. Run at DISPATCH_LEVEL (the dispatch lock,
 * kernel_hal.c), neither can be interrupted by the other.
 *
 * Raising blocks on the lock that the timer thread holds while it waits for
 * the guest lock, so the guest lock is let go for the raise, as a kernel
 * call would; already at DISPATCH (inside the DPC) there is nothing to take. */
extern uint8_t xbox_KfRaiseIrql(uint8_t irql);
extern void xbox_KfLowerIrql(uint8_t irql);
extern uint8_t xbox_CurrentIrql(void);
extern int xbox_gil_suspend(void);
extern void xbox_gil_resume(int depth);
extern void xbox_Nv2aVblankTaken(void);

static uint8_t d3d_isr_enter(void)
{
    uint8_t old = xbox_CurrentIrql();

    if (old < 2) {
        int d = xbox_gil_suspend();
        old = xbox_KfRaiseIrql(2);
        xbox_gil_resume(d);
    }
    return old;
}

static void d3d_isr_leave(uint8_t old)
{
    if (old < 2)
        xbox_KfLowerIrql(old);
}

/* Flips retired by a handler call: D3D's retire index, at +0x1BC of the
 * block both handlers take in ecx (their own reads index the pending-flip
 * slots from it). The pushbuffer executor's FLIP_STALL waits on these, as
 * PGRAPH waits on the retire's PGRAPH_INCREMENT write. */
extern void nv2a_pb_flip_retired(unsigned n);

/* Vblank handler, thiscall. It ends by writing PCRTC_INTR and spinning until
 * PMC_INTR bit 24 drops. Its callers (the DPC, D3D's busy-waits) have read
 * the bits before calling it, so this is the vblank being taken: clear them
 * now, and the spin ends at once instead of holding the guest lock until the
 * runtime's ack thread comes round. */
extern void sub_00242AF0_gen(void);

void sub_00242AF0(void)
{
    uint32_t blk = ecx;
    /* Read only once at DISPATCH: read before, a retire by the other
     * handler (DPC thread vs a D3D busy-wait) in between was counted by
     * both, the executor's flip index ran one off for good, and every
     * FLIP_STALL then waited out its 250 ms (Carbon, console, 3.7 fps). */
    uint8_t old = d3d_isr_enter();
    uint32_t head = MEM32(blk + 0x1BC);

    xbox_Nv2aVblankTaken();
    sub_00242AF0_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);
    d3d_isr_leave(old);
}

/* PGRAPH handler, thiscall, ecx = the device's hardware block (+0 =
 * 0xFD000000). Reads a software-method trap from PGRAPH_INTR / NSOURCE /
 * TRAPPED_ADDR / TRAPPED_DATA, acknowledges it by writing PGRAPH_INTR back,
 * and acts on it: NOP(5) sets the event BlockOnTime sleeps on, a flip trap
 * queues the flip (sub_00242DF0). The acknowledge is write-1-to-clear on
 * hardware and a no-op on RAM. Left pending, every turn of a busy-wait took
 * the same trap again (tens of thousands of flips a second, until the flip
 * targets were millions of vblanks ahead); guessed from the handler's
 * PGRAPH_FIFO writes, a trap posted during a call was sometimes dropped (the
 * fence event never set). So the executor posts traps under a lock held here
 * around the call, and a call that found one reports it taken, which clears
 * it (nv2a_pb_exec.c). */
extern void sub_00243060_gen(void);
extern void nv2a_pb_trap_lock(void);
/* The frame-rate cap (as NFSU2/Carbon: same D3D). Present posts the flip as
 * NOP trap data (vblanks << 5) | 1 -- software method 1 in the handler's
 * table at 0x243024, the flip queue 0x242DF0 adding (data >> 5) & 7 vblanks
 * to the last flip's target. FlatOut 2 asks for 2 (0x10580141): 30 fps at
 * most. Rewritten to 1 as the trap is taken, a frame ready in time shows on
 * the next vblank (60 fps at most); slower frames are unchanged.
 * FO2_FPS_CAP=30 keeps the title's interval. */
static int fps_cap_30(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("FO2_FPS_CAP");
        v = e && atoi(e) == 30;
        fprintf(stderr, "[fps] cap: %s\n", v ? "30 (title)" : "60 (FO2_FPS_CAP=30 for 30)");
    }
    return v;
}
extern void nv2a_pb_trap_unlock(void);
extern void nv2a_pb_trap_taken(void);

void sub_00243060(void)
{
    volatile uint32_t *nv = (volatile uint32_t *)XBOX_PTR(0xFD000000u);
    uint32_t blk = ecx;
    uint8_t old = d3d_isr_enter();
    uint32_t head = MEM32(blk + 0x1BC);     /* at DISPATCH, as above */
    uint32_t nsource;

    nv2a_pb_trap_lock();
    nsource = nv[0x400108 / 4];
    if (nsource && (nv[0x400704 / 4] & 0xFFFFu) == 0x0100u && !fps_cap_30()) {
        uint32_t data = nv[0x400708 / 4];
        if ((data & 0x1Fu) == 1u && ((data >> 5) & 7u) > 1u) {
            static int told;
            nv[0x400708 / 4] = (data & ~(7u << 5)) | (1u << 5);   /* flip: 1 vblank */
            if (!told++)
                fprintf(stderr, "[fps] flip interval %u -> 1 vblank (uncapped)\n", (data >> 5) & 7u);
        }
    }
    sub_00243060_gen();
    nv2a_pb_flip_retired(MEM32(blk + 0x1BC) - head);   /* a flip queued on its vblank retires at once */
    if (nsource)
        nv2a_pb_trap_taken();
    nv2a_pb_trap_unlock();
    d3d_isr_leave(old);
}

/* ── Float to integer, sub_00218FD4 (_ftol2) ──────────────────
 *
 * MSVC's CRT _ftol2: st(0) truncated to a 64-bit integer in edx:eax, popped.
 * Every (int)float cast in the title calls it; same body as Carbon's
 * 0x0032C47C (NFSU2's hottest main-thread function in a race, 4.4%).
 * It rounds with fistp
 * (the control word's mode), then corrects towards zero by the sign of the
 * float (x - r): x >= 0 and x - r < 0 -> r - 1; x < 0 (sign of (float)x)
 * and x - r > 0 -> r + 1. r == 0 or the integer indefinite is returned as
 * is. Exact, including ecx, the frame it publishes and the x87 top.
 * RECOMP_NATIVE=0 lifted, RECOMP_NATIVE_CHECK=1 both and compare. */
extern void sub_00218FD4_gen(void);

__attribute__((noinline, cold)) static int64_t ftol_fist_slow(double x, uint16_t cw)
{
    return recomp_fist(x, cw, 64);
}

static inline int64_t ftol_fist(double x, uint16_t cw)
{
    if (__builtin_expect(((cw >> 10) & 3u) == 0 && fabs(x) < 0x1p63, 1))
        return (int64_t)rint(x);        /* host rounding is never changed: nearest */
    return ftol_fist_slow(x, cw);       /* other modes, NaN, out of range */
}

static inline void ftol2_native(double x, uint32_t *peax, uint32_t *pedx, uint32_t *pecx)
{
    int64_t r = ftol_fist(x, g_fp_control_word);
    uint32_t lo = (uint32_t)r, hi = (uint32_t)((uint64_t)r >> 32), d;
    float fx = (float)x, fd;
    uint32_t sx;

    if (lo == 0 && (hi & 0x7FFFFFFFu) == 0) {   /* 0 or the indefinite */
        *peax = lo;
        *pedx = hi;
        return;
    }
    fd = (float)(x - (double)r);
    memcpy(&d, &fd, 4);
    memcpy(&sx, &fx, 4);
    if (sx & 0x80000000u) {
        d ^= 0x80000000u;
        r += d >= 0x80000001u;          /* carry of d + 0x7FFFFFFF */
    } else {
        r -= d >= 0x80000001u;          /* borrow */
    }
    *peax = (uint32_t)r;
    *pedx = (uint32_t)((uint64_t)r >> 32);
    *pecx = d + 0x7FFFFFFFu;
}

/* Modes other than plain native, out of line so that the hot path below is
 * small enough for LTO to inline into its lifted callers. */
static int s_ftol_mode = -1;            /* 0 lifted, 1 native, 2 native + check */

__attribute__((noinline, cold)) static void ftol2_slow(void)
{
    double x = g_fp_stack[g_fp_top & 7];
    uint32_t a, d, c = ecx, frame = esp - 4u;

    if (s_ftol_mode < 0) {
        const char *e = getenv("RECOMP_NATIVE"), *k = getenv("RECOMP_NATIVE_CHECK");
        s_ftol_mode = (e && *e == '0') ? 0 : (k && *k == '1') ? 2 : 1;
    }
    if (s_ftol_mode == 0) {
        sub_00218FD4_gen();
        return;
    }
    ftol2_native(x, &a, &d, &c);
    if (s_ftol_mode == 2) {
        static unsigned long calls, bad;
        int top = (g_fp_top + 1) & 7;
        uint32_t sp = esp + 4u;
        sub_00218FD4_gen();
        calls++;
        if ((eax != a || edx != d || ecx != c || g_fp_top != top || esp != sp
             || g_ebp != frame) && bad++ < 20)
            fprintf(stderr, "[native] sub_00218FD4 mismatch for %.17g: lifted %08X:%08X "
                    "ecx %08X, native %08X:%08X ecx %08X\n", x, edx, eax, ecx, d, a, c);
        if ((calls & 0xFFFFFF) == 0)
            fprintf(stderr, "[native] sub_00218FD4: %lu calls, %lu mismatches\n", calls, bad);
        return;
    }
    eax = a;
    edx = d;
    ecx = c;
    g_fp_top = (g_fp_top + 1) & 7;
    g_ebp = g_seh_ebp = frame;
    esp += 4;
}

inline void sub_00218FD4(void)
{
    uint32_t a, d, c, frame;
    int top;

    if (__builtin_expect(s_ftol_mode != 1, 0)) {
        ftol2_slow();                   /* first call, lifted or check mode */
        return;
    }
    top = g_fp_top;
    c = ecx;
    frame = esp - 4u;
    ftol2_native(g_fp_stack[top & 7], &a, &d, &c);
    eax = a;
    edx = d;
    ecx = c;
    g_fp_top = (top + 1) & 7;
    g_ebp = g_seh_ebp = frame;          /* as its push ebp / mov ebp, esp left them */
    esp += 4;                           /* return address */
}


/* ── XMV colour conversion, sub_002A5F31 ────────────────────
 *
 * The XMV player's YUV 4:2:0 -> 32-bit pixel converter for one 16x16 block,
 * hand-written MMX/SSE: stdcall, 7 arguments (Y, U, V, Y pitch, UV pitch,
 * destination, destination pitch), ret 28. The coefficients come in
 * registers the caller set up once per frame: mm4 (zero), mm5 (pmaddwd
 * weights), mm6 (chroma bias), mm7 (luma bias and the alpha byte, << 20),
 * xmm4-xmm7 (chroma weights). Eight pixels by two rows a step, left half
 * then right half, eight steps down.
 *
 * Half of the game thread on Linux during the intro and menu movies, and
 * the Switch ran those at 4 fps. Lifted, every op goes through the
 * thread-local guest registers, reloaded after each store since a guest
 * store may alias them. This is the same sequence of the same lane helpers
 * on locals, so it gives the same bytes; contraction is off so that a*b+c
 * is not fused into an FMA (which rounds once) on the Switch.
 * RECOMP_NATIVE=0 lifted, RECOMP_NATIVE_CHECK=1 both and compare. */
extern void sub_002A5F31_gen(void);

typedef struct {
    RecompMmx m4, m5, m7;
    RecompXmm x4, x5, x6, x7;
} XmvCoef;

/* cvtps2pi, psrad 15, packssdw, packuswb: clamp(round(f) >> 15, 0, 255).
 * Out of range cvtps2pi gives 0x80000000, which clamps to 0 (an ARM
 * conversion would saturate instead, hence the explicit check). */
static inline uint32_t xmv_u8(float f)
{
    int32_t i;
#if defined(__i386__) || defined(__x86_64__)
    i = _mm_cvtss_si32(_mm_set_ss(f));
#else
    if (!(f >= -2147483648.0f && f < 2147483648.0f))
        return 0;
    i = (int32_t)__builtin_rintf(f);    /* host rounding is never changed: nearest */
#endif
    i >>= 15;
    return i < 0 ? 0u : i > 255 ? 255u : (uint32_t)i;
}

static inline RecompMmx xmv_pack(float lo, float hi, RecompMmx m4)
{
    RecompMmx r;

    if (__builtin_expect(m4.q == 0, 1)) {   /* mm4 is the zero register here */
        r.ud[0] = xmv_u8(lo);
        r.ud[1] = xmv_u8(hi);
        return r;
    }
    r = MMX_FROM_PS(lo, hi, 0);
    r = MMX_PSRAD(r, 15u);
    r = MMX_PACKSSDW(r, m4);
    r = MMX_PACKUSWB(r, m4);
    r = MMX_PUNPCKLBW(r, m4);
    return MMX_PUNPCKLWD(r, m4);
}

/* loc_002CD998: four luma bytes in y -> four pixels in *o0, *o1. */
static inline void xmv_quad(const XmvCoef *k, RecompMmx y, RecompXmm x1, RecompXmm x2,
                            RecompXmm *x0, RecompXmm *x3, RecompMmx *o0, RecompMmx *o1)
{
    RecompMmx a, b, c;

    a = MMX_PUNPCKLBW(y, k->m4);
    a = MMX_PADDW(a, k->m7);
    b = MMX_PSHUFW(a, 250u);
    a = MMX_PSHUFW(a, 80u);
    a = MMX_PMADDWD(a, k->m5);
    b = MMX_PMADDWD(b, k->m5);
    *x0 = XMM_FROM_PI(*x0, a);
    *x3 = XMM_FROM_PI(*x3, b);
    *x0 = XMM_SHUFFLE(*x0, *x3, 0x44);
    *x3 = XMM_ADD(XMM_MUL(x1, k->x4), *x0);
    a = MMX_PSLLD(k->m7, 20u);
    a = MMX_POR(a, xmv_pack(x3->f[0], x3->f[1], k->m4));
    b = MMX_PSLLD(k->m7, 20u);
    *x3 = XMM_SHUFFLE(*x3, *x3, 0x2E);
    b = MMX_POR(b, xmv_pack(x3->f[0], x3->f[1], k->m4));
    *x3 = XMM_ADD(XMM_MUL(x2, k->x7), *x0);
    a = MMX_POR(a, MMX_PSLLQ(xmv_pack(x3->f[0], x3->f[1], k->m4), 16u));
    *x3 = XMM_SHUFFLE(*x3, *x3, 0x2E);
    b = MMX_POR(b, MMX_PSLLQ(xmv_pack(x3->f[0], x3->f[1], k->m4), 16u));
    *x3 = XMM_MUL(x2, k->x5);
    *x0 = XMM_ADD(*x0, *x3);
    *x3 = XMM_MUL(x1, k->x6);
    *x0 = XMM_ADD(*x0, *x3);
    c = MMX_PSLLQ(xmv_pack(x0->f[0], x0->f[1], k->m4), 8u);
    a = MMX_POR(a, c);
    *x0 = XMM_SHUFFLE(*x0, *x0, 0x2E);
    c = MMX_PSLLQ(xmv_pack(x0->f[0], x0->f[1], k->m4), 8u);
    b = MMX_POR(b, c);
    *o0 = a;
    *o1 = b;
}

static inline uint32_t xmv_ld32(uint32_t va)
{
    uint32_t v;
    memcpy(&v, (const void *)XBOX_PTR(va), 4);
    return v;
}

/* two biased chroma dwords -> [c0, c0, c1, c1] */
static inline RecompXmm xmv_chroma(RecompXmm x, RecompMmx c)
{
    x = XMM_FROM_PI(x, c);
    return XMM_SHUFFLE(x, x, 0x50);
}

__attribute__((optimize("fp-contract=off")))
static void xmv_yuv_native(void)
{
    uint32_t a = esp + 4;                       /* past the return address */
    uint32_t ys = MEM32(a), us = MEM32(a + 4), vs = MEM32(a + 8);
    uint32_t ypitch = MEM32(a + 12), uvpitch = MEM32(a + 16);
    uint32_t dst = MEM32(a + 20), dpitch = MEM32(a + 24);
    uint32_t n;
    /* The XMV player hands the movie surface in the tiled aperture
     * (0xF0000000 | physical). This is not lifted device-aware, and the
     * Switch build does not fold every access (RECOMP_FOLD_TILED=0), so the
     * addresses are folded onto the contiguous window here. */
#define FO2_FOLD(x) ((x) - 0xF0000000u < 0x08000000u ? (0x80000000u | ((x) & 0x03FFFFFFu)) : (x))
    dst = FO2_FOLD(dst); ys = FO2_FOLD(ys); us = FO2_FOLD(us); vs = FO2_FOLD(vs);
#undef FO2_FOLD
    XmvCoef k = { mm4, mm5, mm7, xmm4, xmm5, xmm6, xmm7 };
    RecompMmx m6 = mm6, r0 = mm0, r1 = mm1, r2 = mm2, r3 = mm3, u, v;
    RecompXmm x0 = xmm0, x1 = xmm1, x2 = xmm2, x3 = xmm3;

    for (n = 16;;) {
        u = MMX_PUNPCKLBW(MMX_FROM32(xmv_ld32(us)), k.m4);
        r3 = u;
        x1 = xmv_chroma(x1, MMX_PADDD(MMX_PUNPCKLWD(u, k.m4), m6));
        v = MMX_PUNPCKLBW(MMX_FROM32(xmv_ld32(vs)), k.m4);
        r1 = v;
        x2 = xmv_chroma(x2, MMX_PADDD(MMX_PUNPCKLWD(v, k.m4), m6));
        r3 = MMX_PUNPCKHWD(r3, r1);

        xmv_quad(&k, MMX_FROM32(xmv_ld32(ys)), x1, x2, &x0, &x3, &r0, &r1);
        MMX_STORE(dst, r0);
        MMX_STORE(dst + 8, r1);
        xmv_quad(&k, MMX_FROM32(xmv_ld32(ys + ypitch)), x1, x2, &x0, &x3, &r0, &r1);
        MMX_STORE(dst + dpitch, r0);
        MMX_STORE(dst + dpitch + 8, r1);

        r0 = MMX_PADDD(MMX_PSRLD(MMX_PSLLD(r3, 16u), 16u), m6);
        x1 = xmv_chroma(x1, r0);
        r3 = MMX_PADDD(MMX_PSRLD(r3, 16u), m6);
        x2 = xmv_chroma(x2, r3);

        xmv_quad(&k, MMX_FROM32(xmv_ld32(ys + 4)), x1, x2, &x0, &x3, &r0, &r1);
        MMX_STORE(dst + 0x10, r0);
        MMX_STORE(dst + 0x18, r1);
        xmv_quad(&k, MMX_FROM32(xmv_ld32(ys + ypitch + 4)), x1, x2, &x0, &x3, &r0, &r1);
        MMX_STORE(dst + dpitch + 0x10, r0);
        MMX_STORE(dst + dpitch + 0x18, r1);

        if (--n == 0)
            break;
        if (n & 1) {                            /* right half of the same rows */
            dst += 0x20; ys += 8; us += 4; vs += 4;
        } else {                                /* next two rows, back to the left */
            dst += dpitch * 2 - 32;
            ys += ypitch * 2 - 8;
            us += uvpitch - 4;
            vs += uvpitch - 4;
        }
    }
    /* r2 is only the packing scratch: the last value it held */
    r2 = xmv_pack(x0.f[0], x0.f[1], k.m4);
    r2 = MMX_PSLLQ(r2, 8u);
    mm0 = r0; mm1 = r1; mm2 = r2; mm3 = r3;
    xmm0 = x0; xmm1 = x1; xmm2 = x2; xmm3 = x3;
    eax = 0;
    ecx = us;
    edx = vs;
    g_ebp = g_seh_ebp = esp - 4;                /* as its push ebp / mov ebp, esp left them */
    esp += 4 + 28;                              /* return address, ret 28 */
}

void sub_002A5F31(void)
{
    static int mode = -1;                       /* 0 lifted, 1 native, 2 both + compare */

    if (__builtin_expect(mode < 0, 0)) {
        const char *e = getenv("RECOMP_NATIVE"), *k = getenv("RECOMP_NATIVE_CHECK");
        mode = (e && *e == '0') ? 0 : (k && *k == '1') ? 2 : 1;
    }
    if (mode == 1) {
        xmv_yuv_native();
        return;
    }
    if (mode == 0) {
        sub_002A5F31_gen();
        return;
    }
    {
        static unsigned long calls, bad;
        static uint8_t want[16][64];
        uint32_t sp = esp, dst = MEM32(esp + 4 + 20), dpitch = MEM32(esp + 4 + 24);
        uint32_t r_eax, r_ecx, r_edx, r_esp, row;
        uint64_t r_mm[4];
        RecompXmm r_x[4];

        sub_002A5F31_gen();
        for (row = 0; row < 16; row++)
            memcpy(want[row], (const void *)XBOX_PTR(dst + row * dpitch), 64);
        r_eax = eax; r_ecx = ecx; r_edx = edx; r_esp = esp;
        r_mm[0] = mm0.q; r_mm[1] = mm1.q; r_mm[2] = mm2.q; r_mm[3] = mm3.q;
        r_x[0] = xmm0; r_x[1] = xmm1; r_x[2] = xmm2; r_x[3] = xmm3;
        /* the lifted run clobbered the scratch registers the native one reads
         * as its starting values; only lanes neither reads before writing */
        esp = sp;
        ecx = MEM32(sp + 4 + 4);
        edx = MEM32(sp + 4 + 8);
        xmv_yuv_native();
        calls++;
        for (row = 0; row < 16; row++)
            if (memcmp(want[row], (const void *)XBOX_PTR(dst + row * dpitch), 64))
                break;
        if ((row < 16 || eax != r_eax || ecx != r_ecx || edx != r_edx || esp != r_esp
             || mm0.q != r_mm[0] || mm1.q != r_mm[1] || mm2.q != r_mm[2] || mm3.q != r_mm[3]
             || memcmp(&xmm0, &r_x[0], 16) || memcmp(&xmm1, &r_x[1], 16)
             || memcmp(&xmm2, &r_x[2], 16) || memcmp(&xmm3, &r_x[3], 16)) && bad++ < 20)
            fprintf(stderr, "[native] sub_002A5F31 mismatch at dst %08X: row %u, regs "
                    "eax %d ecx %d edx %d esp %d mm %d%d%d%d xmm %d%d%d%d\n", dst, row,
                    eax != r_eax, ecx != r_ecx, edx != r_edx, esp != r_esp,
                    mm0.q != r_mm[0], mm1.q != r_mm[1], mm2.q != r_mm[2], mm3.q != r_mm[3],
                    !!memcmp(&xmm0, &r_x[0], 16), !!memcmp(&xmm1, &r_x[1], 16),
                    !!memcmp(&xmm2, &r_x[2], 16), !!memcmp(&xmm3, &r_x[3], 16));
        if ((calls & 0xFFFF) == 0)
            fprintf(stderr, "[native] sub_002A5F31: %lu calls, %lu mismatches\n", calls, bad);
    }
}


/* ── DirectSound DSP command post (0x00252E89) ───────────────
 *
 * thiscall, ecx = the DSP-side object. Copies a command block into the GP
 * DSP's scratch area, stores the command code at scratch + 0x810, and spins
 * until the DSP zeroes it (this loop does re-read memory). The scratch block
 * is ***(this + 8 + 0x10), the same chain the runtime's RECOMP_DSP_ACK notes
 * describe. There is no DSP, so register the word with the runtime's
 * completion list before running the original body. */
extern int xbox_ApuDspAckWord(uint32_t va);
extern void sub_00252E89_gen(void);

void sub_00252E89(void)
{
    uint32_t obj = MEM32(ecx + 8);
    uint32_t blk = obj ? MEM32(obj + 0x10) : 0;
    uint32_t scratch = blk ? MEM32(blk) : 0;

    if (scratch)
        xbox_ApuDspAckWord(scratch + 0x810);
    sub_00252E89_gen();
}

/* ── DirectSound EP watchdog reset (0x0025360B) ──────────────
 *
 * thiscall, no arguments. FlatOut 2's DSOUND (the 5849 build Carbon has, flags 0x400B;
 * NFSU1/NFSU2 have no such check) ends its per-frame work (sub_0025384B) by
 * reading a word in the EP DSP's memory through the pointer at 0x0026CF50
 * (0xFE85A018); the EP firmware keeps 0xCCCCCC there. Anything else means
 * the DSP died, and it calls this: reset the whole APU (both AC'97
 * channels, a 10 ms stall). The runtime's DSPs are stubbed and the APU model
 * reads their memory as 0, so the title reset the APU every frame (~60/s)
 * and the intro movie, paced by the audio clock, froze on one frame.
 *
 * The DSP memory cannot just start holding writes: DSOUND's mailboxes there
 * "complete" today because they read back 0. So only the watchdog's call
 * (return address 0x002538EF) is skipped; DirectSound's own resets run. */
extern void sub_0025360B_gen(void);

void sub_0025360B(void)
{
    if (MEM32(esp) == 0x002538EFu) {
        esp += 4;                       /* return address; no arguments */
        return;
    }
    sub_0025360B_gen();
}

/* ── DirectSound AC'97 channel reset (0x002596A8) ────────────
 *
 * thiscall, ecx = channel object. Sets RR (bit 1) in the channel's NABM
 * control byte, then waits for the controller to clear it -- except MSVC
 * hoisted the load out of the loop, so the original reads the byte once and
 * spins on the register copy forever unless the bit is already clear at that
 * one read:
 *
 *     mov  byte [eax+0xFEC0010B], 2
 *     mov  cl, [eax+0xFEC0010B]
 *     and  cl, 2
 *   L: test cl, cl
 *     jne  L
 *
 * On hardware the reset has completed by then. The Windows runtime answers it
 * with a write trap on the NABM page; there is no fault handling on POSIX or
 * Switch, so this is the same function with the reset completing at once.
 * Everything else -- the lock taken through sub_0024EF10/sub_0024EF32, the
 * buffer-descriptor base store, the 0xFEC0017C write for channel 1 and the
 * sub_002594B2 re-arm -- is as the original does it. */
void sub_002596A8(void)
{
    uint32_t ebp_saved = g_ebp, frame, chan, nabm;

    PUSH32(esp, ebp_saved);
    frame = esp;
    g_ebp = frame;
    g_seh_ebp = frame;
    esp -= 8;
    MEM32(frame - 4) = 0;
    PUSH32(esp, esi);
    esi = ecx;
    chan = esi;

    ecx = frame - 8;
    PUSH32(esp, 0x002596BCu); RECOMP_ABI_CALL(0x0024EF10u, sub_0024EF10);

    nabm = MEM32(MEM32(chan) * 4 + 0x0026C8F8u);
    MEM8(nabm + 0xFEC0010Bu) = 2;
    MEM8(nabm + 0xFEC0010Bu) &= (uint8_t)~2u;   /* reset complete */
    eax = nabm;
    SET_LO8(ecx, 0);

    MEM32(nabm + 0xFEC00100u) = MEM32(chan + 0x1C);
    if (MEM32(chan) == 1)
        MEM32(0xFEC0017Cu) = MEM32(chan + 0x28);

    PUSH32(esp, 1);
    PUSH32(esp, 1);
    ecx = chan;
    PUSH32(esp, MEM8(chan + 0x24));
    PUSH32(esp, MEM8(chan + 0x25));
    eax = MEM8(chan + 0x25);
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x0025970Fu); RECOMP_ABI_CALL(0x002594B2u, sub_002594B2);

    ecx = frame - 8;
    g_ebp = frame;
    g_seh_ebp = frame;
    PUSH32(esp, 0x00259717u); RECOMP_ABI_CALL(0x0024EF32u, sub_0024EF32);

    POP32(esp, esi);
    esp = frame;
    POP32(esp, ebp_saved);
    g_ebp = ebp_saved;
    esp += 4;
}










/* ── Movie stream "primed" check (0x000E01D0) ─────────────────
 *
 * thiscall, no arguments. The XMV file stream keeps a ring of six 256 KB
 * overlapped reads (slot i at this + 0x148 + 12i: state 0 free, 1 reading,
 * 2 full; current slot at this + 0x13C). This polls the ring (sub_000DFFC0:
 * a finished read becomes full and the cursor moves on, a free slot gets its
 * read issued) and answers "primed" when the cursor is on the last slot.
 * Movie start (sub_000E0280) spins on it with Sleep(1).
 *
 * From a DVD the last read is still in flight when the cursor gets there.
 * The runtime completes reads at once, so the poll that finishes slot 5 also
 * wraps the cursor to slot 0, which is full and waits to be consumed: the
 * answer stays no and the boot hung before the Dolby logo. A ring that is
 * full everywhere is primed too. */
extern void sub_000E01D0_gen(void);
#if defined(__vita__)
void vita_log(const char *fmt, ...);
#endif

void sub_000E01D0(void)
{
    uint32_t self = ecx;
    int i, full = 1;

    sub_000E01D0_gen();
    if (eax)
        return;
    for (i = 0; i < 6; i++)
        if (MEM32(self + 0x148u + 12u * i) != 2)
            full = 0;
    eax = full;
#if defined(__vita__)
    {   /* ring state: first 40 polls, then every 500th (fo2x_log.txt) */
        static unsigned s_polls;
        if (s_polls < 40u || (s_polls % 500u) == 0u) {
            vita_log("[xmv] ring poll %u: cursor=%u states=%u,%u,%u,%u,%u,%u "
                     "iosb=0x%08X pos=0x%08X -> primed %d",
                     s_polls, (unsigned)MEM32(self + 0x13Cu),
                     (unsigned)MEM32(self + 0x148u), (unsigned)MEM32(self + 0x154u),
                     (unsigned)MEM32(self + 0x160u), (unsigned)MEM32(self + 0x16Cu),
                     (unsigned)MEM32(self + 0x178u), (unsigned)MEM32(self + 0x184u),
                     (unsigned)MEM32(self + 0x10Cu), (unsigned)MEM32(self + 0x12Cu),
                     (int)eax);
        }
        s_polls++;
    }
#endif
}

/* ── XMV stream consumer (0x000E0090) probe ──────────────────────────
 *
 * stdcall-ish: ecx = the ring object, arg1 = dest, arg2 = count; returns
 * the bytes copied in eax. The movie decoder calls it through a vtable.
 * It copies from the slot at this+0x138 once that slot is full, and after
 * the chunk is used up frees the slot (state 0) and advances to the next.
 * DFFC0 only issues a new read for a free slot, so a missing NtReadFile
 * after primed means this function never got past the chunk.
 *
 * Logs the first 60 calls, then every 1000th, plus the first 20 calls that
 * return 0 bytes for a non-zero request. Vita only. */
extern void sub_000E0090_gen(void);
void sub_000E0090(void)
{
#if defined(__vita__)
    uint32_t self  = ecx;
    uint32_t dest  = MEM32(g_esp + 4u);
    uint32_t count = MEM32(g_esp + 8u);
    uint32_t cur   = MEM32(self + 0x138u) % 6u;
    uint32_t cst   = MEM32(self + 0x148u + 12u * cur);
    uint32_t pos   = MEM32(self + 0x128u);
    uint32_t fsize = MEM32(self + 0x130u);
    uint32_t chunk = MEM32(self + 0x124u);
    static unsigned s_calls, s_zero;
    unsigned n = s_calls++;
#endif

    sub_000E0090_gen();

#if defined(__vita__)
    if (n < 60u || (n % 1000u) == 0u || (eax == 0u && count != 0u && s_zero < 20u)) {
        if (eax == 0u && count != 0u) s_zero++;
        vita_log("[xmv] consume #%u: dest=0x%08X count=%u pos=0x%08X fsize=0x%08X "
                 "chunk=0x%X cur=%u state=%u -> %u",
                 n, (unsigned)dest, (unsigned)count, (unsigned)pos,
                 (unsigned)fsize, (unsigned)chunk, (unsigned)cur, (unsigned)cst,
                 (unsigned)eax);
    }
#endif
}



/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

#if defined(__vita__)
    /* Vita: XAPI controller hooks (see the end of this file). The USB/OHCI
     * path cannot poll without a kernel scheduler, so the game's XInput calls
     * are answered here directly. Other platforms keep the lifted code. */
    if (xbox_va == 0x002D7349u) return (recomp_func_t)sub_002D7349;
    if (xbox_va == 0x002D736Bu) return (recomp_func_t)sub_002D736B;
    if (xbox_va == 0x002DD9BDu) return (recomp_func_t)sub_002DD9BD;
    if (xbox_va == 0x002DDA13u) return (recomp_func_t)sub_002DDA13;
    if (xbox_va == 0x002DDA1Fu) return (recomp_func_t)sub_002DDA1F;
#endif
    (void)xbox_va;
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}
extern void sub_000DFDC0_gen(void); void sub_000DFDC0(void) { sub_000DFDC0_gen(); }
extern void sub_000E0280_gen(void);
void sub_000E0280(void)
{
#if defined(__vita__)
    RECOMP_RR_DECL
    g_eax = 0u;
    g_esp += 4u;
    return;
#else
    sub_000E0280_gen();
#endif
}
extern void sub_000E0460_gen(void); void sub_000E0460(void) { sub_000E0460_gen(); }
extern void sub_000E05F0_gen(void); void sub_000E05F0(void) { sub_000E05F0_gen(); }
extern void sub_000E0730_gen(void); void sub_000E0730(void) { sub_000E0730_gen(); }
extern void sub_000E0CB0_gen(void); void sub_000E0CB0(void) { sub_000E0CB0_gen(); }
extern void sub_000E0D80_gen(void); void sub_000E0D80(void) { sub_000E0D80_gen(); }
extern void sub_0012E2D0_gen(void); void sub_0012E2D0(void) { sub_0012E2D0_gen(); }
extern void sub_002D8417_gen(void); void sub_002D8417(void) { sub_002D8417_gen(); }
extern void sub_002D63C4_gen(void); void sub_002D63C4(void) { sub_002D63C4_gen(); }
extern void sub_002D65CF_gen(void); void sub_002D65CF(void) { sub_002D65CF_gen(); }
extern void sub_002D6648_gen(void); void sub_002D6648(void) { sub_002D6648_gen(); }
extern void sub_002D690A_gen(void); void sub_002D690A(void) { sub_002D690A_gen(); }
extern void sub_002D6A53_gen(void); void sub_002D6A53(void) { sub_002D6A53_gen(); }
extern void sub_002D6AF6_gen(void); void sub_002D6AF6(void) { sub_002D6AF6_gen(); }
extern void sub_002D6B02_gen(void); void sub_002D6B02(void) { sub_002D6B02_gen(); }
extern void sub_002D6CBF_gen(void); void sub_002D6CBF(void) { sub_002D6CBF_gen(); }
extern void sub_002D70D5_gen(void); void sub_002D70D5(void) { sub_002D70D5_gen(); }
extern void sub_002D7178_gen(void); void sub_002D7178(void) { sub_002D7178_gen(); }
extern void sub_002D7733_gen(void); void sub_002D7733(void) { sub_002D7733_gen(); }
extern void sub_002DD568_gen(void); void sub_002DD568(void) { sub_002DD568_gen(); }

#if defined(__vita__)
/* ── PS Vita XInput XAPI overrides ───────────────────────────────────────
 *
 * The XDK XInputGetState(HANDLE hDevice, PXINPUT_STATE pState) (lifted
 * sub_002DDA1F, recomp_0046.c:10273) does this on success:
 *   - zeroes pState[0..24];
 *   - pState[0] = a byte of the device object (not reproduced here);
 *   - DMAs the 20-byte XID input report into pState+1 .. pState+20.
 * The report layout is the one usb_gamepad.c builds (the Linux path that
 * works): report[0]=0 id, [1]=20 length, [2..3] digital buttons LE,
 * [4..11] analog A B X Y Black White LTrig RTrig, [12..19] four signed
 * LE stick axes LX LY RX RY.
 *
 * sub_002D736B (XGetDeviceChanges, per frame) refreshes the report at the
 * last pState. */

#include <psp2/ctrl.h>
#include <psp2/power.h>

void vita_log(const char *fmt, ...);

static uint32_t g_fo2_pstate;   /* last pState handed to XInputGetState */

static int16_t fo2_vita_axis(unsigned char raw, int invert)
{
    int v = ((int)raw - 128) * 256;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    if (v > -3500 && v < 3500) v = 0;
    if (invert) v = -1 - v;
    return (int16_t)v;
}

volatile uint32_t g_vita_sdl_buttons = 0u;
volatile int16_t  g_vita_sdl_axes[4] = { 0, 0, 0, 0 };

static void fo2_vita_report(uint8_t out[20])
{
    static int sampling_set;
    static uint16_t last_btn = 0xFFFFu;
    static SceCtrlData pad __attribute__((aligned(64)));
    uint16_t dig = 0;
    int i;

    /* Clocks and stderr buffering are set at boot (vita_boot, src/vita.c). */
    if (!sampling_set) {
        int rc_m1 = sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
        int rc_m2 = sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
        fprintf(stderr, "[VITA_PAD] SamplingMode -> %d, Ext -> %d\n", rc_m1, rc_m2);
        fflush(stderr);
        sampling_set = 1;
    }
    static SceCtrlData s_buf[64] __attribute__((aligned(64)));
    memset(&pad, 0, sizeof pad);
    pad.lx = pad.ly = pad.rx = pad.ry = 128;
    int rc_ctrl = sceCtrlPeekBufferPositive(0, s_buf, 64);
    if (rc_ctrl > 0) {
        int cnt = rc_ctrl > 64 ? 64 : rc_ctrl;
        for (int k = 0; k < cnt; k++) pad.buttons |= s_buf[k].buttons;
        pad.lx = s_buf[cnt - 1].lx;
        pad.ly = s_buf[cnt - 1].ly;
        pad.rx = s_buf[cnt - 1].rx;
        pad.ry = s_buf[cnt - 1].ry;
    }
    int rc_ext = sceCtrlPeekBufferPositive2(1, s_buf, 64);
    if (rc_ext > 0) {
        int cnt = rc_ext > 64 ? 64 : rc_ext;
        for (int k = 0; k < cnt; k++) pad.buttons |= s_buf[k].buttons;
    }
    pad.buttons |= g_vita_sdl_buttons;

    memset(out, 0, 20);
    out[1] = 20;

    if (pad.buttons & SCE_CTRL_UP)     dig |= 0x0001;
    if (pad.buttons & SCE_CTRL_DOWN)   dig |= 0x0002;
    if (pad.buttons & SCE_CTRL_LEFT)   dig |= 0x0004;
    if (pad.buttons & SCE_CTRL_RIGHT)  dig |= 0x0008;
    if (pad.buttons & SCE_CTRL_START)  dig |= 0x0010;
    if (pad.buttons & SCE_CTRL_SELECT) dig |= 0x0020;
    /* XINPUT_GAMEPAD digital bits for the face buttons: a title that reads
     * wButtons (movie skip, menus) must see Cross as XINPUT_GAMEPAD_A
     * (0x1000) as well as in the analog byte below. */
    if (pad.buttons & SCE_CTRL_CROSS)    dig |= 0x1000u;   /* A */
    if (pad.buttons & SCE_CTRL_CIRCLE)   dig |= 0x2000u;   /* B */
    if (pad.buttons & SCE_CTRL_SQUARE)   dig |= 0x4000u;   /* X */
    if (pad.buttons & SCE_CTRL_TRIANGLE) dig |= 0x8000u;   /* Y */
    out[2] = (uint8_t)(dig & 0xFFu);
    out[3] = (uint8_t)(dig >> 8);

    out[4 + 0] = (pad.buttons & SCE_CTRL_CROSS)    ? 255 : 0;   /* A */
    out[4 + 1] = (pad.buttons & SCE_CTRL_CIRCLE)   ? 255 : 0;   /* B */
    out[4 + 2] = (pad.buttons & SCE_CTRL_SQUARE)   ? 255 : 0;   /* X */
    out[4 + 3] = (pad.buttons & SCE_CTRL_TRIANGLE) ? 255 : 0;   /* Y */
    out[4 + 4] = 0;                                              /* Black */
    out[4 + 5] = 0;                                              /* White */
    out[4 + 6] = (pad.buttons & SCE_CTRL_LTRIGGER) ? 255 : 0;   /* LTrig */
    out[4 + 7] = (pad.buttons & SCE_CTRL_RTRIGGER) ? 255 : 0;   /* RTrig */

    {
        int16_t axes[4];
        axes[0] = fo2_vita_axis(pad.lx, 0);
        axes[1] = fo2_vita_axis(pad.ly, 1);
        axes[2] = fo2_vita_axis(pad.rx, 0);
        axes[3] = fo2_vita_axis(pad.ry, 1);
        for (i = 0; i < 4; i++) {
            out[12 + 2 * i]     = (uint8_t)((uint16_t)axes[i] & 0xFFu);
            out[12 + 2 * i + 1] = (uint8_t)((uint16_t)axes[i] >> 8);
        }
    }

    /* One line per change, so a Start press shows up as buttons=0x0010. */
    {
        static unsigned s_rep_n = 0;
        s_rep_n++;
        if (s_rep_n == 10u || (s_rep_n % 120u) == 0u) {
            fprintf(stderr, "[VITA_PAD] rep #%u rc=%d raw1=0x%08X dig=0x%04X\n",
                    s_rep_n, rc_ctrl, (unsigned)pad.buttons, (unsigned)dig);
            fflush(stderr);
        }
    }
    if (dig != last_btn) {
        last_btn = dig;
        vita_log("[VITA_XINPUT] buttons=0x%04X%s", (unsigned)dig,
                 (dig & 0x0010u) ? " START" : "");
    }
}

/* Write the report into guest memory at pState+1, byte by byte: the
 * XInput buffer is not aligned for word stores. */
static void fo2_vita_put_report(uint32_t pstate)
{
    static uint8_t s_pkt = 1u;
    uint8_t rep[20];
    int i;
    fo2_vita_report(rep);
    MEM8(pstate) = s_pkt;
    s_pkt = (uint8_t)(s_pkt + 1u);
    if (s_pkt == 0u) s_pkt = 1u;
    for (i = 0; i < 20; i++)
        MEM8(pstate + 1u + (uint32_t)i) = rep[i];
}

extern void sub_001EB940_gen(void);
void sub_001EB940(void)
{
#if defined(__vita__)
    RECOMP_RR_DECL
    static unsigned s_in940 = 0;
    uint32_t self = g_ecx;
    s_in940++;
    if (s_in940 == 1u || s_in940 == 60u) {
        fprintf(stderr, "[VITA_XINPUT] sub_001EB940 #%u self=0x%08X port=%u hdev=0x%08X mask=0x%08X\n",
                s_in940, (unsigned)self, (unsigned)MEM32(self + 0x140u),
                (unsigned)MEM32(self + 0x99Cu), (unsigned)MEM32(g_esp + 8u));
    }
    if (self >= 0x00100000u && self < 0x04000000u && MEM32(self + 0x140u) < 4u) {
        uint32_t port = MEM32(self + 0x140u);
        MEM32(self + 0x99Cu) = 0u;
        MEM32(g_esp + 4u) |= (1u << port);
    }
#endif
    sub_001EB940_gen();
}

static uint16_t g_vita_cur_btn = 0u;
static uint16_t g_vita_edge_btn = 0u;

static void fo2_vita_sync_pad_slots(uint32_t self, uint32_t arg_idx)
{
#if defined(__vita__)
    (void)arg_idx;
    if (self >= 0x00100000u && self < 0x04000000u && MEM32(self + 0x140u) < 4u) {
        uint8_t rep[20];
        fo2_vita_report(rep);
        uint16_t wbtn = (uint16_t)rep[2] | ((uint16_t)rep[3] << 8);
        uint16_t cur = 0u;
        if (rep[4] > 0x1Eu) cur |= (1u << 0);  /* 0: A / Cross */
        if (rep[5] > 0x1Eu) cur |= (1u << 1);  /* 1: B / Circle */
        if (rep[6] > 0x1Eu) cur |= (1u << 2);  /* 2: X / Square */
        if (rep[7] > 0x1Eu) cur |= (1u << 3);  /* 3: Y / Triangle */
        if (wbtn & 0x0010u) cur |= (1u << 8);  /* 8: Start */
        if (wbtn & 0x0020u) cur |= (1u << 9);  /* 9: Select */
        if (wbtn & 0x0001u) cur |= (1u << 10); /* 10: Up */
        if (wbtn & 0x0002u) cur |= (1u << 11); /* 11: Down */
        if (wbtn & 0x0004u) cur |= (1u << 12); /* 12: Left */
        if (wbtn & 0x0008u) cur |= (1u << 13); /* 13: Right */
        uint16_t new_edge = (uint16_t)(cur & ~g_vita_cur_btn);
        if (new_edge) g_vita_edge_btn |= new_edge;
        g_vita_cur_btn = cur;

        MEM8(self + 0x130u) = 1u;
        for (uint32_t act = 0u; act < 20u; act++) {
            if (MEM8(self + 0x78Cu + act) == 0xFFu)
                MEM8(self + 0x78Cu + act) = (uint8_t)act;
        }
        for (uint32_t idx = 0u; idx < 20u; idx++) {
            uint32_t base = self + idx * 20u + 0x7ECu;
            uint32_t bit = (idx < 14u) ? (1u << idx) : ((1u << 0) | (1u << 8));
            if (g_vita_edge_btn & bit) MEM8(base) = 1u;
            MEM32(base + 4u) = (cur & bit) ? 1u : 0u;
        }
        if (cur & ((1u << 8) | (1u << 0))) {
            MEM32(self + 0x7B4u) = 1u;
            MEM32(self + 0x7C4u) = 1u;
            MEM32(self + 0x7B8u) = 1u;
            MEM32(self + 0x7C8u) = 1u;
        }
    }
#else
    (void)self; (void)arg_idx;
#endif
}

static uint32_t s_ctrl_objs[4] = { 0u, 0u, 0u, 0u };
static void fo2_vita_register_ctrl(uint32_t self)
{
    if (self >= 0x00100000u && self < 0x04000000u) {
        uint32_t port = MEM32(self + 0x140u);
        if (port < 4u) s_ctrl_objs[port] = self;
    }
}

/* The loading screen's state (Vita): the thread that waits for the bar to
 * finish sleeps until *(*0x330980 + 4) == 13 (0x232520). Each frame, log the
 * value when it changes, and the byte at +0x1524 that thread 4 polls, so a
 * run shows whether the state moves after the bar reaches 100% and which
 * value it stops on. Change-only, capped: a few dozen lines per run. */
void fo2_vita_state_probe(void)
{
#if defined(__vita__)
    static uint32_t last_state = 0xFFFFFFFFu, last_flag = 0xFFFFFFFFu;
    static unsigned frames, lines;
    uint32_t obj, st, flag;

    frames++;
    if (lines >= 80u)
        return;
    obj = MEM32(0x00330980u);
    if (obj < 0x00010000u || obj >= 0x04000000u) {
        if (last_state != 0xFFFFFFFEu) {
            last_state = 0xFFFFFFFEu;
            lines++;
            fprintf(stderr, "[state] frame %u: state object not set (0x%08X)\n",
                    frames, (unsigned)obj);
        }
        return;
    }
    st = MEM32(obj + 4u);
    flag = MEM8(obj + 0x1524u);
    if (st != last_state || flag != last_flag) {
        last_state = st;
        last_flag = flag;
        lines++;
        fprintf(stderr, "[state] frame %u: obj 0x%08X +4=%u +0x1524=%u\n",
                frames, (unsigned)obj, (unsigned)st, (unsigned)flag);
    }
#else
    (void)0;
#endif
}

void fo2_vita_frame_tick(void)
{
#if defined(__vita__)
    static unsigned s_ftick = 0u;
    s_ftick++;
    static uint16_t s_prev_frame_btn = 0u;
    uint8_t rep[20];
    fo2_vita_report(rep);
    uint16_t wbtn = (uint16_t)rep[2] | ((uint16_t)rep[3] << 8);
    uint16_t cur = 0u;
    if (rep[4] > 0x1Eu) cur |= (1u << 0);  /* 0: A / Cross */
    if (rep[5] > 0x1Eu) cur |= (1u << 1);  /* 1: B / Circle */
    if (rep[6] > 0x1Eu) cur |= (1u << 2);  /* 2: X / Square */
    if (rep[7] > 0x1Eu) cur |= (1u << 3);  /* 3: Y / Triangle */
    if (wbtn & 0x0010u) cur |= (1u << 8);  /* 8: Start */
    if (wbtn & 0x0020u) cur |= (1u << 9);  /* 9: Select */
    if (wbtn & 0x0001u) cur |= (1u << 10); /* 10: Up */
    if (wbtn & 0x0002u) cur |= (1u << 11); /* 11: Down */
    if (wbtn & 0x0004u) cur |= (1u << 12); /* 12: Left */
    if (wbtn & 0x0008u) cur |= (1u << 13); /* 13: Right */
    if (cur != s_prev_frame_btn) {
        fprintf(stderr, "[VITA_FRAME_PAD] cur=0x%04X (ctrl0=0x%08X)\n", (unsigned)cur, (unsigned)s_ctrl_objs[0]);
        if (cur != 0u) {
            fprintf(stderr, "[VITA_STACK]");
            int printed = 0;
            for (uint32_t sp = 0x01CA0D00u; sp < 0x01CA1200u && printed < 16; sp += 4u) {
                uint32_t val = MEM32(sp);
                if (val >= 0x00011000u && val < 0x00310000u) {
                    fprintf(stderr, " %08X", (unsigned)val);
                    printed++;
                }
            }
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
    uint16_t edge = (uint16_t)(cur & ~s_prev_frame_btn);
    s_prev_frame_btn = cur;

    if (s_ctrl_objs[0] == 0u) {
        for (uint32_t va = 0x01400000u; va < 0x01700000u; va += 4u) {
            if (MEM32(va) == 0x0031B1B0u) {
                uint32_t port = MEM32(va + 0x140u);
                if (port < 4u) s_ctrl_objs[port] = va;
            }
        }
    }
    for (uint32_t p = 0u; p < 4u; p++) {
        uint32_t self = s_ctrl_objs[p];
        if (self < 0x00100000u || self >= 0x04000000u) continue;
        MEM32(self + 0x12Cu) = 1u;
        MEM8(self + 0x130u) = 1u;
        MEM32(self + 0x9B2u) = MEM32(self + 0x9B2u) + 1u;
        for (uint32_t b = 0u; b < 16u; b++)
            MEM8(self + 0x9B6u + b) = rep[2u + b];
        for (uint32_t act = 0u; act < 20u; act++) {
            if (MEM8(self + 0x78Cu + act) == 0xFFu)
                MEM8(self + 0x78Cu + act) = (uint8_t)act;
        }
        for (uint32_t idx = 0u; idx < 20u; idx++) {
            uint32_t base = self + idx * 20u + 0x7ECu;
            uint32_t bit = (idx < 14u) ? (1u << idx) : ((1u << 0) | (1u << 8));
            if (edge & bit) MEM8(base) = 1u;
            MEM32(base + 4u) = (cur & bit) ? 1u : 0u;
        }
        if (cur & ((1u << 8) | (1u << 0))) {
            MEM32(self + 0x7B4u) = 1u;
            MEM32(self + 0x7C4u) = 1u;
            MEM32(self + 0x7B8u) = 1u;
            MEM32(self + 0x7C8u) = 1u;
        }
    }
    g_vita_edge_btn = edge;
    g_vita_cur_btn = cur;
#endif
}

extern void sub_001AF170_gen(void);
void sub_001AF170(void)
{
#if defined(__vita__)
    RECOMP_RR_DECL
    static unsigned s_txt_cnt = 0u;
    static uint32_t s_seen[64];
    uint32_t cstr_obj = g_ecx;
    if (s_txt_cnt < 50u && cstr_obj >= 0x00100000u && cstr_obj < 0x04000000u) {
        uint32_t ptr = MEM32(cstr_obj);
        if (ptr >= 0x00100000u && ptr < 0x04000000u) {
            int dup = 0;
            for (unsigned i = 0u; i < s_txt_cnt; i++) if (s_seen[i] == ptr) dup = 1;
            if (!dup) {
                s_seen[s_txt_cnt++] = ptr;
                char buf[96];
                unsigned j = 0u;
                for (unsigned k = 0u; k < 90u; k += 2u) {
                    uint8_t c0 = MEM8(ptr + k);
                    uint8_t c1 = MEM8(ptr + k + 1u);
                    if (c0 == 0u && c1 == 0u) break;
                    if (c1 == 0u && c0 >= 32u && c0 < 127u) buf[j++] = (char)c0;
                    else if (c0 >= 32u && c0 < 127u) { buf[j++] = (char)c0; if (c1 >= 32u && c1 < 127u) buf[j++] = (char)c1; }
                }
                buf[j] = '\0';
                if (j > 0u) {
                    fprintf(stderr, "[VITA_TEXT] #%u @0x%08X: %s\n", s_txt_cnt, (unsigned)ptr, buf);
                    fflush(stderr);
                }
            }
        }
    }
#endif
    sub_001AF170_gen();
}

extern void sub_001EBA20_gen(void);
void sub_001EBA20(void)
{
    RECOMP_RR_DECL
    uint32_t self = g_ecx, arg = MEM32(g_esp + 4u);
    fo2_vita_sync_pad_slots(self, arg);
    sub_001EBA20_gen();
    static unsigned s_log_a20 = 0;
    if (g_vita_cur_btn && s_log_a20 < 25u) {
        s_log_a20++;
        fprintf(stderr, "[MENU_PAD] 1EBA20 self=0x%08X arg=%u cur=0x%04X -> eax=0x%02X\n",
                (unsigned)self, (unsigned)arg, (unsigned)g_vita_cur_btn, (unsigned)(g_eax & 0xFFu));
    }
}

extern void sub_001EBB70_gen(void);
void sub_001EBB70(void)
{
    RECOMP_RR_DECL
    fo2_vita_sync_pad_slots(g_ecx, MEM32(g_esp + 4u));
    sub_001EBB70_gen();
}

extern void sub_001EBBF0_gen(void);
void sub_001EBBF0(void)
{
    RECOMP_RR_DECL
    fo2_vita_sync_pad_slots(g_ecx, MEM32(g_esp + 4u));
    sub_001EBBF0_gen();
}

extern void sub_001EBC20_gen(void);
void sub_001EBC20(void)
{
    RECOMP_RR_DECL
    fo2_vita_sync_pad_slots(g_ecx, MEM32(g_esp + 4u));
    sub_001EBC20_gen();
}

extern void sub_001EBF70_gen(void);
void sub_001EBF70(void)
{
    RECOMP_RR_DECL
    uint32_t self = g_ecx;
    if (self >= 0x10000u && self < 0x04000000u) {
        MEM8(self + 0x130u) = 1u;
        if (MEM32(self + 0x99Cu) == 0u)
            MEM32(self + 0x99Cu) = 0x1001u;
        fo2_vita_sync_pad_slots(self, 0u);
    }
    sub_001EBF70_gen();
}

extern void sub_001EC1B0_gen(void);
void sub_001EC1B0(void)
{
#if defined(__vita__)
    RECOMP_RR_DECL
    fo2_vita_register_ctrl(g_ecx);
    static unsigned s_c1b0 = 0;
    uint32_t self = g_ecx;
    s_c1b0++;
    if (s_c1b0 == 1u || s_c1b0 == 60u) {
        fprintf(stderr, "[VITA_PAD] sub_001EC1B0 #%u self=0x%08X\n", s_c1b0, (unsigned)self);
        fflush(stderr);
    }
    if (self >= 0x00100000u && self < 0x04000000u && MEM32(self + 0x140u) == 0u) {
        MEM8(self + 0x130u) = 1u;
        if (MEM32(self + 0x99Cu) == 0u)
            MEM32(self + 0x99Cu) = 0x00001001u;
    }
#endif
    sub_001EC1B0_gen();
}

void sub_002DDC9D(void)                       /* XInputPoll: stdcall 4 */
{
    RECOMP_RR_DECL
    g_eax = 0u;
    g_esp += 8u;
}

void sub_002DDBF7(void)                       /* XInputGetState (gamepad): stdcall 8 */
{
    RECOMP_RR_DECL
    static uint32_t s_pkt = 1u;
    uint32_t hdev   = MEM32(g_esp + 4u);
    uint32_t pstate = MEM32(g_esp + 8u);
    if (hdev != 0u && pstate != 0u) {
        uint8_t rep[20];
        int i;
        fo2_vita_report(rep);
        MEM32(pstate) = s_pkt++;
        for (i = 0; i < 20; i++)
            MEM8(pstate + 4u + (uint32_t)i) = rep[i];
        g_eax = 0u;
    } else {
        g_eax = 0x0000048Fu;
    }
    g_esp += 12u;
}

void sub_002D7349(void)                       /* XGetDevices: stdcall 4 */
{
    RECOMP_RR_DECL
    g_eax = 1u;
    g_esp += 8u;
}

static int g_xinput_announced = 0;

void sub_002D736B(void)                       /* XGetDeviceChanges: stdcall 12 */
{
    RECOMP_RR_DECL
    uint32_t pins = MEM32(g_esp + 8u);
    uint32_t prem = MEM32(g_esp + 12u);
    if (!g_xinput_announced) {
        g_xinput_announced = 1;
        if (pins) MEM32(pins) = 1u;
        if (prem) MEM32(prem) = 0u;
        g_eax = 1u;
    } else {
        if (pins) MEM32(pins) = 0u;
        if (prem) MEM32(prem) = 0u;
        g_eax = 0u;
    }
    if (g_fo2_pstate)
        fo2_vita_put_report(g_fo2_pstate);
    g_esp += 16u;
}

void sub_002DD9BD(void)                       /* XInputOpen: stdcall 16 */
{
    RECOMP_RR_DECL
    uint32_t port = MEM32(g_esp + 8u);
    g_eax = (port < 4u) ? (0x00001001u + port) : 0u;
    g_esp += 20u;
}

void sub_002DDA13(void)                       /* XInputClose: stdcall 4 */
{
    RECOMP_RR_DECL
    g_esp += 8u;
}

void sub_002DDA1F(void)                       /* XInputGetState: stdcall 8 */
{
    RECOMP_RR_DECL
    uint32_t hdev   = MEM32(g_esp + 4u);
    uint32_t pstate = MEM32(g_esp + 8u);
    if (hdev != 0u && pstate != 0u) {
        static unsigned s_poll_cnt = 0;
        uint32_t i;
        s_poll_cnt++;
        if (s_poll_cnt == 1u || s_poll_cnt == 10u || s_poll_cnt == 100u || (s_poll_cnt % 600u) == 0u) {
            fprintf(stderr, "[VITA_XINPUT] poll #%u pState=0x%08X\n", s_poll_cnt, (unsigned)pstate);
        }
        for (i = 0; i < 21u; i++)
            MEM8(pstate + i) = 0u;
        g_fo2_pstate = pstate;
        fo2_vita_put_report(pstate);
        g_eax = 0u;
    } else {
        g_eax = 0x0000048Fu;                 /* ERROR_DEVICE_NOT_CONNECTED */
    }
    g_esp += 12u;
}
#endif /* __vita__ */

/* XInitDevices (stdcall 12). The USBD bring-up under it (sub_002D6B44) polls
 * the OHCI for a device and an interrupt that the Vita never delivers, and
 * the title's main thread stays inside this call until that poll gives up.
 * Every XAPI call the game makes is answered above, so there is nothing for
 * the bring-up to set up: on the Vita it returns at once. Other platforms
 * keep the lifted routine (sub_002D7308_gen, regenerated under this name). */
void sub_002D7308(void)
{
    RECOMP_RR_DECL
#if defined(__vita__)
    g_xinput_announced = 0;
    fprintf(stderr, "[VITA_XINPUT] XInitDevices (sub_002D7308) called -> reset g_xinput_announced=0\n");
    g_esp += 12u;                        /* ret 8: return address + 2 args */
    g_eax = 0u;
#else
    sub_002D7308_gen();
#endif
}

void xbox_OhciTick(void) {}
/* No USBD trace window exists any more (ohci.c has none); 0 keeps the
 * [usbd] lines silent. Matches the int(void) declarations in ohci.h and
 * kernel_bridge.c. */
int xbox_usbd_trace_take(void) { return 0; }
