/*
 * win32_compat.c - POSIX implementation of generic Win32 host primitives.
 *
 * See win32_compat.h. This is deliberately a *generic* OS-primitive layer
 * (threads/events/mutexes/atomics/heap/timers) -- it carries no Xbox
 * semantics. The Xbox kernel HLE in src/kernel builds on top of it.
 *
 * POSIX (Linux/MacOS) only.
 */

#if !defined(_WIN32)

/* Enable memfd_create, MAP_FIXED_NOREPLACE, timegm. Must precede all #includes. */
#define _GNU_SOURCE
/* Darwin: exposes memset_s, its explicit_bzero equivalent. */
#define __STDC_WANT_LIB_EXT1__ 1

#include "win32_compat.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <fenv.h>
#if defined(__SWITCH__)
/* Horizon: no mmap. Memory is kernel objects mapped at chosen addresses
 * (see "Virtual memory" below). Only the kernel headers are pulled in --
 * the rest of libnx's names would collide with the Win32 vocabulary. */
#include <switch/types.h>
#include <switch/result.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/virtmem.h>
#include <switch/runtime/env.h>
#include <malloc.h>
#else
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <malloc.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#elif !defined(__SWITCH__) && !defined(__vita__)
#include <sys/sysinfo.h>
#endif

/* ===================================================================== */
/* Last-error (thread-local)                                             */
/* ===================================================================== */

#if defined(__vita__)
static pthread_key_t  s_vita_err_key;
static pthread_once_t s_vita_err_once = PTHREAD_ONCE_INIT;
static void vita_err_key_init(void) { pthread_key_create(&s_vita_err_key, NULL); }
DWORD GetLastError(void) {
    pthread_once(&s_vita_err_once, vita_err_key_init);
    return (DWORD)(uintptr_t)pthread_getspecific(s_vita_err_key);
}
VOID SetLastError(DWORD code) {
    pthread_once(&s_vita_err_once, vita_err_key_init);
    pthread_setspecific(s_vita_err_key, (void *)(uintptr_t)code);
}
#else
static __thread DWORD t_last_error = 0;

DWORD GetLastError(void)            { return t_last_error; }
VOID  SetLastError(DWORD code)      { t_last_error = code; }
#endif

/* ===================================================================== */
/* Interlocked atomics                                                   */
/* ===================================================================== */

LONG InterlockedIncrement(volatile LONG *p)        { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedDecrement(volatile LONG *p)        { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }
/* Both return the original value, as Win32 does. */
LONG InterlockedOr(volatile LONG *p, LONG v)          { return __atomic_fetch_or(p, v, __ATOMIC_SEQ_CST); }
LONG InterlockedAnd(volatile LONG *p, LONG v)         { return __atomic_fetch_and(p, v, __ATOMIC_SEQ_CST); }

LONG InterlockedCompareExchange(volatile LONG *p, LONG xchg, LONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

LONGLONG InterlockedCompareExchange64(volatile LONGLONG *p, LONGLONG xchg, LONGLONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

PVOID InterlockedCompareExchangePointer(PVOID volatile *p, PVOID xchg, PVOID cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

/* ===================================================================== */
/* Critical sections (recursive pthread mutex)                           */
/* ===================================================================== */

VOID InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
    memset(cs, 0, sizeof(*cs));
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    cs->LockSemaphore = m;
}

VOID InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin)
{
    InitializeCriticalSection(cs);
    cs->SpinCount = spin;
}

VOID EnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    pthread_mutex_lock((pthread_mutex_t *)cs->LockSemaphore);
    cs->RecursionCount++;
}

VOID LeaveCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) return;
    cs->RecursionCount--;
    pthread_mutex_unlock((pthread_mutex_t *)cs->LockSemaphore);
}

BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    if (pthread_mutex_trylock((pthread_mutex_t *)cs->LockSemaphore) == 0) {
        cs->RecursionCount++;
        return TRUE;
    }
    return FALSE;
}

VOID DeleteCriticalSection(LPCRITICAL_SECTION cs)
{
    if (cs->LockSemaphore) {
        pthread_mutex_destroy((pthread_mutex_t *)cs->LockSemaphore);
        free(cs->LockSemaphore);
        cs->LockSemaphore = NULL;
    }
}

/* ===================================================================== */
/* Slim reader/writer locks                                              */
/* ===================================================================== */

/* An SRWLOCK is usable straight from SRWLOCK_INIT, so the pthread_rwlock_t
 * behind it has to appear on first use. Unlike the condition variables below
 * -- whose lazy init is covered by the caller holding the paired CRITICAL
 * SECTION -- an SRWLOCK is by definition taken from several threads at once
 * with nothing else held, so first use genuinely races. Serialise just that:
 * once Ptr is published, every acquire is a plain atomic load. */
static pthread_rwlock_t *srw_lazy_init(PSRWLOCK lock)
{
    pthread_rwlock_t *rw = __atomic_load_n((pthread_rwlock_t **)&lock->Ptr,
                                           __ATOMIC_ACQUIRE);
    if (!rw) {
        static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&init_lock);
        rw = (pthread_rwlock_t *)lock->Ptr;
        if (!rw) {
            rw = (pthread_rwlock_t *)malloc(sizeof(*rw));
            pthread_rwlock_init(rw, NULL);
            __atomic_store_n((pthread_rwlock_t **)&lock->Ptr, rw, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&init_lock);
    }
    return rw;
}

VOID InitializeSRWLock(PSRWLOCK lock)
{
    lock->Ptr = NULL;
    srw_lazy_init(lock);
}

VOID AcquireSRWLockShared(PSRWLOCK lock)     { pthread_rwlock_rdlock(srw_lazy_init(lock)); }
VOID ReleaseSRWLockShared(PSRWLOCK lock)     { pthread_rwlock_unlock(srw_lazy_init(lock)); }
VOID AcquireSRWLockExclusive(PSRWLOCK lock)  { pthread_rwlock_wrlock(srw_lazy_init(lock)); }
VOID ReleaseSRWLockExclusive(PSRWLOCK lock)  { pthread_rwlock_unlock(srw_lazy_init(lock)); }

/* ===================================================================== */
/* One-time initialisation                                               */
/* ===================================================================== */

/* Win32 semantics: the callback runs at most once for a given INIT_ONCE, and
 * a callback returning FALSE leaves it un-run so a later call retries. Ptr
 * doubles as the "done" flag. One global mutex covers every INIT_ONCE --
 * initialisation is rare, and the fast path never touches it. */
BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, PVOID *context)
{
    static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;

    if (__atomic_load_n(&once->Ptr, __ATOMIC_ACQUIRE))
        return TRUE;

    pthread_mutex_lock(&once_lock);
    BOOL ok = TRUE;
    if (!once->Ptr) {
        ok = fn(once, param, context);
        if (ok)
            __atomic_store_n(&once->Ptr, (PVOID)1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once_lock);
    return ok;
}

/* ===================================================================== */
/* Condition variables (paired with a CRITICAL_SECTION)                  */
/* ===================================================================== */

/* Forward decl; the definition lives further down with the wait helpers. */
static void deadline_from_ms(DWORD ms, struct timespec *ts);

static void cv_lazy_init(PCONDITION_VARIABLE cv)
{
    if (!cv->Ptr) {
        pthread_cond_t *c = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
        pthread_cond_init(c, NULL);
        /* Race window OK for typical Win32 usage (the CS is held). */
        cv->Ptr = c;
    }
}

VOID InitializeConditionVariable(PCONDITION_VARIABLE cv)
{
    cv->Ptr = NULL;
    cv_lazy_init(cv);
}

BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms)
{
    cv_lazy_init(cv);
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    pthread_cond_t  *c = (pthread_cond_t  *)cv->Ptr;
    pthread_mutex_t *m = (pthread_mutex_t *)cs->LockSemaphore;
    if (ms == INFINITE) {
        pthread_cond_wait(c, m);
        return TRUE;
    }
    struct timespec ts;
    deadline_from_ms(ms, &ts);
    int rc = pthread_cond_timedwait(c, m, &ts);
    if (rc == ETIMEDOUT) { SetLastError(WAIT_TIMEOUT); return FALSE; }
    return TRUE;
}

VOID WakeConditionVariable(PCONDITION_VARIABLE cv)
{
    cv_lazy_init(cv);
    pthread_cond_signal((pthread_cond_t *)cv->Ptr);
}

VOID WakeAllConditionVariable(PCONDITION_VARIABLE cv)
{
    cv_lazy_init(cv);
    pthread_cond_broadcast((pthread_cond_t *)cv->Ptr);
}

/* ===================================================================== */
/* Waitable kernel objects                                               */
/* ===================================================================== */

typedef enum { K_EVENT, K_SEM, K_MUTEX, K_THREAD, K_TIMER, K_HEAP,
               K_FILEMAP, K_FILE, K_WAITABLE_TIMER } w32_kind;

#define W32_MAX_APC 16

typedef struct w32_object {
    w32_kind        kind;
    LONG            refcount;
    pthread_mutex_t lock;
    pthread_cond_t  cond;

    /* event */
    int             signaled;
    int             manual_reset;

    /* semaphore */
    long            sem_count;
    long            sem_max;

    /* mutex */
    DWORD           mtx_owner;
    int             mtx_recursion;

    /* thread */
    pthread_t       thread;
    int             thread_joinable;
    DWORD           tid;
    int             exited;
    DWORD           exit_code;
    int             suspend_count;
    pthread_cond_t  gate;
    LPTHREAD_START_ROUTINE start;
    LPVOID          start_param;
    int             priority;
    int             host_rt;         /* host priority raised (host_priority_apply) */
    int             host_base;       /* Linux: RT priority it started at */
    volatile int    started;         /* Linux: o->thread is valid */
#if defined(__SWITCH__)
    Handle          nx_handle;       /* Horizon thread, once it runs */
#endif
    PAPCFUNC        apc_func[W32_MAX_APC];
    ULONG_PTR       apc_data[W32_MAX_APC];
    int             apc_count;

    /* timer-queue timer */
    int             timer_cancel;
    DWORD           timer_due;
    DWORD           timer_period;
    uintptr_t       nx_view0;      /* Switch file mapping: its first view */
    WAITORTIMERCALLBACK timer_cb;
    PVOID           timer_param;

    /* waitable timer */
    int             waitable_manual_reset;
    struct timespec waitable_due_time;
    int             waitable_triggered;
    int             waitable_armed;

    /* file mapping / fd-backed file handle */
    int             fd;
    SIZE_T          map_size;
    char           *file_path;
} w32_object;

/* pseudo handles for "current thread"/"current process" */
#define PSEUDO_CURRENT_PROCESS ((HANDLE)(LONG_PTR)-1)
#define PSEUDO_CURRENT_THREAD  ((HANDLE)(LONG_PTR)-2)
#define STILL_ACTIVE 259u

static volatile LONG        s_next_tid = 1000;
#if defined(__vita__)
static pthread_key_t  s_vita_self_key;
static pthread_key_t  s_vita_tid_key;
static pthread_once_t s_vita_thr_once = PTHREAD_ONCE_INIT;
static void vita_thr_keys_init(void) {
    pthread_key_create(&s_vita_self_key, NULL);
    pthread_key_create(&s_vita_tid_key, NULL);
}
static inline w32_object *get_t_self_obj(void) {
    pthread_once(&s_vita_thr_once, vita_thr_keys_init);
    return (w32_object *)pthread_getspecific(s_vita_self_key);
}
static inline void set_t_self_obj(w32_object *o) {
    pthread_once(&s_vita_thr_once, vita_thr_keys_init);
    pthread_setspecific(s_vita_self_key, o);
}
static inline DWORD get_t_tid(void) {
    pthread_once(&s_vita_thr_once, vita_thr_keys_init);
    return (DWORD)(uintptr_t)pthread_getspecific(s_vita_tid_key);
}
static inline void set_t_tid(DWORD tid) {
    pthread_once(&s_vita_thr_once, vita_thr_keys_init);
    pthread_setspecific(s_vita_tid_key, (void *)(uintptr_t)tid);
}
#define t_self_obj (get_t_self_obj())
#else
static __thread w32_object *t_self_obj = NULL;
static __thread DWORD       t_tid      = 0;
#define set_t_self_obj(o) (t_self_obj = (o))
#define get_t_tid()       (t_tid)
#define set_t_tid(v)      (t_tid = (v))
#endif

DWORD GetCurrentThreadId(void)
{
    DWORD tid = get_t_tid();
    if (tid == 0) {
        tid = (DWORD)InterlockedIncrement(&s_next_tid);
        set_t_tid(tid);
    }
    return tid;
}

static w32_object *obj_from(HANDLE h);

/* 0 for anything that is not a thread, as Win32 reports failure. */
DWORD GetThreadId(HANDLE h)
{
    w32_object *o;

    if (h == PSEUDO_CURRENT_THREAD)
        return GetCurrentThreadId();
    o = obj_from(h);
    return (o && o->kind == K_THREAD) ? o->tid : 0;
}

DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
HANDLE GetCurrentThread(void)   { return t_self_obj ? (HANDLE)t_self_obj : PSEUDO_CURRENT_THREAD; }
HANDLE GetCurrentProcess(void)  { return PSEUDO_CURRENT_PROCESS; }

/* Registry of live objects.
 *
 * A HANDLE here is a w32_object pointer, and Win32 callers -- the kernel HLE
 * above all -- pass values that are not handles at all: a guest dispatcher
 * object's VA, a token from another table, a stale handle already closed.
 * Windows answers those with ERROR_INVALID_HANDLE; dereferencing them faults.
 * Every handle entry point resolves through obj_from(), which returns NULL for
 * anything not registered here, so a bad handle fails the way it does on
 * Windows instead of taking the process down. */
#define W32_REG_SIZE 65536u            /* power of two */
#define W32_REG_TOMB ((w32_object *)(uintptr_t)1)
static w32_object     *s_reg[W32_REG_SIZE];
static pthread_mutex_t s_reg_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t reg_hash(const void *p)
{
    uintptr_t v = (uintptr_t)p >> 4;
    return (uint32_t)((v * 0x9E3779B97F4A7C15ull) >> 40) & (W32_REG_SIZE - 1);
}

static void reg_add(w32_object *o)
{
    uint32_t i = reg_hash(o), n;
    pthread_mutex_lock(&s_reg_lock);
    for (n = 0; n < W32_REG_SIZE; n++, i = (i + 1) & (W32_REG_SIZE - 1)) {
        if (!s_reg[i] || s_reg[i] == W32_REG_TOMB) {
            s_reg[i] = o;
            break;
        }
    }
    pthread_mutex_unlock(&s_reg_lock);
}

static void reg_remove(w32_object *o)
{
    uint32_t i = reg_hash(o), n;
    pthread_mutex_lock(&s_reg_lock);
    for (n = 0; n < W32_REG_SIZE && s_reg[i];
         n++, i = (i + 1) & (W32_REG_SIZE - 1)) {
        if (s_reg[i] == o) {
            s_reg[i] = W32_REG_TOMB;
            break;
        }
    }
    pthread_mutex_unlock(&s_reg_lock);
}

static w32_object *obj_from(HANDLE h)
{
    w32_object *o = (w32_object *)h, *hit = NULL;
    uint32_t i, n;

    if (!o || o == W32_REG_TOMB || h == INVALID_HANDLE_VALUE)
        return NULL;
    i = reg_hash(o);
    pthread_mutex_lock(&s_reg_lock);
    for (n = 0; n < W32_REG_SIZE && s_reg[i];
         n++, i = (i + 1) & (W32_REG_SIZE - 1)) {
        if (s_reg[i] == o) {
            hit = o;
            break;
        }
    }
    pthread_mutex_unlock(&s_reg_lock);
    if (!hit)
        SetLastError(ERROR_INVALID_HANDLE);
    return hit;
}

static w32_object *obj_alloc(w32_kind kind)
{
    w32_object *o = (w32_object *)calloc(1, sizeof(w32_object));
    o->kind     = kind;
    o->refcount = 1;
    pthread_mutex_init(&o->lock, NULL);
    pthread_cond_init(&o->cond, NULL);
    pthread_cond_init(&o->gate, NULL);
    reg_add(o);
    return o;
}

static void vita_big_free(void *ptr);
static void obj_release(w32_object *o)
{
    if (InterlockedDecrement(&o->refcount) > 0)
        return;
    reg_remove(o);
    if (o->kind == K_FILE) {
        if (o->fd >= 0) close(o->fd);
        free(o->file_path);
    } else if (o->kind == K_FILEMAP) {
#if defined(__vita__)
        if (o->nx_view0) vita_big_free((void *)o->nx_view0);
#else
        if (o->fd >= 0) close(o->fd);
#endif
    } else if (o->kind == K_WAITABLE_TIMER) {
        /* Waitable timers: no special cleanup needed */
    }
    pthread_mutex_destroy(&o->lock);
    pthread_cond_destroy(&o->cond);
    pthread_cond_destroy(&o->gate);
    free(o);
}

/* ---- fd-backed file handle (for the file-I/O HLE) -------------------- */
HANDLE w32_open_handle(int fd, const char *host_path)
{
    w32_object *o = obj_alloc(K_FILE);
    o->fd        = fd;
    o->file_path = host_path ? strdup(host_path) : NULL;
    return (HANDLE)o;
}

int w32_handle_fd(HANDLE h)
{
    w32_object *o = obj_from(h);
    return (o && o->kind == K_FILE) ? o->fd : -1;
}

const char *w32_handle_path(HANDLE h)
{
    w32_object *o = obj_from(h);
    return (o && o->kind == K_FILE) ? o->file_path : NULL;
}

BOOL CloseHandle(HANDLE h)
{
    if (!h || h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS ||
        h == INVALID_HANDLE_VALUE)
        return TRUE;
    {
        w32_object *o = obj_from(h);
        if (!o)
            return FALSE;
        obj_release(o);
    }
    return TRUE;
}

BOOL DuplicateHandle(HANDLE srcProc, HANDLE src, HANDLE dstProc, PHANDLE dst,
                     DWORD access, BOOL inherit, DWORD options)
{
    (void)srcProc; (void)dstProc; (void)access; (void)inherit;
    if (!dst) return FALSE;
    if (src == PSEUDO_CURRENT_THREAD)  src = GetCurrentThread();
    if (src == PSEUDO_CURRENT_PROCESS) { *dst = src; return TRUE; }
    if (src == PSEUDO_CURRENT_THREAD || !src) { *dst = src; return TRUE; }
    w32_object *o = obj_from(src);
    if (!o)
        return FALSE;
    InterlockedIncrement(&o->refcount);
    *dst = src;
    if (options & DUPLICATE_CLOSE_SOURCE)
        obj_release(o);
    return TRUE;
}

/* ---- deadline helper -------------------------------------------------- */
static void deadline_from_ms(DWORD ms, struct timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* Run any pending user-APCs for the calling thread. Returns count run. */
static int drain_apcs(void)
{
    w32_object *o = t_self_obj;
    int run = 0;
    if (!o) return 0;
    pthread_mutex_lock(&o->lock);
    while (o->apc_count > 0) {
        PAPCFUNC  f = o->apc_func[0];
        ULONG_PTR d = o->apc_data[0];
        memmove(o->apc_func, o->apc_func + 1, sizeof(PAPCFUNC) * (o->apc_count - 1));
        memmove(o->apc_data, o->apc_data + 1, sizeof(ULONG_PTR) * (o->apc_count - 1));
        o->apc_count--;
        pthread_mutex_unlock(&o->lock);
        f(d);
        run++;
        pthread_mutex_lock(&o->lock);
    }
    pthread_mutex_unlock(&o->lock);
    return run;
}

static int timespec_before(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec != b->tv_sec ? a->tv_sec < b->tv_sec : a->tv_nsec < b->tv_nsec;
}

/* Signalled once the due time passes; latched, so a manual-reset timer stays
 * signalled until it is set or cancelled again. Caller holds o->lock. */
static int waitable_due(w32_object *o)
{
    struct timespec now;
    if (o->waitable_triggered) return 1;
    if (!o->waitable_armed) return 0;
    clock_gettime(CLOCK_REALTIME, &now);
    if (timespec_before(&now, &o->waitable_due_time)) return 0;
    o->waitable_triggered = 1;
    return 1;
}

/*
 * Wait on a single object. The object lock must NOT be held.
 * Returns WAIT_OBJECT_0 / WAIT_TIMEOUT.
 */
static DWORD wait_single(w32_object *o, DWORD ms)
{
    struct timespec ts;
    int timed = (ms != INFINITE);
    if (timed) deadline_from_ms(ms, &ts);

    pthread_mutex_lock(&o->lock);
    DWORD result = WAIT_OBJECT_0;

    for (;;) {
        int ready = 0;
        switch (o->kind) {
        case K_EVENT:  ready = o->signaled; break;
        case K_THREAD: ready = o->exited;   break;
        case K_SEM:    ready = (o->sem_count > 0); break;
        case K_MUTEX:
            ready = (o->mtx_owner == 0 || o->mtx_owner == GetCurrentThreadId());
            break;
        case K_WAITABLE_TIMER: ready = waitable_due(o); break;
        default:       ready = 1; break;
        }
        if (ready) break;

        /* An armed timer has its own deadline. Waiting on the caller's alone
         * would sleep straight past the due time, so take whichever comes
         * first and re-test. */
        struct timespec until = ts;
        int bounded = timed;
        if (o->kind == K_WAITABLE_TIMER && o->waitable_armed &&
            (!timed || timespec_before(&o->waitable_due_time, &ts))) {
            until = o->waitable_due_time;
            bounded = 1;
        }
        int rc = bounded ? pthread_cond_timedwait(&o->cond, &o->lock, &until)
                         : pthread_cond_wait(&o->cond, &o->lock);
        if (rc == ETIMEDOUT && timed && !timespec_before(&until, &ts)) {
            result = WAIT_TIMEOUT; break;
        }
    }

    if (result == WAIT_OBJECT_0) {
        switch (o->kind) {
        case K_EVENT: if (!o->manual_reset) o->signaled = 0; break;
        case K_WAITABLE_TIMER:
            if (!o->waitable_manual_reset) { o->waitable_triggered = 0; o->waitable_armed = 0; }
            break;
        case K_SEM:   o->sem_count--; break;
        case K_MUTEX: o->mtx_owner = GetCurrentThreadId(); o->mtx_recursion++; break;
        default: break;
        }
    }
    pthread_mutex_unlock(&o->lock);
    return result;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (!h || h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS)
        return WAIT_OBJECT_0;
    {
        w32_object *o = obj_from(h);
        if (!o)
            return WAIT_FAILED;
        return wait_single(o, ms);
    }
}

DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    return WaitForSingleObject(h, ms);
}

/*
 * WaitForMultipleObjects: polling implementation. Adequate for the light
 * multi-object waits the Xbox kernel HLE issues; not a high-throughput path.
 */
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll, DWORD ms)
{
    return WaitForMultipleObjectsEx(count, handles, waitAll, ms, FALSE);
}

DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL waitAll,
                               DWORD ms, BOOL alertable)
{
    struct timespec ts;
    int timed = (ms != INFINITE);
    if (timed) deadline_from_ms(ms, &ts);

    for (DWORD i = 0; i < count; i++) {
        HANDLE h = handles[i];
        if (h && h != PSEUDO_CURRENT_THREAD && h != PSEUDO_CURRENT_PROCESS
                && !obj_from(h))
            return WAIT_FAILED;
    }

    for (;;) {
        if (alertable && drain_apcs() > 0)
            return WAIT_IO_COMPLETION;

        if (waitAll) {
            DWORD got = 0;
            for (DWORD i = 0; i < count; i++)
                if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) got++;
            if (got == count) return WAIT_OBJECT_0;
        } else {
            for (DWORD i = 0; i < count; i++)
                if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0)
                    return WAIT_OBJECT_0 + i;
        }

        if (timed) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > ts.tv_sec ||
                (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec))
                return WAIT_TIMEOUT;
        }
        usleep(1000);
    }
}

/* ===================================================================== */
/* Events                                                                */
/* ===================================================================== */

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_EVENT);
    o->manual_reset = manualReset ? 1 : 0;
    o->signaled     = initialState ? 1 : 0;
    return (HANDLE)o;
}
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCWSTR name)
{
    (void)name;
    return CreateEventA(sa, manualReset, initialState, NULL);
}

BOOL SetEvent(HANDLE h)
{
    w32_object *o = obj_from(h);
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL ResetEvent(HANDLE h)
{
    w32_object *o = obj_from(h);
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL PulseEvent(HANDLE h)
{
    w32_object *o = obj_from(h);
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cond);
    o->signaled = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Semaphores                                                            */
/* ===================================================================== */

HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_SEM);
    o->sem_count = initial;
    o->sem_max   = maximum;
    return (HANDLE)o;
}
HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR name)
{
    (void)name;
    return CreateSemaphoreA(sa, initial, maximum, NULL);
}

BOOL ReleaseSemaphore(HANDLE h, LONG releaseCount, PLONG previousCount)
{
    w32_object *o = obj_from(h);
    if (!o || o->kind != K_SEM) return FALSE;
    pthread_mutex_lock(&o->lock);
    if (previousCount) *previousCount = (LONG)o->sem_count;
    o->sem_count += releaseCount;
    if (o->sem_max && o->sem_count > o->sem_max) o->sem_count = o->sem_max;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Mutexes                                                               */
/* ===================================================================== */

HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_MUTEX);
    if (initialOwner) { o->mtx_owner = GetCurrentThreadId(); o->mtx_recursion = 1; }
    return (HANDLE)o;
}
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCWSTR name)
{
    (void)name;
    return CreateMutexA(sa, initialOwner, NULL);
}

BOOL ReleaseMutex(HANDLE h)
{
    w32_object *o = obj_from(h);
    if (!o || o->kind != K_MUTEX) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    pthread_mutex_lock(&o->lock);
    if (o->mtx_owner != GetCurrentThreadId()) {
        pthread_mutex_unlock(&o->lock);
        SetLastError(ERROR_NOT_OWNER);
        return FALSE;
    }
    if (--o->mtx_recursion <= 0) {
        o->mtx_owner = 0;
        o->mtx_recursion = 0;
        pthread_cond_broadcast(&o->cond);
    }
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Threads                                                               */
/* ===================================================================== */

/* Spread threads over the application cores.
 *
 * libnx creates every pthread at priority 59 with the process's core mask
 * but its default core as the preferred one, so all of them queue on core 0
 * and reach cores 1-2 only by migration. Preferred cores are dealt out in
 * turn instead. Cores 0-2 only: core 3 is the system's, and Horizon
 * time-slices priority 59 on 0-2 alone -- two busy threads meeting on core 3
 * (Eden reports mask 0xF) never yield to each other. */
/* ── One core for the guest ────────────────────────────────────
 *
 * The Xbox has one CPU. Only one of a title's threads runs at a time, an
 * interrupt pre-empts whatever runs rather than running beside it, and code
 * written for that machine can rely on both without knowing it. Here every
 * guest thread used to run truly in parallel, as did the interrupt and DPC
 * work: NFSU2 then read a pointer from a block its allocator had only just
 * filled with 0xAA and crashed after Start -- on the console nearly every
 * time, on a PC once in a few runs.
 *
 * So the threads that run guest code share one core: guest threads at the
 * time-sliced priority, the interrupt and DPC threads (xbox_guest_pin(1))
 * above them, so they pre-empt a guest thread the way an interrupt does and
 * never run alongside one. The runtime's own threads -- pushbuffer
 * executor, audio, log -- keep the other cores. RECOMP_GUEST_ONE_CORE=1
 * turns it on (2: guest threads only, interrupt threads float). */
int xbox_guest_core(void)
{
    static int core = -2;
    if (core == -2) {
        const char *e = getenv("RECOMP_GUEST_ONE_CORE");
        /* Opt-in: on Horizon it costs far too much. Its 10 ms slices at
         * priority 59 do not carry a movie's worth of guest threads on one
         * core without the title's own thread priorities, and throughput
         * fell to a tenth once they started. */
        int on = e && (*e == '1' || *e == '2');
        core = -1;
        if (on) {
#if defined(__SWITCH__)
            u64 mask = 0;
            if (R_SUCCEEDED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
                for (int c = 2; c >= 0; c--)
                    if ((mask >> c) & 1u) { core = c; break; }
#else
#if defined(__vita__)
            core = -1;
#else
            long n = sysconf(_SC_NPROCESSORS_ONLN);
            core = n > 1 ? (int)n - 1 : -1;
#endif
#endif
            if (core >= 0)
                fprintf(stderr, "  [THREAD] guest code runs on core %d only\n", core);
        }
    }
    return core;
}

void xbox_guest_pin(int interrupt)
{
    int core = xbox_guest_core();
    const char *e = getenv("RECOMP_GUEST_ONE_CORE");
    if (core < 0)
        return;
    if (interrupt && e && *e == '2')
        return;                /* mode 2: guest threads only, interrupts float */
#if defined(__SWITCH__)
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    /* libnx threads start at 59, Horizon's time-sliced priority; an
     * interrupt thread goes above it so it pre-empts guest threads. */
    if (interrupt)
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
#else
#if defined(__vita__)
    (void)core; (void)interrupt;
#else
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(core, &set);
        pthread_setaffinity_np(pthread_self(), sizeof set, &set);
        (void)interrupt;
    }
#endif
#endif
}

/* A runtime thread whose deadlines are audible (the APU frame thread): above
 * the guest threads on Horizon (0x2C < 59). Priority 59 is the only one that
 * time-slices, so there it waited behind whatever shared its core, in 10 ms
 * slices -- the APU fell to 1283 of 1500 frames/s in a race once the game
 * kept all three cores busy. It sleeps between blocks, so a higher,
 * non-sliced priority costs the others only the microseconds a block takes.
 * RECOMP_NX_AUDIO_PRIO=0 leaves it at 59. No-op elsewhere. */
void xbox_nx_raise_host_thread(void)
{
#if defined(__SWITCH__)
    const char *e = getenv("RECOMP_NX_AUDIO_PRIO");
    if (e && *e == '0')
        return;
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
#endif
}

#if defined(__SWITCH__)
void xbox_nx_spread_thread(void)
{
    static int next = 1;                 /* core 0 is the loader's main thread */
    u64 mask = 0;
    int k, core;

    if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)))
        return;
    mask &= 0x7u;
    if (xbox_guest_core() >= 0 && (mask & ~(1u << xbox_guest_core())))
        mask &= ~(1u << xbox_guest_core());   /* that core is the guest's */
    if (!mask || !(mask & (mask - 1)))
        return;                          /* one core: nothing to spread */
    k = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
    if (k == 1)
        fprintf(stderr, "  [NX] threads spread over core mask 0x%llX\n",
                (unsigned long long)mask);
    k %= __builtin_popcountll(mask);
    for (core = 0; core < 3; core++)
        if (((mask >> core) & 1u) && k-- == 0)
            break;
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, (u32)mask);
}


/* ── Per-thread CPU report (Switch) ────────────────────────────
 *
 * There is no profiler on the console. Each thread that goes through the
 * trampolines is recorded with its entry point; xbox_nx_thread_report()
 * (called by the Switch glue every 10 s) prints how much of a core each one
 * used since the last report, as Horizon counts it. Entry points are
 * printed relative to __start__, which is the ELF address:
 *   aarch64-none-elf-addr2line -f -e nfsu2_recomp.elf 0x<offset> */
Handle threadGetCurHandle(void);
extern char __start__[];

#define NX_MAX_TRACKED 64
static struct { Handle h; uintptr_t entry; u64 last; } s_nx_threads[NX_MAX_TRACKED];
static int s_nx_nthreads;

void xbox_nx_track_thread(void *entry)
{
    int i = __atomic_fetch_add(&s_nx_nthreads, 1, __ATOMIC_RELAXED);
    if (i >= NX_MAX_TRACKED)
        return;
    s_nx_threads[i].entry = (uintptr_t)entry;
    s_nx_threads[i].h = threadGetCurHandle();
}

static int nx_thread_ticks(Handle h, u64 *out)
{
    return R_SUCCEEDED(svcGetInfo(out, InfoType_ThreadTickCount, h, (u64)-1))
        || R_SUCCEEDED(svcGetInfo(out, InfoType_ThreadTickCountDeprecated, h, (u64)-1));
}

/* Tracked thread i for a sampling profiler (switch_nx.c): its handle and
 * the entry it is named by (0 for the game's main thread). Returns 0 past
 * the end of the table. */
int xbox_nx_thread_at(int i, Handle *h, uintptr_t *entry)
{
    int n = s_nx_nthreads < NX_MAX_TRACKED ? s_nx_nthreads : NX_MAX_TRACKED;
    if (i < 0 || i >= n || !s_nx_threads[i].h)
        return 0;
    *h = s_nx_threads[i].h;
    *entry = s_nx_threads[i].entry;
    return 1;
}

/* Guest threads all start in the bridge's trampoline; name them by the
 * lifted function they run instead. */
void xbox_nx_retag_thread(void *entry)
{
    Handle h = threadGetCurHandle();
    int n = s_nx_nthreads < NX_MAX_TRACKED ? s_nx_nthreads : NX_MAX_TRACKED, i;
    for (i = 0; i < n; i++)
        if (s_nx_threads[i].h == h) {
            s_nx_threads[i].entry = (uintptr_t)entry;
            return;
        }
}

void xbox_nx_thread_report(double interval_s)
{
    char line[1024];
    int n = s_nx_nthreads < NX_MAX_TRACKED ? s_nx_nthreads : NX_MAX_TRACKED, i, len;

    int failed = 0;
    len = snprintf(line, sizeof line, "[perf] %% of a core per thread (%d tracked):", n);
    for (i = 0; i < n && len < (int)sizeof line - 40; i++) {
        u64 t = 0, d;
        if (!s_nx_threads[i].h || !nx_thread_ticks(s_nx_threads[i].h, &t)) {
            failed++;
            continue;
        }
        d = t - s_nx_threads[i].last;
        s_nx_threads[i].last = t;
        if (d * 100 < (u64)(interval_s * 19200000.0))       /* under 1%: skip */
            continue;
        len += snprintf(line + len, sizeof line - len, " %s%lx=%.0f",
                        s_nx_threads[i].entry ? "" : "game:",
                        s_nx_threads[i].entry
                            ? (unsigned long)(s_nx_threads[i].entry - (uintptr_t)__start__) : 0ul,
                        (double)d / 19200000.0 / interval_s * 100.0);
    }
    if (n && failed == n)
        len += snprintf(line + len, sizeof line - len, " (thread tick counts unavailable)");
    fprintf(stderr, "%s\n", line);
}
#else
void xbox_nx_spread_thread(void) { }
void xbox_nx_track_thread(void *entry) { (void)entry; }
void xbox_nx_retag_thread(void *entry) { (void)entry; }
#endif

static void host_priority_apply(w32_object *o);

static void *thread_trampoline(void *arg)
{
    w32_object *o = (w32_object *)arg;
    xbox_nx_spread_thread();
    xbox_nx_track_thread((void *)o->start);
    set_t_self_obj(o);
    set_t_tid(o->tid);
#if defined(__SWITCH__)
    {
        extern Handle threadGetCurHandle(void);   /* libnx */
        o->nx_handle = threadGetCurHandle();
    }
#elif defined(__vita__)
    o->started = 1;
#else
    {
        struct sched_param sp;
        int pol;
        if (pthread_getschedparam(pthread_self(), &pol, &sp) == 0)
            o->host_base = sp.sched_priority;
        o->started = 1;
    }
    host_priority_apply(o);              /* a priority set before it ran */
#endif

#if defined(__vita__)
    sceKernelChangeThreadCpuAffinityMask(SCE_KERNEL_THREAD_ID_SELF,
                                         SCE_KERNEL_CPU_MASK_USER_1 | SCE_KERNEL_CPU_MASK_USER_2);
    sceKernelChangeThreadPriority(SCE_KERNEL_THREAD_ID_SELF, 0x64);
#endif

    /* CREATE_SUSPENDED gate */
    pthread_mutex_lock(&o->lock);
    while (o->suspend_count > 0)
        pthread_cond_wait(&o->gate, &o->lock);
    pthread_mutex_unlock(&o->lock);

    DWORD rc = o->start ? o->start(o->start_param) : 0;

    pthread_mutex_lock(&o->lock);
    o->exit_code = rc;
    o->exited    = 1;
    o->signaled  = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);

    obj_release(o);   /* drop the trampoline's reference */
    return NULL;
}

HANDLE CreateServiceThread(LPTHREAD_START_ROUTINE start, LPVOID param){return CreateThread(NULL,64u*1024u,start,param,0,NULL);}
HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stackSize,
                    LPTHREAD_START_ROUTINE start, LPVOID param,
                    DWORD flags, LPDWORD threadId)
{
    (void)sa;
    w32_object *o = obj_alloc(K_THREAD);
    o->start         = start;
    o->start_param   = param;
    o->tid           = (DWORD)InterlockedIncrement(&s_next_tid);
    o->suspend_count = (flags & CREATE_SUSPENDED) ? 1 : 0;
    o->refcount      = 2;   /* one for caller, one for the trampoline */

    pthread_attr_t attr;
    pthread_attr_init(&attr);
#if defined(__SWITCH__)
    /* Recompiled code nests host calls as deep as the guest nests its own,
     * and Horizon's default thread stack is a fraction of the 8 MB a Linux
     * thread gets. Match Linux rather than find the overflow in a game. */
    if (stackSize < 8u * 1024 * 1024)
        stackSize = 8u * 1024 * 1024;
#elif defined(__vita__)
    if (stackSize < 512u * 1024)
        stackSize = 512u * 1024;
#endif
    if (stackSize)
        pthread_attr_setstacksize(&attr, stackSize < 65536 ? 65536 : stackSize);

    if (pthread_create(&o->thread, &attr, thread_trampoline, o) != 0) {
        pthread_attr_destroy(&attr);
        o->refcount = 1;
        obj_release(o);
        SetLastError(8 /* ERROR_NOT_ENOUGH_MEMORY */);
        return NULL;
    }
    pthread_attr_destroy(&attr);
    o->thread_joinable = 1;

    if (threadId) *threadId = o->tid;
    return (HANDLE)o;
}

VOID ExitThread(DWORD exitCode)
{
    w32_object *o = t_self_obj;
    if (o) {
        pthread_mutex_lock(&o->lock);
        o->exit_code = exitCode;
        o->exited    = 1;
        o->signaled  = 1;
        pthread_cond_broadcast(&o->cond);
        pthread_mutex_unlock(&o->lock);
        obj_release(o);
    }
    pthread_exit(NULL);
}

BOOL GetExitCodeThread(HANDLE h, LPDWORD exitCode)
{
    w32_object *o = obj_from(h);
    if (!o || o->kind != K_THREAD || !exitCode) return FALSE;
    pthread_mutex_lock(&o->lock);
    *exitCode = o->exited ? o->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

DWORD ResumeThread(HANDLE h)
{
    w32_object *o = obj_from(h);
    if (!o || o->kind != K_THREAD) return (DWORD)-1;
    pthread_mutex_lock(&o->lock);
    DWORD prev = (DWORD)o->suspend_count;
    if (o->suspend_count > 0 && --o->suspend_count == 0)
        pthread_cond_broadcast(&o->gate);
    pthread_mutex_unlock(&o->lock);
    return prev;
}

DWORD SuspendThread(HANDLE h)
{
    /* True mid-run suspension is not supported on POSIX; only the
     * CREATE_SUSPENDED start gate is. Track the count for ResumeThread. */
    w32_object *o = obj_from(h);
    if (!o || o->kind != K_THREAD) return (DWORD)-1;
    pthread_mutex_lock(&o->lock);
    DWORD prev = (DWORD)o->suspend_count;
    o->suspend_count++;
    pthread_mutex_unlock(&o->lock);
    return prev;
}

BOOL TerminateThread(HANDLE h, DWORD exitCode)
{
    w32_object *o = obj_from(h);
    if (!o || o->kind != K_THREAD) return FALSE;
    pthread_cancel(o->thread);
    pthread_mutex_lock(&o->lock);
    o->exit_code = exitCode;
    o->exited    = 1;
    o->signaled  = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* Opt-in (RECOMP_NX_GUEST_RT=1): a guest thread at TIME_CRITICAL gets a
 * host priority that pre-empts. On Horizon every guest thread runs at 59,
 * the one priority that time-slices, so a thread with a deadline can wait a
 * 10 ms slice behind whatever shares its core. NFSU2's EA mixer (entry
 * 0x00274CA0, the only thread the title raises to time critical) refills its
 * DirectSound ring 5-15 ms ahead of the play cursor; late, the VP plays the
 * ring's 50 ms old contents (counted as stale in the [perf] APU ring line).
 * nfsmw-nx fixed the same with 0x2D for its audio server. Not the default:
 * the guest lock is a FIFO ticket lock, so the priority does not move the
 * mixer up its queue, and a pre-emptive thread that polls starves the
 * runtime threads sharing its core. Under a Linux SCHED_RR test (one core,
 * RR slices) it did not help. Elsewhere real RT priorities need privileges:
 * only a process already under SCHED_RR/FIFO is bumped. */
static void host_priority_apply(w32_object *o)
{
    const char *e = getenv("RECOMP_NX_GUEST_RT");
    int rt = o->priority >= THREAD_PRIORITY_TIME_CRITICAL;
    if (!(e && *e == '1') || rt == o->host_rt)
        return;
#if defined(__SWITCH__)
    if (!o->nx_handle)
        return;                          /* not running yet: the trampoline applies it */
    if (R_SUCCEEDED(svcSetThreadPriority(o->nx_handle, rt ? 0x2D : 0x3B))) {
        o->host_rt = rt;
        fprintf(stderr, "  [NX] guest thread %p at host priority 0x%X\n",
                (void *)o->start, rt ? 0x2D : 0x3B);
    }
#else
    {
        struct sched_param sp;
        int pol;
        pthread_t th = (o == t_self_obj) ? pthread_self() : o->thread;
        if (!o->started || pthread_getschedparam(th, &pol, &sp) ||
            (pol != SCHED_RR && pol != SCHED_FIFO))
            return;
        sp.sched_priority = o->host_base + (rt ? 1 : 0);
        if (pthread_setschedparam(th, pol, &sp) == 0)
            o->host_rt = rt;
    }
#endif
}

BOOL SetThreadPriority(HANDLE h, int priority)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : obj_from(h);
    if (o && o->kind == K_THREAD) {
        o->priority = priority;
        host_priority_apply(o);
    }
    return TRUE;
}

int GetThreadPriority(HANDLE h)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : obj_from(h);
    return (o && o->kind == K_THREAD) ? o->priority : THREAD_PRIORITY_NORMAL;
}

VOID SwitchToThread(void) { sched_yield(); }

DWORD QueueUserAPC(PAPCFUNC func, HANDLE thread, ULONG_PTR data)
{
    w32_object *o = (thread == PSEUDO_CURRENT_THREAD) ? t_self_obj : obj_from(thread);
    if (!o || o->kind != K_THREAD) return 0;
    pthread_mutex_lock(&o->lock);
    DWORD ok = 0;
    if (o->apc_count < W32_MAX_APC) {
        o->apc_func[o->apc_count] = func;
        o->apc_data[o->apc_count] = data;
        o->apc_count++;
        ok = 1;
    }
    pthread_mutex_unlock(&o->lock);
    return ok;
}

/* ===================================================================== */
/* Sleep                                                                 */
/* ===================================================================== */

VOID Sleep(DWORD ms)
{
    if (ms == 0) { sched_yield(); return; }
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
}

DWORD SleepEx(DWORD ms, BOOL alertable)
{
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    Sleep(ms);
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    return 0;
}

/* ===================================================================== */
/* Timer queues (one helper thread per timer)                            */
/* ===================================================================== */

static void *timer_thread(void *arg)
{
    w32_object *o = (w32_object *)arg;
    int once = (o->timer_period == 0);

    xbox_nx_spread_thread();

    /* initial due time */
    if (o->timer_due) Sleep(o->timer_due);
    if (!o->timer_cancel && o->timer_cb)
        o->timer_cb(o->timer_param, TRUE);

    while (!once && !o->timer_cancel) {
        Sleep(o->timer_period);
        if (o->timer_cancel) break;
        if (o->timer_cb) o->timer_cb(o->timer_param, TRUE);
    }
    obj_release(o);
    return NULL;
}

HANDLE CreateTimerQueue(void)
{
    /* A timer queue is just a grouping token here. */
    return (HANDLE)obj_alloc(K_TIMER);
}

BOOL DeleteTimerQueue(HANDLE timerQueue)
{
    return CloseHandle(timerQueue);
}

BOOL CreateTimerQueueTimer(PHANDLE newTimer, HANDLE timerQueue,
                           WAITORTIMERCALLBACK callback, PVOID param,
                           DWORD dueTime, DWORD period, ULONG flags)
{
    (void)timerQueue;
    if (flags & WT_EXECUTEONLYONCE) period = 0;
    w32_object *o = obj_alloc(K_TIMER);
    o->timer_cb     = callback;
    o->timer_param  = param;
    o->timer_due    = dueTime;
    o->timer_period = period;
    o->refcount     = 2;   /* caller + timer thread */

    if (pthread_create(&o->thread, NULL, timer_thread, o) != 0) {
        o->refcount = 1;
        obj_release(o);
        return FALSE;
    }
    pthread_detach(o->thread);
    if (newTimer) *newTimer = (HANDLE)o;
    return TRUE;
}

BOOL ChangeTimerQueueTimer(HANDLE timerQueue, HANDLE timer, ULONG dueTime, ULONG period)
{
    (void)timerQueue;
    w32_object *o = obj_from(timer);
    if (!o || o->kind != K_TIMER) return FALSE;
    o->timer_due    = dueTime;
    o->timer_period = period;
    return TRUE;
}

BOOL DeleteTimerQueueTimer(HANDLE timerQueue, HANDLE timer, HANDLE completionEvent)
{
    (void)timerQueue;
    w32_object *o = obj_from(timer);
    if (!o || o->kind != K_TIMER) return FALSE;
    o->timer_cancel = 1;
    if (completionEvent) SetEvent(completionEvent);
    obj_release(o);
    return TRUE;
}

struct w32_tp_args { PTP_SIMPLE_CALLBACK cb; PVOID ctx; };

static void *w32_tp_trampoline(void *arg)
{
    struct w32_tp_args *a = (struct w32_tp_args *)arg;
    xbox_nx_spread_thread();
    a->cb(NULL, a->ctx);
    free(a);
    return NULL;
}

BOOL TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK callback,
                                 PVOID context, PVOID env)
{
    (void)env;
    /* Run on a throwaway detached thread. */
    struct w32_tp_args *a = (struct w32_tp_args *)malloc(sizeof(*a));
    a->cb = callback; a->ctx = context;
    pthread_t th;
    if (pthread_create(&th, NULL, w32_tp_trampoline, a) != 0) { free(a); return FALSE; }
    pthread_detach(th);
    return TRUE;
}

/* ===================================================================== */
/* Waitable timers                                                       */
/* ===================================================================== */

HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, LPCWSTR name)
{
    (void)sa;
    (void)name;
    w32_object *o = obj_alloc(K_WAITABLE_TIMER);
    o->waitable_manual_reset = manualReset;
    return (HANDLE)o;
}

BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *dueTime, LONG period,
                      PTIMERAPCROUTINE completion, PVOID arg, BOOL resume)
{
    w32_object *o = obj_from(h);
    (void)completion; (void)arg; (void)resume;
    if (!o || o->kind != K_WAITABLE_TIMER || !dueTime) return FALSE;
    pthread_mutex_lock(&o->lock);
    /* Win32 100ns units: negative is relative to now, positive is an absolute
     * FILETIME. ponytail: period is ignored -- one-shot only, revisit if a
     * title actually arms a repeating timer. */
    if (dueTime->QuadPart <= 0) {
        clock_gettime(CLOCK_REALTIME, &o->waitable_due_time);
        LONGLONG ns = -dueTime->QuadPart * 100LL;
        o->waitable_due_time.tv_sec  += (time_t)(ns / 1000000000LL);
        o->waitable_due_time.tv_nsec += (long)(ns % 1000000000LL);
        if (o->waitable_due_time.tv_nsec >= 1000000000L) {
            o->waitable_due_time.tv_sec++;
            o->waitable_due_time.tv_nsec -= 1000000000L;
        }
    } else {
        /* FILETIME epoch is 1601-01-01; Unix is 1970-01-01. */
        LONGLONG unix100ns = dueTime->QuadPart - 116444736000000000LL;
        o->waitable_due_time.tv_sec  = (time_t)(unix100ns / 10000000LL);
        o->waitable_due_time.tv_nsec = (long)((unix100ns % 10000000LL) * 100LL);
    }
    (void)period;
    o->waitable_armed = 1;
    o->waitable_triggered = 0;
    pthread_mutex_unlock(&o->lock);
    pthread_cond_broadcast(&o->cond);
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE h)
{
    w32_object *o = obj_from(h);
    if (!o || o->kind != K_WAITABLE_TIMER) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->waitable_triggered = 0;
    o->waitable_armed = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Heap (thin wrapper over malloc; the single process heap)              */
/* ===================================================================== */

static w32_object s_process_heap = { .kind = K_HEAP };

HANDLE GetProcessHeap(void)                       { return (HANDLE)&s_process_heap; }
HANDLE HeapCreate(DWORD o, SIZE_T i, SIZE_T m)    { (void)o;(void)i;(void)m; return (HANDLE)&s_process_heap; }
BOOL   HeapDestroy(HANDLE h)                      { (void)h; return TRUE; }

LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{
    (void)heap;
    return (flags & HEAP_ZERO_MEMORY) ? calloc(1, bytes ? bytes : 1)
                                      : malloc(bytes ? bytes : 1);
}
LPVOID HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T bytes)
{
    (void)heap; (void)flags;
    return realloc(mem, bytes ? bytes : 1);
}
BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID mem)
{
    (void)heap; (void)flags;
    free(mem);
    return TRUE;
}
SIZE_T HeapSize(HANDLE heap, DWORD flags, LPCVOID mem)
{
    (void)heap; (void)flags; (void)mem;
    return 0;   /* glibc malloc_usable_size could be used; not needed yet */
}

/* ===================================================================== */
/* Virtual memory                                                        */
/* ===================================================================== */

void view_register(void *addr, size_t len);
size_t view_take(const void *addr);
#if defined(__vita__)
typedef struct { void *base; SceUID uid; } VitaBigSlot;
static VitaBigSlot s_vv[32];
static pthread_mutex_t s_vvl = PTHREAD_MUTEX_INITIALIZER;
static void *vita_big_alloc(size_t sz){
    size_t al=(sz+0xFFFu)&~(size_t)0xFFFu; if(!al)al=0x1000u;
    if(al>=1024u*1024u){
        al=(al+0xFFFFFu)&~(size_t)0xFFFFFu;
        SceUID u=sceKernelAllocMemBlock("fo2x_big",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,(SceSize)al,NULL);
        if(u<0){fprintf(stderr,"[vita] memblock %u MB refused: sceKernelAllocMemBlock 0x%08X\n",(unsigned)(al>>20),(unsigned)u);return NULL;}
        void *base=NULL;
        if(sceKernelGetMemBlockBase(u,&base)<0||!base){fprintf(stderr,"[vita] memblock %u MB: no base address\n",(unsigned)(al>>20));sceKernelFreeMemBlock(u);return NULL;}
        pthread_mutex_lock(&s_vvl); int slot=-1;
        for(int i=0;i<32;i++) if(!s_vv[i].base){s_vv[i].base=base;s_vv[i].uid=u;slot=i;break;}
        pthread_mutex_unlock(&s_vvl);
        if(slot<0){fprintf(stderr,"[vita] memblock %u MB: all %d slots in use\n",(unsigned)(al>>20),32);sceKernelFreeMemBlock(u);return NULL;}
        memset(base,0,al); return base;
    }
    void *p=memalign(0x1000u,al); if(p)memset(p,0,al); return p;
}
static void vita_big_free(void *ptr){
    if(!ptr)return; pthread_mutex_lock(&s_vvl); SceUID u=-1;
    for(int i=0;i<32;i++) if(s_vv[i].base==ptr){s_vv[i].base=NULL;u=s_vv[i].uid;break;}
    pthread_mutex_unlock(&s_vvl);
    if(u>=0)sceKernelFreeMemBlock(u); else free(ptr);
}
LPVOID VirtualAlloc(LPVOID addr,SIZE_T sz,DWORD fl,DWORD pr){(void)addr;(void)fl;(void)pr;if(!sz)return NULL;return vita_big_alloc(sz);}
BOOL VirtualFree(LPVOID addr,SIZE_T sz,DWORD ft){(void)sz;(void)ft;if(!addr)return FALSE;vita_big_free(addr);return TRUE;}
BOOL VirtualProtect(LPVOID a,SIZE_T sz,DWORD np,PDWORD op){(void)a;(void)sz;(void)np;if(op)*op=PAGE_READWRITE;return TRUE;}
HANDLE CreateFileMappingA(HANDLE h,LPSECURITY_ATTRIBUTES sa,DWORD pr,DWORD hi,DWORD lo,LPCSTR nm){(void)h;(void)sa;(void)pr;(void)hi;(void)nm;void *p=vita_big_alloc((size_t)lo);if(!p){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}w32_object *o=obj_alloc(K_FILEMAP);if(!o){vita_big_free(p);SetLastError(ERROR_NOT_ENOUGH_MEMORY);return NULL;}o->nx_view0=(uintptr_t)p;return (HANDLE)o;}
HANDLE CreateFileMappingW(HANDLE h,LPSECURITY_ATTRIBUTES sa,DWORD pr,DWORD hi,DWORD lo,LPCWSTR nm){(void)nm;return CreateFileMappingA(h,sa,pr,hi,lo,NULL);}
LPVOID MapViewOfFileEx(HANDLE hm,DWORD ac,DWORD hi,DWORD lo,SIZE_T sz,LPVOID ba){(void)ac;(void)hi;(void)lo;(void)sz;(void)ba;w32_object *o=obj_from(hm);if(!o||o->kind!=K_FILEMAP)return NULL;return (LPVOID)o->nx_view0;}
LPVOID MapViewOfFile(HANDLE hm,DWORD ac,DWORD hi,DWORD lo,SIZE_T sz){return MapViewOfFileEx(hm,ac,hi,lo,sz,NULL);}
BOOL UnmapViewOfFile(LPCVOID ba){(void)ba;return TRUE;}
void vita_susp_watch_scan(void){}
#endif

/* ===================================================================== */
/* Time                                                                  */
/* ===================================================================== */

/* 100-ns intervals between 1601-01-01 and 1970-01-01 */
#define FILETIME_EPOCH_DIFF 116444736000000000ULL

VOID GetSystemTimeAsFileTime(LPFILETIME ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ULONGLONG t = FILETIME_EPOCH_DIFF
                + (ULONGLONG)ts.tv_sec * 10000000ULL
                + (ULONGLONG)ts.tv_nsec / 100ULL;
    ft->dwLowDateTime  = (DWORD)(t & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

static void fill_systemtime(LPSYSTEMTIME st, const struct tm *tm, long nsec)
{
    st->wYear         = (WORD)(tm->tm_year + 1900);
    st->wMonth        = (WORD)(tm->tm_mon + 1);
    st->wDayOfWeek    = (WORD)tm->tm_wday;
    st->wDay          = (WORD)tm->tm_mday;
    st->wHour         = (WORD)tm->tm_hour;
    st->wMinute       = (WORD)tm->tm_min;
    st->wSecond       = (WORD)tm->tm_sec;
    st->wMilliseconds = (WORD)(nsec / 1000000L);
}

VOID GetSystemTime(LPSYSTEMTIME st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    fill_systemtime(st, &tm, ts.tv_nsec);
}

VOID GetLocalTime(LPSYSTEMTIME st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fill_systemtime(st, &tm, ts.tv_nsec);
}

ULONGLONG GetTickCount64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ULL + (ULONGLONG)ts.tv_nsec / 1000000ULL;
}

DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }

BOOL QueryPerformanceCounter(PLARGE_INTEGER count)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    count->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}

BOOL QueryPerformanceFrequency(PLARGE_INTEGER freq)
{
    freq->QuadPart = 1000000000LL;   /* QPC is in nanoseconds */
    return TRUE;
}

/* ===================================================================== */
/* Misc                                                                  */
/* ===================================================================== */

VOID OutputDebugStringA(LPCSTR str)
{
    if (str) fputs(str, stderr);
}

VOID ExitProcess(UINT exitCode) { exit((int)exitCode); }

BOOL IsDebuggerPresent(void)
{
#if defined(__APPLE__)
    /* Darwin: KERN_PROC_PID reports P_TRACED when a debugger is attached. */
    struct kinfo_proc info;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    size_t size = sizeof(info);
    memset(&info, 0, size);
    if (sysctl(mib, sizeof(mib), &info, &size, NULL, 0) != 0) return FALSE;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
    /* Linux: a non-zero TracerPid in /proc/self/status means ptrace is attached. */
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return FALSE;
    char line[256];
    BOOL traced = FALSE;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "TracerPid:", 10) == 0) {
            traced = strtol(line + 10, NULL, 10) != 0;
            break;
        }
    }
    fclose(f);
    return traced;
#endif
}

VOID DebugBreak(void) {
    /* Not __debugbreak(): that is an MSVC intrinsic, and this file is the
     * half that MSVC never compiles. SIGTRAP is the POSIX equivalent --
     * continuable under a debugger, fatal without one, as on Windows. */
    raise(SIGTRAP);
}

VOID SecureZeroMemory(PVOID ptr, SIZE_T cnt)
{
#if defined(__APPLE__)
    memset_s(ptr, cnt, 0, cnt);
#else
    explicit_bzero(ptr, cnt);
#endif
}

unsigned int _clearfp(void)
{
    feclearexcept(FE_ALL_EXCEPT);
    return 0;
}

/* ===================================================================== */
/* Win32 file API on POSIX (open/read/write/fstat-backed)                */
/* ===================================================================== */

#include <fcntl.h>
#include <sys/stat.h>

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    (void)share; (void)sa; (void)flags; (void)templ;
    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }

    int rw = O_RDONLY;
    int wantW = (access & (GENERIC_WRITE | GENERIC_ALL)) != 0;
    int wantR = (access & (GENERIC_READ  | GENERIC_ALL)) != 0;
    if (wantW && wantR) rw = O_RDWR;
    else if (wantW)     rw = O_WRONLY;

    int extra = 0;
    switch (disp) {
    case CREATE_NEW:        extra = O_CREAT | O_EXCL;  break;
    case CREATE_ALWAYS:     extra = O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING:     extra = 0;                 break;
    case OPEN_ALWAYS:       extra = O_CREAT;           break;
    case TRUNCATE_EXISTING: extra = O_TRUNC;           break;
    default:                extra = 0;                 break;
    }
    if ((extra & (O_CREAT | O_TRUNC)) && rw == O_RDONLY) rw = O_RDWR;

    /* Normalise embedded Windows-style backslashes before open(). */
    char norm[1024];
    snprintf(norm, sizeof(norm), "%s", name);
    xbox_path_normalize(norm);
    int fd = open(norm, rw | extra, 0644);
    if (fd < 0) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return w32_open_handle(fd, name);
}

HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    /* Basic UTF-16 -> UTF-8 (ASCII path of WideCharToMultiByte). */
    char buf[1024];
    int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), NULL, NULL);
    if (len <= 0) buf[0] = '\0';
    return CreateFileA(buf, access, share, sa, disp, flags, templ);
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD nread, void *overlapped)
{
    (void)overlapped;
    int fd = w32_handle_fd(h);
    if (fd < 0) { if (nread) *nread = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ssize_t n = read(fd, buf, len);
    if (n < 0)  { if (nread) *nread = 0; SetLastError(ERROR_GEN_FAILURE);    return FALSE; }
    if (nread)  *nread = (DWORD)n;
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD len, LPDWORD nwritten, void *overlapped)
{
    (void)overlapped;
    int fd = w32_handle_fd(h);
    if (fd < 0) { if (nwritten) *nwritten = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ssize_t n = write(fd, buf, len);
    if (n < 0)  { if (nwritten) *nwritten = 0; SetLastError(ERROR_GEN_FAILURE);    return FALSE; }
    if (nwritten) *nwritten = (DWORD)n;
    return TRUE;
}

DWORD GetFileSize(HANDLE h, LPDWORD high)
{
    int fd = w32_handle_fd(h);
    if (fd < 0) return INVALID_FILE_SIZE;
    struct stat st;
    if (fstat(fd, &st) != 0) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)(((uint64_t)st.st_size >> 32) & 0xFFFFFFFFu);
    return (DWORD)(st.st_size & 0xFFFFFFFFu);
}

BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    int fd = w32_handle_fd(h);
    if (fd < 0 || !size) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    struct stat st;
    if (fstat(fd, &st) != 0) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    size->QuadPart = (LONGLONG)st.st_size;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE h)
{
    int fd = w32_handle_fd(h);
    if (fd < 0) return FALSE;
    return fsync(fd) == 0;
}

/* ===================================================================== */
/* Keyboard + window helpers -- stubs. Real keyboard polling will come   */
/* via SDL_GetKeyboardState when main.c gets its SDL2 port.              */
/* ===================================================================== */

SHORT GetAsyncKeyState(int vKey)          { (void)vKey; return 0; }
HWND  FindWindowA(LPCSTR c, LPCSTR w)     { (void)c; (void)w; return NULL; }
HWND  GetActiveWindow(void)               { return NULL; }
BOOL  SetWindowTextA(HWND h, LPCSTR t)    { (void)h; (void)t; return TRUE; }
int   GetWindowTextA(HWND h, LPSTR t, int n) { (void)h; (void)t; (void)n; return 0; }
BOOL  EnumWindows(WNDENUMPROC p, LPARAM l) { (void)p; (void)l; return FALSE; }

int MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type)
{
    (void)h; (void)type;
    fprintf(stderr, "[%s] %s\n", caption ? caption : "MessageBox",
                                   text    ? text    : "");
    return 1;   /* IDOK */
}

/* Message-loop stubs: no Win32 messages on POSIX (SDL events drive the
 * d3d8_gl backend; this layer is just for the game's Win32 message pump). */
BOOL    PeekMessageA(LPMSG m, HWND w, UINT a, UINT b, UINT f)
{ (void)m; (void)w; (void)a; (void)b; (void)f; return FALSE; }
BOOL    TranslateMessage(const MSG *m) { (void)m; return TRUE; }
LRESULT DispatchMessageA(const MSG *m) { (void)m; return 0; }

/* XInput stub: real gamepad is wired through input_compat (SDL2). */
DWORD XInputGetState(DWORD idx, XINPUT_STATE *state)
{ (void)idx; if (state) memset(state, 0, sizeof(*state)); return ERROR_DEVICE_NOT_CONNECTED; }

BOOL TerminateProcess(HANDLE process, UINT exitCode)
{
    (void)process;
    exit((int)exitCode);
}

VOID OutputDebugStringW(LPCWSTR str)
{
    if (!str) return;
    for (const WCHAR *p = str; *p; p++)
        fputc((*p < 128) ? (int)*p : '?', stderr);
}

/*
 * Minimal MultiByteToWideChar / WideCharToMultiByte. Handles UTF-8 and a
 * latin-1 interpretation of CP_ACP -- enough for path/name strings.
 */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR mb, int mbCount,
                        LPWSTR wide, int wideCount)
{
    (void)flags;
    if (!mb) return 0;
    int srcLen = (mbCount < 0) ? (int)strlen(mb) + 1 : mbCount;
    int out = 0;

    for (int i = 0; i < srcLen; ) {
        unsigned int cpval;
        unsigned char c = (unsigned char)mb[i];

        if (cp == CP_UTF8 && c >= 0x80) {
            if ((c & 0xE0) == 0xC0 && i + 1 < srcLen) {
                cpval = ((c & 0x1F) << 6) | (mb[i+1] & 0x3F); i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < srcLen) {
                cpval = ((c & 0x0F) << 12) | ((mb[i+1] & 0x3F) << 6) |
                        (mb[i+2] & 0x3F); i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < srcLen) {
                cpval = ((c & 0x07) << 18) | ((mb[i+1] & 0x3F) << 12) |
                        ((mb[i+2] & 0x3F) << 6) | (mb[i+3] & 0x3F); i += 4;
            } else { cpval = c; i += 1; }
        } else {
            cpval = c; i += 1;   /* ASCII / latin-1 */
        }

        if (cpval > 0xFFFF) cpval = '?';   /* no surrogate pairs */
        if (wideCount > 0) {
            if (out >= wideCount) return 0;
            wide[out] = (WCHAR)cpval;
        }
        out++;
    }
    return out;
}

int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR wide, int wideCount,
                        LPSTR mb, int mbCount, LPCSTR defChar, PBOOL usedDef)
{
    (void)flags; (void)defChar; (void)usedDef;
    if (!wide) return 0;
    int srcLen = wideCount;
    if (srcLen < 0) { srcLen = 0; while (wide[srcLen]) srcLen++; srcLen++; }
    int out = 0;

    for (int i = 0; i < srcLen; i++) {
        unsigned int cpval = wide[i];
        char buf[4]; int n;
        if (cp == CP_UTF8 && cpval >= 0x80) {
            if (cpval < 0x800) {
                buf[0] = (char)(0xC0 | (cpval >> 6));
                buf[1] = (char)(0x80 | (cpval & 0x3F)); n = 2;
            } else {
                buf[0] = (char)(0xE0 | (cpval >> 12));
                buf[1] = (char)(0x80 | ((cpval >> 6) & 0x3F));
                buf[2] = (char)(0x80 | (cpval & 0x3F)); n = 3;
            }
        } else {
            buf[0] = (char)(cpval > 0xFF ? '?' : cpval); n = 1;
        }
        if (mbCount > 0) {
            if (out + n > mbCount) return 0;
            for (int k = 0; k < n; k++) mb[out + k] = buf[k];
        }
        out += n;
    }
    return out;
}

/* ===================================================================== */
/* File mapping (memfd-backed) -- true aliased mirror views              */
/* ===================================================================== */

/* Registry of active views: UnmapViewOfFile takes no length, so we must
 * recover the mapping length here for munmap. */
typedef struct { void *addr; size_t len; } w32_view;
static w32_view        s_views[512];
static pthread_mutex_t s_views_lock = PTHREAD_MUTEX_INITIALIZER;

void view_register(void *addr, size_t len)
{
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++)
        if (!s_views[i].addr) { s_views[i].addr = addr; s_views[i].len = len; break; }
    pthread_mutex_unlock(&s_views_lock);
}

/* Non-destructive counterpart to view_take, and interior-aware: VirtualQuery
 * is asked about addresses *within* a region at least as often as about its
 * base -- a translated guest VA lands in the middle of the 64 MB window.
 * Picks the containing region and reports where it starts. */
int view_lookup(const void *addr, void **base_out, size_t *len_out)
{
    int found = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++) {
        if (!s_views[i].addr)
            continue;
        uintptr_t lo = (uintptr_t)s_views[i].addr;
        uintptr_t hi = lo + s_views[i].len;
        if ((uintptr_t)addr >= lo && (uintptr_t)addr < hi) {
            if (base_out) *base_out = s_views[i].addr;
            if (len_out)  *len_out  = s_views[i].len;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_views_lock);
    return found;
}

size_t view_take(const void *addr)
{
    size_t len = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++)
        if (s_views[i].addr == addr) { len = s_views[i].len; s_views[i].addr = NULL; break; }
    pthread_mutex_unlock(&s_views_lock);
    return len;
}


/* ===================================================================== */
/* VirtualQuery                                                           */
/* ===================================================================== */

/*
 * Answers from the view registry rather than from a constant.
 *
 * This used to report RegionSize 0x1000, MEM_COMMIT, PAGE_READWRITE and
 * AllocationBase NULL for every address it was handed, mapped or not. Four
 * kernel entry points are built on it, and one of them chooses a deallocator
 * with it: MmFreeContiguousMemory frees via VirtualFree only when
 * AllocationBase equals the pointer, so a hardcoded NULL sent every
 * contiguous buffer -- all of which come from VirtualAlloc, i.e. mmap -- to
 * _aligned_free, which is free(). The allocator aborts on the foreign
 * pointer. MmQueryAllocationSize answered 0x1000 for everything and
 * NtQueryVirtualMemory called unmapped addresses committed and readable.
 *
 * Everything the shim maps -- VirtualAlloc and MapViewOfFileEx alike -- is in
 * the registry, so it can answer for exactly the memory it owns and say
 * MEM_FREE for the rest. Saying MEM_FREE for an address it did not map is the
 * honest answer: a host heap pointer is not a Win32 reservation, and the
 * callers that branch on this want to know which allocator owns the pointer.
 */
SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buffer, SIZE_T length)
{
    if (!buffer || length < sizeof(*buffer)) return 0;
    memset(buffer, 0, sizeof(*buffer));

    void  *base = NULL;
    size_t len  = 0;

    if (!view_lookup(address, &base, &len)) {
        /* Not ours. Report it free rather than inventing a committed page. */
        buffer->BaseAddress    = (PVOID)address;
        buffer->AllocationBase = NULL;
        buffer->RegionSize     = 0;
        buffer->State          = MEM_FREE;
        buffer->Protect        = PAGE_NOACCESS;
        buffer->Type           = 0;
        return sizeof(*buffer);
    }

    buffer->BaseAddress      = (PVOID)address;
    buffer->AllocationBase   = base;
    buffer->AllocationProtect = PAGE_READWRITE;
    /* From the queried address to the end of the region, which is what Win32
     * reports and what callers sizing a copy out of it depend on. */
    buffer->RegionSize       = len - (size_t)((uintptr_t)address - (uintptr_t)base);
    buffer->State            = MEM_COMMIT;
    buffer->Protect          = PAGE_READWRITE;
    buffer->Type             = MEM_PRIVATE;
    return sizeof(*buffer);
}

BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX b)
{
    if (!b) return FALSE;
#if defined(__SWITCH__)
    {
        u64 total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        b->ullTotalPhys = total;
        b->ullAvailPhys = total > used ? total - used : 0;
        b->ullTotalPageFile = b->ullTotalPhys;
        b->ullAvailPageFile = b->ullAvailPhys;
    }
#elif defined(__vita__)
    b->ullTotalPhys=256ull*1024*1024;b->ullAvailPhys=128ull*1024*1024;b->ullTotalPageFile=b->ullTotalPhys;b->ullAvailPageFile=b->ullAvailPhys;
#elif defined(__APPLE__)
    /* Darwin has no sysinfo(2): physical memory comes from sysctl hw.memsize,
     * swap from vm.swapusage, the free page count from the Mach VM statistics. */
    uint64_t memsize = 0;
    size_t   len     = sizeof(memsize);
    int oid_memsize[] = { CTL_HW, HW_MEMSIZE };
    if (sysctl(oid_memsize, 2, &memsize, &len, NULL, 0) != 0) return FALSE;

    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) page = 4096;

    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    ULONGLONG avail = 0;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &count) == KERN_SUCCESS)
        avail = ((ULONGLONG)vm.free_count + vm.inactive_count) * (ULONGLONG)page;

    struct xsw_usage swap;
    len = sizeof(swap);
    int oid_swapusage[] = { CTL_VM, VM_SWAPUSAGE };
    if (sysctl(oid_swapusage, 2, &swap, &len, NULL, 0) != 0)
        memset(&swap, 0, sizeof(swap));

    b->ullTotalPhys     = (ULONGLONG)memsize;
    b->ullAvailPhys     = avail;
    b->ullTotalPageFile = b->ullTotalPhys + (ULONGLONG)swap.xsu_total;
    b->ullAvailPageFile = b->ullAvailPhys + (ULONGLONG)swap.xsu_avail;
#else
    struct sysinfo si;
    if (sysinfo(&si) != 0) return FALSE;

    ULONGLONG unit = si.mem_unit ? si.mem_unit : 1;
    b->ullTotalPhys     = (ULONGLONG)si.totalram  * unit;
    b->ullAvailPhys     = (ULONGLONG)si.freeram   * unit;
    b->ullTotalPageFile = b->ullTotalPhys + (ULONGLONG)si.totalswap * unit;
    b->ullAvailPageFile = b->ullAvailPhys + (ULONGLONG)si.freeswap  * unit;
#endif
    b->ullTotalVirtual  = b->ullTotalPhys;
    b->ullAvailVirtual  = b->ullAvailPhys;
    b->ullAvailExtendedVirtual = 0;
    b->dwMemoryLoad = b->ullTotalPhys
        ? (DWORD)(100 - (b->ullAvailPhys * 100 / b->ullTotalPhys)) : 0;
    return TRUE;
}

/* ===================================================================== */
/* Aligned allocation                                                     */
/* ===================================================================== */

void *_aligned_malloc(SIZE_T size, SIZE_T alignment)
{
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    /* round alignment up to a power of two */
    SIZE_T a = sizeof(void *);
    while (a < alignment) a <<= 1;
    void *p = NULL;
#if defined(__SWITCH__) || defined(__vita__)
    p = memalign(a, size ? size : 1);   /* newlib has no posix_memalign */
    if (!p) return NULL;
#else
    if (posix_memalign(&p, a, size ? size : 1) != 0) return NULL;
#endif
    return p;
}

void _aligned_free(void *ptr) { free(ptr); }

/* ===================================================================== */
/* Case-insensitive string compare                                        */
/* ===================================================================== */

int _stricmp(const char *a, const char *b)            { return strcasecmp(a, b); }
int _strnicmp(const char *a, const char *b, SIZE_T n) { return strncasecmp(a, b, n); }

/* ===================================================================== */
/* Wide-string helpers (16-bit Xbox WCHAR)                                */
/* ===================================================================== */

SIZE_T xbox_wcslen(const WCHAR *s)
{
    SIZE_T n = 0;
    if (s) while (s[n]) n++;
    return n;
}

int xbox_wcsncmp(const WCHAR *a, const WCHAR *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        if (a[i] == 0)    return 0;
    }
    return 0;
}

WCHAR *xbox_wcscat(WCHAR *dst, const WCHAR *src)
{
    SIZE_T d = xbox_wcslen(dst), i = 0;
    while (src[i]) { dst[d + i] = src[i]; i++; }
    dst[d + i] = 0;
    return dst;
}

WCHAR *xbox_wcscpy(WCHAR *dst, const WCHAR *src)
{
    SIZE_T i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return dst;
}

/* ===================================================================== */
/* Time conversion                                                        */
/* ===================================================================== */

#if defined(__SWITCH__) || defined(__vita__)
/* newlib has no timegm. UTC calendar date to seconds since 1970, from the
 * civil-from-days inverse (valid for the proleptic Gregorian calendar). */
static time_t timegm(const struct tm *tm)
{
    long long y = (long long)tm->tm_year + 1900, m = tm->tm_mon + 1, d = tm->tm_mday;
    long long era, yoe, doy, doe, days;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}
#endif

BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon  = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min  = st->wMinute;
    tm.tm_sec  = st->wSecond;
    time_t t = timegm(&tm);
    ULONGLONG ticks = FILETIME_EPOCH_DIFF
                    + (ULONGLONG)t * 10000000ULL
                    + (ULONGLONG)st->wMilliseconds * 10000ULL;
    ft->dwLowDateTime  = (DWORD)(ticks & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(ticks >> 32);
    return TRUE;
}

BOOL FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st)
{
    ULONGLONG ticks = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (ticks < FILETIME_EPOCH_DIFF) { memset(st, 0, sizeof(*st)); return FALSE; }
    ULONGLONG since = ticks - FILETIME_EPOCH_DIFF;
    time_t t = (time_t)(since / 10000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    fill_systemtime(st, &tm, (long)((since % 10000000ULL) * 100ULL));
    return TRUE;
}

/* ===================================================================== */
/* Exception handling (compile-shim -- SEH not yet emulated on Linux)     */
/* ===================================================================== */

VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
               PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue)
{
    (void)TargetFrame; (void)TargetIp; (void)ExceptionRecord; (void)ReturnValue;
    /* TODO: Windows SEH unwinding is not yet emulated on Linux. */
}

VOID RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args)
{
    (void)flags; (void)nargs; (void)args;
    fprintf(stderr, "[win32_compat] RaiseException(0x%08X): SEH not emulated\n", code);
    /* TODO: on Windows this does not return; SEH dispatch unimplemented. */
}

PVOID AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler)
{ (void)First; (void)Handler; return NULL; }   /* TODO: wire to sigaction */
ULONG RemoveVectoredExceptionHandler(PVOID h) { (void)h; return 1; }

#endif /* !_WIN32 */
