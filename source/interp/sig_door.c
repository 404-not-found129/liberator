// sig_door.c — the typed door: a native call placed from the callee's real signature, not from a fixed shape.
//
// Why a third door. native_door.ll carries one shape for every native callee the host has ever seen:
//    x0-x7 as eight integers, v0-v7 as eight doubles, 16 words blind-copied off the arm64 stack, and a
//    return of {rax, rdx, xmm0, xmm1}. That is right for scalar arguments with an integer or double result
//    and it is wrong, silently, for the rest — and "silently" is the expensive part, because the call
//    returns, the program continues, and the wrong value surfaces somewhere else.
//    The per-app translator never guessed: sigdb.json holds clang's own declaration for every import and
//    arm2x86.py:bind() emits a call with the real C types. The host has no compiler at run time, so it
//    reuses the data (sig_table.h, generated from sigdb.json by mksigtable.py)
//    and lets libffi do the x86-64 classification it was written for. libffi is already linked and
//    objc_bridge.c places every non-trivial message send through it.
//
// What this closes, counted over the 65 macOS 27 system apps:
//      · a spilled argument. 14 imported symbols (29 call slots): UCKeyTranslate, DNSServiceRegister,
//        CGImageCreate, CCKeyDerivationPBKDF, vm_remap … AAPCS64-Darwin packs an argument past x7/v7 on
//        the arm64 stack at its natural alignment, so a short or an int there leaves the following words
//        un-aligned with x86's 8-byte slots and the blind 16-word copy shifts everything after it.
//      · a narrowing return. 118 imported symbols (309 call slots): CFEqual, CFDictionaryContainsKey,
//        CFBooleanGetValue, CFStringHasPrefix … x86-64 SysV leaves bits 8..63 of rax undefined for a
//        Boolean (= unsigned char) return; arm64 callers read w0 whole, because clang emits `cbz w0`. So a
//        previous computation's leftovers decide the branch. arm2x86.py narrows those, and
//        the narrowing table is read out of its own NARROW_RET dict, never retyped.
//
// What it refuses, and refusing is the point. No sigdb entry, an 'x' (sigdb could not type a parameter or
//    the return), variadic (vararg.c owns those and solves them from the format string), a spilled argument
//    with no known size, more than SIG_MAXARGS arguments, or a full plan cache — every one of those returns
//    0 and the caller takes the existing untyped door, unchanged. Strictly additive: when there is no
//    signature nothing moves.
//
// And a call it cannot type is named, not quietly mis-placed. A callee whose return sigdb marked 'x' may
//    return a struct larger than 16 bytes: arm64 puts the caller's result buffer in x8, x86-64 puts it in
//    rdi, which shifts every integer argument by one register. The fixed-shape door runs it anyway and says
//    nothing. A64_SIG_X[] (1,766 names) lets this file say the name the first time it sees one.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <ffi/ffi.h>
#include "sig_table.h"

extern FILE *a2x_errf(void);
/* an argument that is an exact guest function start is a CALLBACK: native code must receive a
      native-callable trampoline, never an arm64 address. Identity for everything else (host.c:87). */
extern unsigned long __arm2x86_cbarg(unsigned long v);

#define SIG_MAXARGS 32
#define SIG_PLANS 1024                 /* power of two: the cif cache is masked, not divided */
#define SIG_NAMED_MAX 64               /* how many untypeable callees get named before the summary line */

/* ARM2X86_SIGDOOR: 0 = off (the untyped door only), unset = only the calls the fixed shape gets wrong,
   all = every symbol sigdb can type.
   "all" is the proof switch, the same idea as ARM2X86_INTERP_FORCE: by default this door takes 727 of
      9,581 typed symbols, so on a program that works it would almost never fire and could ship unproven.
      With =all the same binary runs both ways and any difference is this door's. */
static int sig_mode(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = A2X_DBGENV("ARM2X86_SIGDOOR");
        v = !e ? 1 : (!strcmp(e, "0") ? 0 : (!strcmp(e, "all") ? 2 : 1));
    }
    return v;
}
static int sig_trace(void) {
    static int v = -1;
    if (v < 0) v = A2X_DBGENV("ARM2X86_TRACE_SIGDOOR") ? 1 : 0;
    return v;
}

/* The table is binary-searched, so a generator that emitted it out of order would make every entry past
      the first break invisible — a typed door that silently types nothing. mksigtable.py asserts the order
      when it writes the header; this asserts it once at run time, because the header on disk and the header
      this binary was compiled against are not necessarily the same file. */
static void sig_check_sorted(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    for (unsigned k = 1; k < A64_SIG_N; k++)
        if (strcmp(A64_SIG[k - 1].name, A64_SIG[k].name) >= 0) {
            fprintf(a2x_errf(), "[sigdoor] sig_table.h is not sorted at %u (%s then %s) — the typed door is "
                    "DISABLED rather than typing an arbitrary subset\n", k, A64_SIG[k - 1].name, A64_SIG[k].name);
            a2x_stop("internal table out of order", "signatures"); abort();
        }
    for (unsigned k = 1; k < A64_SIG_X_N; k++)
        if (strcmp(A64_SIG_X[k - 1].name, A64_SIG_X[k].name) >= 0) {
            fprintf(a2x_errf(), "[sigdoor] A64_SIG_X is not sorted at %u\n", k);
            a2x_stop("internal table out of order", "extra signatures"); abort();
        }
}

static const struct a64_sig *sig_find(const char *n) {
    unsigned lo = 0, hi = A64_SIG_N;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(A64_SIG[mid].name, n);
        if (c == 0) return &A64_SIG[mid];
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* Compiler prologues cannot override an authoritative fixed C declaration. */
int __arm2x86_sig_is_fixed(const char *name) {
    if (!name) return 0;
    const struct a64_sig *s = sig_find(name[0] == '_' ? name + 1 : name);
    return s && !(s->flags & SIGF_VA);
}

/* Name the calls that are wrong. Once per symbol, capped, on stderr by default: a known-wrong call that
      prints nothing is indistinguishable from a correct one, and that is how several of these holes
      stayed invisible. */
static void sig_name_untypeable(const char *n) {
    static const char *named[SIG_NAMED_MAX];
    static unsigned nnamed, nsuppressed;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    unsigned lo = 0, hi = A64_SIG_X_N;
    const struct a64_sig_x *x = 0;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(A64_SIG_X[mid].name, n);
        if (c == 0) { x = &A64_SIG_X[mid]; break; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    if (!x) return;                    /* not in sigdb at all: nothing is known, so there is nothing to say */
    pthread_mutex_lock(&m);
    for (unsigned k = 0; k < nnamed; k++)
        if (named[k] == x->name) { pthread_mutex_unlock(&m); return; }
    if (nnamed < SIG_NAMED_MAX) {
        named[nnamed++] = x->name;
        const char *what = x->which == 'r' ? "its return type"
                         : x->which == 'a' ? "one of its arguments" : "its return type and an argument";
        fprintf(a2x_errf(), "[sigdoor] %s: sigdb cannot type %s, so this call goes through the fixed-shape "
                "door. A by-value struct or long double there is placed WRONG (a return over 16 bytes wants "
                "x8 on arm64 and rdi on x86, which shifts every integer argument by one).\n", n, what);
    } else if (++nsuppressed == 1) {
        fprintf(a2x_errf(), "[sigdoor] further untypeable callees are not being named (cap %d)\n", SIG_NAMED_MAX);
    }
    pthread_mutex_unlock(&m);
}

/* ── the cif cache ────────────────────────────────────────────────────────────────────────────────────────
   Keyed on the table row, not on the native address: two stubs can reach one function, and the row is
      what decides the cif. ffi_prep_cif keeps the ffi_type array pointer, so at[] lives in the cache entry. */
struct sig_plan {
    const struct a64_sig *s;
    ffi_cif cif;
    ffi_type *at[SIG_MAXARGS];
};
static struct sig_plan *g_plan[SIG_PLANS];
static pthread_mutex_t g_planm = PTHREAD_MUTEX_INITIALIZER;

static unsigned sig_hash(const struct a64_sig *s) {
    uint64_t h = (uint64_t)(uintptr_t)s * 0x9E3779B97F4A7C15ULL;
    return (unsigned)((h >> 32) ^ h);
}

static ffi_type *sig_rtype(unsigned char ret) {
    switch (ret) {
    case SIG_VOID: return &ffi_type_void;
    /* i32 comes back as a 64-bit ffi_arg whatever type is named here, and the narrowing below is what
          decides the bits that reach x0 — so sint64 is used for both integer widths deliberately. */
    case SIG_I32: case SIG_I64: return &ffi_type_sint64;
    case SIG_F32: return &ffi_type_float;
    default: return &ffi_type_double;
    }
}

static struct sig_plan *sig_plan(const struct a64_sig *s) {
    unsigned h = sig_hash(s) & (SIG_PLANS - 1);
    for (unsigned i = 0; i < 8; i++) {
        struct sig_plan *p = __atomic_load_n(&g_plan[(h + i) & (SIG_PLANS - 1)], __ATOMIC_ACQUIRE);
        if (!p) break;                 /* an empty slot ends the probe: an insert never skips one */
        if (p->s == s) return p;
    }
    pthread_mutex_lock(&g_planm);
    unsigned slot = SIG_PLANS;
    for (unsigned i = 0; i < 8; i++) {
        unsigned k = (h + i) & (SIG_PLANS - 1);
        struct sig_plan *p = g_plan[k];
        if (!p) { slot = k; break; }
        if (p->s == s) { pthread_mutex_unlock(&g_planm); return p; }
    }
    if (slot == SIG_PLANS) { pthread_mutex_unlock(&g_planm); return 0; }   /* full: the caller falls back */
    struct sig_plan *p = (struct sig_plan *)calloc(1, sizeof *p);
    if (!p) { pthread_mutex_unlock(&g_planm); return 0; }
    p->s = s;
    for (unsigned i = 0; i < s->argc; i++)
        /* Every INTEGER argument is one EIGHTBYTE on x86-64 SysV, whatever its declared width: a `char`
              parameter occupies a whole register or a whole 8-byte stack slot and the callee reads the low
              bits. So one integer ffi_type is correct for all of them, and `sizes` is needed only on the
              reading side — the arm64 stack, which packs at natural alignment. */
        p->at[i] = s->kinds[i] == 'i' ? &ffi_type_uint64
                 : s->kinds[i] == 'f' ? &ffi_type_float : &ffi_type_double;
    if (ffi_prep_cif(&p->cif, FFI_DEFAULT_ABI, s->argc, sig_rtype(s->ret), p->at) != FFI_OK) {
        free(p);
        pthread_mutex_unlock(&g_planm);
        return 0;
    }
    __atomic_store_n(&g_plan[slot], p, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_planm);
    return p;
}

/* Can the typed door take this callee at all, from its name alone: the same four early refusals as
      __arm2x86_sig_door below, in the same order, without the call. The interpreter caches the verdict per
      target address and skips the door when it is 0. The only side effect, naming an untypeable callee, is
      once per symbol already, so running it here (once per target) changes nothing. With the trace on the
      answer is always 1, so every refusal still prints. */
int __arm2x86_sig_applies(const char *name) {
    if (!name) return 0;
    if (sig_trace()) return 1;
    int mode = sig_mode();
    if (!mode) return 0;
    sig_check_sorted();
    const char *n = name[0] == '_' ? name + 1 : name;
    const struct a64_sig *s = sig_find(n);
    if (!s) { sig_name_untypeable(n); return 0; }
    if (s->flags & SIGF_VA) return 0;
    if (mode == 1 && !(s->flags & SIGF_NEED)) return 0;
    if (s->argc > SIG_MAXARGS) return 0;
    return 1;
}

/* ── the door ─────────────────────────────────────────────────────────────────────────────────────────────
   Returns 1 when it placed and made the call (xs/vs hold the arm64 result), 0 when it refused — and a
   refusal means the caller runs the existing untyped door with nothing changed.
     name : the symbol at tgt, as a64_native_name() reports it (leading '_' tolerated)
     xs   : x0..x30 then sp, 32 slots            vs : v0..v31, 16 bytes each
     asp  : the arm64 sp at the call — where a spilled argument lives                                      */
int __arm2x86_sig_door(const char *name, unsigned long tgt, unsigned long *xs, unsigned char *vs,
                       unsigned long asp) {
    if (!name || !tgt) return 0;
    int mode = sig_mode();
    if (!mode) return 0;
    sig_check_sorted();
    const char *n = name[0] == '_' ? name + 1 : name;
    const struct a64_sig *s = sig_find(n);
    if (!s) {
        /* not typed. If sigdb refused the type rather than never having seen it, say so once. */
        sig_name_untypeable(n);
        if (sig_trace()) fprintf(a2x_errf(), "[sigdoor] %s: no signature, fixed-shape door\n", n);
        return 0;
    }
    if (s->flags & SIGF_VA) {
        /* vararg.c already solves these from the format string and refuses the ones it does not know */
        if (sig_trace()) fprintf(a2x_errf(), "[sigdoor] %s: variadic, left to vararg.c\n", n);
        return 0;
    }
    if (mode == 1 && !(s->flags & SIGF_NEED)) {
        if (sig_trace()) fprintf(a2x_errf(), "[sigdoor] %s: fixed shape carries it, not typed "
                                 "(ARM2X86_SIGDOOR=all to type it anyway)\n", n);
        return 0;
    }
    if (s->argc > SIG_MAXARGS) {
        if (sig_trace()) fprintf(a2x_errf(), "[sigdoor] %s: %u arguments is over the cap\n", n, s->argc);
        return 0;
    }

    union sig_val { unsigned long u; double d; float f; } val[SIG_MAXARGS];
    void *av[SIG_MAXARGS];
    unsigned ni = 0, nf = 0;
    unsigned long so = 0;              /* ONE offset for every spilled argument, in declaration order —
                                             integer and floating arguments share the arm64 stack area even
                                             though they do not share a register sequence. */
    for (unsigned i = 0; i < s->argc; i++) {
        char k = s->kinds[i];
        unsigned sz = (unsigned)(s->sizes[i] - '0');
        int spilled = (k == 'i') ? (ni >= 8) : (nf >= 8);
        const void *sp = 0;
        if (spilled) {
            if (!asp || !sz) {
                /* a spilled argument with no stack to read it from, or no size to read it with. The
                      per-app bind raises NotImplementedError here rather than guessing; the host's
                      equivalent is to hand the call back to the untyped door and say which argument. */
                if (sig_trace()) fprintf(a2x_errf(), "[sigdoor] %s: argument %u spills but asp=0x%lx size=%u\n",
                                         n, i, asp, sz);
                return 0;
            }
            unsigned long off = (so + sz - 1) / sz * sz;      /* natural alignment, as arm2x86.from_stack */
            so = off + sz;
            sp = (const void *)(uintptr_t)(asp + off);
        }
        if (k == 'i') {
            unsigned long v;
            if (!spilled) v = xs[ni];
            else {
                /* Read exactly sz BYTES. The whole reason a spilled argument needs its size is that
                      arm64 packed it at natural alignment: reading 8 bytes for a `short` would also
                      swallow the next argument and leave every one after it off by its own width. */
                uint64_t w = 0;
                switch (sz) {
                case 1: w = *(const uint8_t *)sp; break;
                case 2: w = *(const uint16_t *)sp; break;
                case 4: w = *(const uint32_t *)sp; break;
                default: w = *(const uint64_t *)sp; break;
                }
                v = (unsigned long)w;  /* zero-extended, as from_stack does: the callee reads the low bits */
            }
            /* a guest function start becomes a trampoline wherever it rides, including past x7 — the
                  untyped door can only wrap x0-x7, and a comparator in the ninth argument went through raw. */
            if (sz == 8) v = __arm2x86_cbarg(v);
            val[i].u = v;
            ni++;
        } else if (k == 'f') {
            /* a float lives in the low 32 bits of vN on arm64 and of xmmN on x86 */
            float f;
            memcpy(&f, spilled ? sp : (const void *)(vs + 16 * nf), 4);
            val[i].f = f;
            nf++;
        } else {
            double d;
            memcpy(&d, spilled ? sp : (const void *)(vs + 16 * nf), 8);
            val[i].d = d;
            nf++;
        }
        av[i] = &val[i];
    }

    struct sig_plan *p = sig_plan(s);
    if (!p) {
        if (sig_trace()) fprintf(a2x_errf(), "[sigdoor] %s: no cif (cache full or ffi_prep_cif refused)\n", n);
        return 0;
    }

    /* libffi writes at least one ffi_arg for an integer return; 16 bytes aligned covers every case here */
    union { ffi_arg a; double d; float f; unsigned char b[16]; } rb;
    memset(&rb, 0, sizeof rb);
    ffi_call(&p->cif, FFI_FN((void *)(uintptr_t)tgt), &rb, av);

    switch (s->ret) {
    case SIG_VOID:
        /* x0 is left alone, deliberately. arm2x86.py:1377 writes a deterministic 0 after a void call and
              then has to special-case bzero, because a real arm64 bzero preserves x0 and a translated tail
              caller's allocation pointer came back through it. The host cannot know which native callees
              preserve x0, so it changes nothing rather than destroying a value that may be live. */
        break;
    case SIG_I32: {
        uint32_t w = (uint32_t)(uint64_t)rb.a;
        unsigned bits = s->narrow >> 1;
        if (bits) {
            /* arm2x86.NARROW_RET: x86-64 leaves the bits above a Boolean/char/short return
                  undefined; arm64 callers read w0 whole. Truncate to the declared width and re-extend by
                  the declared signedness — the table itself comes out of arm2x86.py, see mksigtable.py. */
            if (s->narrow & 1)
                w = bits == 8 ? (uint32_t)(int32_t)(int8_t)w : (uint32_t)(int32_t)(int16_t)w;
            else
                w = bits == 8 ? (w & 0xffu) : (w & 0xffffu);
        }
        xs[0] = w;                     /* zero-extended into x0, as the per-app bind's zext i32 -> i64 does */
        break;
    }
    case SIG_I64:
        xs[0] = (unsigned long)(uint64_t)rb.a;
        break;
    case SIG_F32:
        memset(vs, 0, 16);             /* writing s0 zeroes the rest of v0 on the hardware */
        memcpy(vs, &rb.f, 4);
        break;
    default:
        memset(vs, 0, 16);
        memcpy(vs, &rb.d, 8);
        break;
    }
    /* x1 is not touched either. On arm64 it only carries a result as the high half of a 16-byte struct
          pair, and every such return is an 'x' in sigdb and never reaches this door. The untyped door
          writes rdx there, which is neither right nor wrong; the per-app typed bind leaves it, so this does
          too. */
    if (sig_trace())
        fprintf(a2x_errf(), "[sigdoor] %s: typed (%u args \"%s\", ret %u%s) -> x0=0x%lx\n",
                n, s->argc, s->kinds, s->ret, (s->flags & SIGF_NEED) ? ", fixed shape gets this wrong" : "",
                xs[0]);
    return 1;
}
