/* spawn_arch.c — a guest's spawn arch preference always ends in x86_64: on Intel the x86_64 slice of a host fat is the
   interpreter that runs its arm64e slice.
   reuses: host.c a2x_late_wrapper (posix_spawnattr_setarchpref_np / setbinpref_np binds land here, like sig_guest.c).
   new-because: on an unmodified macOS 27 system, xpcproxy never exec'd one host-fat daemon
     (logd, configd, diskarbitrationd, sshd...: 0 host lines in ~10,000 xpcproxy launches) — no network, no ssh.
     xpcproxy calls posix_spawnattr_setarchpref_np(psattr, xb_count, xb_cpu_types, xb_cpu_subtypes) with launchd's arm64
     list. On an x86 kernel, posix_spawn of the sshd host fat: pref arm64/arm64e -> 86 Bad CPU type,
     arm64/any -> 86, arm64e then x86_64 -> ok, x86_64 -> ok, no pref -> ok.
   The guest's own entries stay first and in order (an arm64 kernel would honour them); x86_64/any is appended when the
     list has no x86_64 entry. A list already at the kernel's 4 slots gets its last slot replaced.
   Nothing is lost: a thin arm64e target was EBADARCH before and is EBADARCH now; a native fat picks its Apple x86_64
     slice exactly as with no preference.
   ARM2X86_NO_SPAWNARCH=1 turns it off; ARM2X86_TRACE_SPAWNARCH=1 traces each rewrite.
*/
#include <mach/machine.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NPREF 4   /* xnu NBINPREFS: posix_spawnattr holds at most 4 entries */

static int trace_(void) { static int t = -1; if (t < 0) t = A2X_DBGENV("ARM2X86_TRACE_SPAWNARCH") != 0 || getppid() == 1; return t; }   /* xpcproxy always traces */

/* copy the guest list (at most NPREF), then make sure x86_64/ANY is in it; returns the new count */
static size_t widen(size_t n, const cpu_type_t *ct, const cpu_subtype_t *st, cpu_type_t *oct, cpu_subtype_t *ost) {
    size_t k = n < NPREF ? n : NPREF, i;
    int has = 0;
    for (i = 0; i < k; i++) {
        oct[i] = ct ? ct[i] : 0;
        ost[i] = st ? st[i] : CPU_SUBTYPE_ANY;
        if (oct[i] == CPU_TYPE_X86_64) has = 1;
    }
    if (!has) {
        if (k == NPREF) k--;          /* full: the last guest entry gives way */
        oct[k] = CPU_TYPE_X86_64; ost[k] = CPU_SUBTYPE_ANY; k++;
    }
    if (trace_()) {
        fprintf(stderr, "[spawn-arch] guest %zu pref(s):", n);
        for (i = 0; i < (n < NPREF ? n : NPREF); i++) fprintf(stderr, " %#x/%#x", ct ? ct[i] : 0, st ? st[i] : -1);
        fprintf(stderr, "%s\n", has ? " (has x86_64, unchanged)" : " -> + x86_64/any");
    }
    return k;
}

static int w_setarchpref(posix_spawnattr_t *a, size_t n, cpu_type_t *ct, cpu_subtype_t *st, size_t *ocount) {
    cpu_type_t oct[NPREF]; cpu_subtype_t ost[NPREF];
    if (n == 0) return posix_spawnattr_setarchpref_np(a, n, ct, st, ocount);
    size_t k = widen(n, ct, st, oct, ost), got = 0;
    int r = posix_spawnattr_setarchpref_np(a, k, oct, ost, &got);
    if (ocount) *ocount = got < n ? got : n;   /* the guest asked to set n; report how many of its entries took */
    return r;
}

static int w_setbinpref(posix_spawnattr_t *a, size_t n, cpu_type_t *ct, size_t *ocount) {
    cpu_type_t oct[NPREF]; cpu_subtype_t ost[NPREF];
    if (n == 0) return posix_spawnattr_setbinpref_np(a, n, ct, ocount);
    size_t k = widen(n, ct, NULL, oct, ost), got = 0;
    int r = posix_spawnattr_setbinpref_np(a, k, oct, &got);
    if (ocount) *ocount = got < n ? got : n;
    return r;
}

/* A failed spawn says why, on the console. With x86_64 in the preference xpcproxy could still die for every
   daemon but kernelmanagerd, silently (no exit, no abort line) — the spawn result was invisible. Children of PID 1
   (xpcproxy) and PID 1 print path, errno and spawn flags when posix_spawn/posix_spawnp fails; success with
   POSIX_SPAWN_SETEXEC never returns, so a working exec prints nothing. Behaviour is unchanged: the real call's result
   is returned as is. */
#include <unistd.h>
static int spawn_diag_on(void) { return getpid() == 1 || getppid() == 1; }
static void spawn_diag(const char *fn, const char *path, const posix_spawnattr_t *a, int r) {
    short fl = 0;
    if (a) posix_spawnattr_getflags(a, &fl);
    fprintf(stderr, "[host] %s(%s) -> %d pid %d flags %#x\n", fn, path ? path : "(null)", r, (int)getpid(), fl);
}
/* xpcproxy's ~270 SETEXEC spawns returned no error and never reached the host: killed after the exec
   commit. xpcproxy attaches an AMFI launch constraint (Apple platform binary) that our ad-hoc host fat cannot meet.
   The guest's constraint is not attached (logged once per process); every spawn path is logged before the call. */
static int w_amfi_lc(posix_spawnattr_t *a, int which, const void *data, size_t len) {
    (void)a; (void)data;
    if (spawn_diag_on()) fprintf(stderr, "[host] launch constraint %d (%zu B) not attached pid %d\n", which, len, (int)getpid());
    return 0;
}
static int w_posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa,
                         const posix_spawnattr_t *a, char *const argv[], char *const envp[]) {
    if (spawn_diag_on()) fprintf(stderr, "[host] spawn %s pid %d\n", path ? path : "(null)", (int)getpid());
    int r = posix_spawn(pid, path, fa, a, argv, envp);
    if (r && spawn_diag_on()) spawn_diag("posix_spawn", path, a, r);
    return r;
}
static int w_posix_spawnp(pid_t *pid, const char *file, const posix_spawn_file_actions_t *fa,
                          const posix_spawnattr_t *a, char *const argv[], char *const envp[]) {
    if (spawn_diag_on()) fprintf(stderr, "[host] spawn %s pid %d\n", file ? file : "(null)", (int)getpid());
    int r = posix_spawnp(pid, file, fa, a, argv, envp);
    if (r && spawn_diag_on()) spawn_diag("posix_spawnp", file, a, r);
    return r;
}

/* xpcproxy's SETEXEC spawn of logd/configd/... can return 0 while the new image never runs (444 logd
   spawns, 0 host lines); kernelmanagerd alone survived, and the launch-constraint setter was never called. Every other
   spawnattr setter xpcproxy calls is logged with its first three arguments, and the arm64-only / sandbox ones are
   not applied (marked "skip"): on the x86 kernel each can kill the image after the exec commit. The log names the
   setter that kills it, so only that one needs to stay skipped. */
#include <dlfcn.h>
typedef long (*a2x_f6)(long, long, long, long, long, long);
#define SA_SKIP 1
static const struct { const char *nm; int fl; } sa_tab[] = {
    {"sandbox_spawnattrs_setprofilename", SA_SKIP}, {"sandbox_spawnattrs_setcontainer", SA_SKIP},
    {"posix_spawnattr_disable_ptr_auth_a_keys_np", SA_SKIP}, {"posix_spawnattr_set_use_sec_transition_shims_np", SA_SKIP},
    {"posix_spawnattr_set_max_addr_np", SA_SKIP}, {"posix_spawnattr_set_shared_region_config_np", SA_SKIP},
    {"posix_spawnattr_set_subsystem_root_path_np", 0}, {"posix_spawnattr_set_launch_type_np", 0},
    {"posix_spawnattr_set_platform_np", 0}, {"posix_spawnattr_setmacpolicyinfo_np", 0},
    {"posix_spawnattr_set_use_sanitizers_np", 0}, {"posix_spawnattr_setflags", 0},
    {"responsibility_spawnattrs_setdisclaim", 0}, {"sandbox_spawnattrs_init", 0},
};
#define SA_N (int)(sizeof sa_tab / sizeof sa_tab[0])
static long sa_call(int i, long a, long b, long c, long d, long e, long f) {
    int skip = (sa_tab[i].fl & SA_SKIP) != 0;
    if (spawn_diag_on()) fprintf(stderr, "[host] sa %s(%#lx, %#lx, %#lx)%s pid %d\n", sa_tab[i].nm, a, b, c,
                                 skip ? " skip" : "", (int)getpid());
    if (skip) return 0;
    a2x_f6 fn = (a2x_f6)dlsym(RTLD_DEFAULT, sa_tab[i].nm);
    return fn ? fn(a, b, c, d, e, f) : 0;
}
#define SA_W(i) static long sa_w##i(long a, long b, long c, long d, long e, long f) { return sa_call(i, a, b, c, d, e, f); }
SA_W(0) SA_W(1) SA_W(2) SA_W(3) SA_W(4) SA_W(5) SA_W(6) SA_W(7) SA_W(8) SA_W(9) SA_W(10) SA_W(11) SA_W(12) SA_W(13)
static void *const sa_fns[] = { sa_w0, sa_w1, sa_w2, sa_w3, sa_w4, sa_w5, sa_w6, sa_w7, sa_w8, sa_w9, sa_w10, sa_w11,
                                sa_w12, sa_w13 };
_Static_assert(sizeof sa_fns / sizeof sa_fns[0] == sizeof sa_tab / sizeof sa_tab[0], "sa table");

/* host.c a2x_late_wrapper: the two arch-preference setters + the two spawns -> the wrappers above; anything else -> 0 */
void *a2x_spawnarch_wrapper(const char *nm) {
    if (!nm || A2X_DBGENV("ARM2X86_NO_SPAWNARCH")) return 0;
    if (!strcmp(nm, "amfi_launch_constraint_set_spawnattr")) return (void *)w_amfi_lc;
    for (int i = 0; i < SA_N; i++) if (!strcmp(nm, sa_tab[i].nm)) return sa_fns[i];
    if (nm[0] != 'p') return 0;
    if (!strcmp(nm, "posix_spawnattr_setarchpref_np")) return (void *)w_setarchpref;
    if (!strcmp(nm, "posix_spawnattr_setbinpref_np")) return (void *)w_setbinpref;
    if (!strcmp(nm, "posix_spawn")) return (void *)w_posix_spawn;
    if (!strcmp(nm, "posix_spawnp")) return (void *)w_posix_spawnp;
    return 0;
}
