/* sig_guest.c — a guest signal handler is a guest function: install a native entry for it, never the arm64 address.
   reuses: the callback pool (cb_host.c __arm2x86_cb: one native slot per guest function, cb_door runs it at __arm_sp),
     host.c a2x_late_wrapper (sigaction/signal binds land here).
   new-because: without it, an unmodified macOS 27 sshd listener serves two connections and then never
     accepts again, and ignores SIGTERM. `sample` on the listener: 2,559 of 2,559 samples with the native pc exactly at
     guest 0x10000de98 = sshd's sigchld_handler (`mov w8,#1; adrp; str w8,[x9,#0x358]; ret`). The guest's sigaction()
     reached native libc with the arm64 address inside struct sigaction — __arm2x86_cbarg only sees arguments, not a
     pointer inside a struct — so the kernel jumped into arm64 bytes on a sealed code page and the fault re-ran forever.
     Any daemon with a SIGCHLD/SIGTERM/SIGHUP handler is exposed (launchd's children, sshd, cupsd, ...).

   The entry (a2x_sig_entry) runs the guest handler through its callback slot on a per-thread guest signal stack:
      __arm_sp is only the guest's real sp while the thread is inside a native call; a signal landing mid-instruction
      would otherwise start the handler on top of live guest frames. Every interpreter thread-local a nested run can
      write is saved and restored, and errno too, so the interrupted run continues untouched.
   Host-owned signals stay the host's: SIGSEGV, SIGBUS, SIGILL and SIGTRAP carry the code seal, guest-call recovery
      and the crash report. A guest asking for them is remembered (oact answers truthfully) but never installed.
      (The guest's crash handler therefore never runs: the host's report stands in for it.)
   SA_ONSTACK is dropped: a guest sigaltstack is sized for an arm64 handler, not for the interpreter's frames.
   The host's fault signals are removed from every mask, so the seal keeps working inside a handler.
   Not covered: sigvec, sigset, bsd_signal, sigignore (rare); a SA_SIGINFO handler gets the native ucontext (its
      mcontext is x86 — reading registers from it is wrong; siginfo_t itself is identical on both).
   ARM2X86_NO_GUESTSIG=1 turns it off (binds go native exactly as before); ARM2X86_TRACE_SIG=1 traces installs.
*/
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

extern unsigned long __arm2x86_cb(unsigned long fn);
extern __thread unsigned long __arm_sp;
extern __thread unsigned long __arm_xr[96];
extern __thread unsigned long a64_cur_pc;
extern __thread uint32_t a64_cur_insn;
extern __thread const void *a64_cur_state;
extern __thread unsigned long __arm2x86_interp_target;
extern __thread unsigned char __arm2x86_rf[208];
extern __thread unsigned long __arm2x86_rfall[31];
extern __thread const unsigned long *a64_va_ovf;

#define SIGSTK (1UL << 20)          /* chosen: 1 MB of guest stack per thread that takes a signal; handlers are small */

static struct sigaction g_guest[NSIG];   /* what the guest asked for, as it asked (oact answers from this) */
static unsigned char g_has[NSIG];        /* 1 = g_guest[sig] holds the guest's current disposition */
static unsigned long g_tramp[NSIG];      /* the native slot for the guest handler, 0 = none */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread unsigned long t_stk_lo, t_stk_hi;

static int host_owned(int s) { return s == SIGSEGV || s == SIGBUS || s == SIGILL || s == SIGTRAP; }
static int str_(void) { static int t = -1; if (t < 0) t = A2X_DBGENV("ARM2X86_TRACE_SIG") != 0; return t; }

static void a2x_sig_entry(int sig, siginfo_t *si, void *uc) {
    unsigned long t = (sig > 0 && sig < NSIG) ? g_tramp[sig] : 0;
    if (!t) return;
    int e = errno;
    /* ── save every thread-local a nested interpreter run may write ── */
    unsigned long sp = __arm_sp, pc = a64_cur_pc, tgt = __arm2x86_interp_target;
    uint32_t insn = a64_cur_insn;
    const void *st = a64_cur_state;
    const unsigned long *ovf = a64_va_ovf;
    unsigned long xr[96], rfall[31];
    unsigned char rf[208];
    memcpy(xr, __arm_xr, sizeof xr); memcpy(rfall, __arm2x86_rfall, sizeof rfall); memcpy(rf, __arm2x86_rf, sizeof rf);

    if (!t_stk_lo) {                                   /* mmap is a system call: safe in a handler */
        void *p = mmap(0, SIGSTK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p != MAP_FAILED) { t_stk_lo = (unsigned long)(uintptr_t)p; t_stk_hi = t_stk_lo + SIGSTK; }
    }
    if (t_stk_lo && !(sp > t_stk_lo && sp <= t_stk_hi))
        __arm_sp = (t_stk_hi - 4096) & ~15UL;            /* first level: the top of this thread's signal stack */
    else
        __arm_sp = ((sp ? sp : t_stk_hi) - 16384) & ~15UL; /* nested inside a guest handler: below the inner frames */

    ((void (*)(long, long, long))(uintptr_t)t)((long)sig, (long)(uintptr_t)si, (long)(uintptr_t)uc);

    memcpy(__arm_xr, xr, sizeof xr); memcpy(__arm2x86_rfall, rfall, sizeof rfall); memcpy(__arm2x86_rf, rf, sizeof rf);
    a64_va_ovf = ovf; a64_cur_state = st; a64_cur_insn = insn; __arm2x86_interp_target = tgt; a64_cur_pc = pc;
    __arm_sp = sp;
    errno = e;
}

static int w_sigaction(int sig, const struct sigaction *act, struct sigaction *oact) {
    if (sig <= 0 || sig >= NSIG) return sigaction(sig, act, oact);   /* native says EINVAL exactly as before */
    pthread_mutex_lock(&g_mu);
    struct sigaction prev_guest = g_guest[sig];
    int had = g_has[sig];
    int r = 0;
    if (host_owned(sig)) {
        /* remembered, never installed: the host's handler is the seal and the crash report */
        if (act) {
            g_guest[sig] = *act; g_has[sig] = 1;
            if (str_()) fprintf(stderr, "[guest-sig] %d: host-owned, guest handler %p remembered, not installed\n",
                                sig, (void *)act->sa_handler);
        }
        if (oact) {
            if (had) *oact = prev_guest;
            else { memset(oact, 0, sizeof *oact); oact->sa_handler = SIG_DFL; }
        }
        pthread_mutex_unlock(&g_mu);
        return 0;
    }
    struct sigaction nat, old;
    const struct sigaction *use = act;
    unsigned long h = act ? (unsigned long)(uintptr_t)act->sa_handler : 0, t = 0;
    if (act && h > 1 /* not SIG_DFL / SIG_IGN */ && (t = __arm2x86_cb(h)) != h) {
        nat = *act;
        nat.sa_sigaction = a2x_sig_entry;
        nat.sa_flags = (act->sa_flags | SA_SIGINFO) & ~SA_ONSTACK;
        sigdelset(&nat.sa_mask, SIGSEGV); sigdelset(&nat.sa_mask, SIGBUS);
        sigdelset(&nat.sa_mask, SIGILL); sigdelset(&nat.sa_mask, SIGTRAP);
        g_tramp[sig] = t;                              /* before the native install: a signal may arrive at once */
        use = &nat;
    } else {
        t = 0;
    }
    r = sigaction(sig, use, &old);
    if (r == 0 && act) {
        g_tramp[sig] = t;
        if (t) { g_guest[sig] = *act; g_has[sig] = 1; } else g_has[sig] = 0;
        if (str_()) fprintf(stderr, "[guest-sig] %d: %s %p%s\n", sig, t ? "guest handler" : "native disposition",
                            (void *)h, t ? " via its callback slot on a per-thread guest signal stack" : "");
    } else if (r != 0 && act && !had) {
        g_tramp[sig] = 0;                              /* the install failed: nothing of ours is live */
    }
    if (r == 0 && oact) *oact = had ? prev_guest : old;
    pthread_mutex_unlock(&g_mu);
    return r;
}

static void (*w_signal(int sig, void (*h)(int)))(int) {
    struct sigaction sa, o;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sa.sa_flags = SA_RESTART;                          /* libc signal(): BSD semantics, restartable */
    if (w_sigaction(sig, &sa, &o) != 0) return SIG_ERR;
    return o.sa_handler;
}

/* host.c a2x_late_wrapper: sigaction / signal -> the wrappers above; anything else -> 0 */
void *a2x_sig_wrapper(const char *nm) {
    if (!nm || nm[0] != 's' || A2X_DBGENV("ARM2X86_NO_GUESTSIG")) return 0;
    if (!strcmp(nm, "sigaction")) return (void *)w_sigaction;
    if (!strcmp(nm, "signal")) return (void *)w_signal;
    return 0;
}
