// a64_util.c — the shared primitives: condition codes, AddWithCarry, DecodeBitMasks, shifts, extends, atomics.
// Each one implements the pseudocode function of the same name in the Arm ARM (DDI 0487), named in its comment.
#include "a64_priv.h"

/* shared/functions/system/ConditionHolds */
int a64_cond(const a64 *s, unsigned c) {
    int r;
    switch (c >> 1) {
    case 0: r = fZ(s); break;                       // EQ
    case 1: r = fC(s); break;                       // CS
    case 2: r = fN(s); break;                       // MI
    case 3: r = fV(s); break;                       // VS
    case 4: r = fC(s) && !fZ(s); break;             // HI
    case 5: r = fN(s) == fV(s); break;              // GE
    case 6: r = fN(s) == fV(s) && !fZ(s); break;    // GT
    default: r = 1; break;                          // AL
    }
    /* The invert is skipped for 0b1111, not for 0b111x: cond 14 (AL) and 15 (NV) BOTH always hold. */
    if ((c & 1) && c != 15) r = !r;
    return r;
}

/* shared/functions/integer/AddWithCarry — returns the result and, when flags, writes NZCV.
   C AND V come from the same addition at the operation's own width. Computing them at 64 bits for a
      32-bit op gives a carry that is always 0 and a V that never fires; risu caught exactly that shape. */
uint64_t a64_awc(a64 *s, uint64_t a, uint64_t b, unsigned carry, int sf, int flags) {
    if (sf) {
        unsigned __int128 u = (unsigned __int128)a + (unsigned __int128)b + carry;
        uint64_t r = (uint64_t)u;
        if (flags) setf(s, (int)(r >> 63), r == 0, (int)(u >> 64), (int)((~(a ^ b) & (a ^ r)) >> 63));
        return r;
    }
    uint32_t A = (uint32_t)a, Bv = (uint32_t)b;
    uint64_t u = (uint64_t)A + Bv + carry;
    uint32_t r = (uint32_t)u;
    if (flags) setf(s, (int)(r >> 31), r == 0, (int)(u >> 32), (int)((~(A ^ Bv) & (A ^ r)) >> 31));
    return r;
}

/* aarch64/instrs/integer/bitmasks/DecodeBitMasks — the wmask. Returns 0 for the reserved encodings. */
int a64_bitmask(unsigned N, unsigned imms, unsigned immr, int sf, uint64_t *wmask) {
    unsigned x = (N << 6) | (~imms & 0x3f);
    if (!x) return 0;                                   /* len would be -1: UNDEFINED */
    int len = 31 - __builtin_clz(x);
    if (len < 1) return 0;
    if (!sf && N) return 0;                             /* 64-bit element in a 32-bit instruction */
    unsigned esize = 1u << len, levels = esize - 1, S = imms & levels, R = immr & levels;
    if (S == levels) return 0;                          /* all-ones element: UNDEFINED */
    uint64_t emask = esize == 64 ? ~0ULL : ((1ULL << esize) - 1);
    uint64_t welem = (S + 1) == 64 ? ~0ULL : ((1ULL << (S + 1)) - 1);
    uint64_t e = R ? (((welem >> R) | (welem << (esize - R))) & emask) : welem;
    /* Replicate(e, 64/esize) as one multiply — ~0/emask is 1 in every esize-th bit (0x0101... for 8). The
       loop it replaces ran up to 32 times per logical-immediate instruction (25 of 264 profile samples were in it). */
    uint64_t w = esize == 64 ? e : e * (~0ULL / emask);
    *wmask = sf ? w : (uint64_t)(uint32_t)w;
    return 1;
}

/* shared/functions/common/ShiftReg — LSL/LSR/ASR/ROR at the operation's width */
uint64_t a64_shift(uint64_t v, unsigned type, unsigned amt, int sf) {
    unsigned d = sf ? 64 : 32;
    v = trw(v, sf);
    amt &= d - 1;
    if (!amt) return v;
    switch (type) {
    case 0: return trw(v << amt, sf);
    case 1: return sf ? (v >> amt) : (uint64_t)((uint32_t)v >> amt);
    case 2: return sf ? (uint64_t)((int64_t)v >> amt)
                      : (uint64_t)(uint32_t)((int32_t)(uint32_t)v >> amt);
    default: return sf ? ((v >> amt) | (v << (64 - amt)))
                       : (uint64_t)(uint32_t)(((uint32_t)v >> amt) | ((uint32_t)v << (32 - amt)));
    }
}

/* shared/functions/common/ExtendReg — option = UXTB..SXTX, then << shift */
uint64_t a64_extend(uint64_t v, unsigned option, unsigned sh) {
    switch (option) {
    case 0: v = (uint8_t)v; break;
    case 1: v = (uint16_t)v; break;
    case 2: v = (uint32_t)v; break;
    case 3: break;
    case 4: v = sx(v, 8); break;
    case 5: v = sx(v, 16); break;
    case 6: v = sx(v, 32); break;
    default: break;
    }
    return v << sh;
}

/* The LSE atomics are real atomics here, not load+modify+store. Translated code and native frameworks run
      in one process and share these words (every objc refcount, every os_unfair_lock), so an interpreted
      `ldaddal` that was not atomic would corrupt state no certifier running one thread could ever show. */
typedef int8_t s8_t; typedef int16_t s16_t; typedef int32_t s32_t; typedef int64_t s64_t;

#define RMW_BODY(T, ST)                                                                     \
    do {                                                                                    \
        T *p = (T *)(uintptr_t)a, x = (T)v, old;                                            \
        switch (op) {                                                                       \
        case 0: old = __atomic_fetch_add(p, x, __ATOMIC_SEQ_CST); break;                    \
        case 1: old = __atomic_fetch_and(p, (T)~x, __ATOMIC_SEQ_CST); break;  /* CLR */      \
        case 2: old = __atomic_fetch_xor(p, x, __ATOMIC_SEQ_CST); break;                    \
        case 3: old = __atomic_fetch_or(p, x, __ATOMIC_SEQ_CST); break;                     \
        default: {                                                                          \
            old = __atomic_load_n(p, __ATOMIC_SEQ_CST);                                     \
            T nw;                                                                           \
            do {                                                                            \
                switch (op) {                                                               \
                case 4: nw = (ST)old > (ST)x ? old : x; break;   /* SMAX */                 \
                case 5: nw = (ST)old < (ST)x ? old : x; break;   /* SMIN */                 \
                case 6: nw = old > x ? old : x; break;           /* UMAX */                 \
                default: nw = old < x ? old : x; break;          /* UMIN */                 \
                }                                                                           \
            } while (!__atomic_compare_exchange_n(p, &old, nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)); \
        }                                                                                   \
        }                                                                                   \
        return (uint64_t)(T)old;                                                            \
    } while (0)

uint64_t a64_rmw(uint64_t a, unsigned n, unsigned op, uint64_t v) {
    switch (n) {
    case 1: RMW_BODY(uint8_t, s8_t);
    case 2: RMW_BODY(uint16_t, s16_t);
    case 4: RMW_BODY(uint32_t, s32_t);
    default: RMW_BODY(uint64_t, s64_t);
    }
}
#undef RMW_BODY

uint64_t a64_swp(uint64_t a, unsigned n, uint64_t v) {
    switch (n) {
    case 1: return __atomic_exchange_n((uint8_t *)(uintptr_t)a, (uint8_t)v, __ATOMIC_SEQ_CST);
    case 2: return __atomic_exchange_n((uint16_t *)(uintptr_t)a, (uint16_t)v, __ATOMIC_SEQ_CST);
    case 4: return __atomic_exchange_n((uint32_t *)(uintptr_t)a, (uint32_t)v, __ATOMIC_SEQ_CST);
    default: return __atomic_exchange_n((uint64_t *)(uintptr_t)a, v, __ATOMIC_SEQ_CST);
    }
}

/* returns what memory HELD (CAS writes the old value back into Rs, success or not) */
uint64_t a64_cas(uint64_t a, unsigned n, uint64_t expect, uint64_t nw) {
    switch (n) {
    case 1: { uint8_t e = (uint8_t)expect;
        __atomic_compare_exchange_n((uint8_t *)(uintptr_t)a, &e, (uint8_t)nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    case 2: { uint16_t e = (uint16_t)expect;
        __atomic_compare_exchange_n((uint16_t *)(uintptr_t)a, &e, (uint16_t)nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    case 4: { uint32_t e = (uint32_t)expect;
        __atomic_compare_exchange_n((uint32_t *)(uintptr_t)a, &e, (uint32_t)nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    default: { uint64_t e = expect;
        __atomic_compare_exchange_n((uint64_t *)(uintptr_t)a, &e, nw, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return e; }
    }
}

int a64_casp(uint64_t a, unsigned n, uint64_t *e0, uint64_t *e1, uint64_t n0, uint64_t n1) {
    if (n == 4) {
        uint64_t e = ((*e0) & 0xffffffffULL) | ((*e1) << 32);
        uint64_t w = (n0 & 0xffffffffULL) | (n1 << 32);
        int ok = __atomic_compare_exchange_n((uint64_t *)(uintptr_t)a, &e, w, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *e0 = (uint32_t)e; *e1 = (uint32_t)(e >> 32);
        return ok;
    }
    unsigned __int128 e = ((unsigned __int128)*e1 << 64) | *e0;
    unsigned __int128 w = ((unsigned __int128)n1 << 64) | n0;
    int ok = __atomic_compare_exchange_n((unsigned __int128 *)(uintptr_t)a, &e, w, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    *e0 = (uint64_t)e; *e1 = (uint64_t)(e >> 64);
    return ok;
}

/* FP MIN/max in the lane's own type. Both families separate +0 from -0, and they differ only in NaN
      handling — FMAX/FMIN propagate a NaN, the NM forms return the other operand (IEEE 754 maxNum/minNum).
      Widening a float pair to double to share one implementation rewrites NaN payloads: fminnm, fmaxnm and
      fsub all differ from hardware on the NaN states unless each is computed at its own width. */
/* FMAXNM/FMINNM ignore a quiet NaN and propagate a signalling one. Treating every NaN as
      ignorable returns the other operand where hardware returns the quieted SNaN with its payload
      (fminnm s3 gives ffdcf942 from an ff9cf942 input, not the number). IEEE 754 maxNum is
      defined on quiet NaNs; an SNaN is an invalid operation first. */
static int snan_f(float v) {
    uint32_t u; memcpy(&u, &v, 4);
    return (u & 0x7f800000u) == 0x7f800000u && (u & 0x7fffffu) && !(u & 0x400000u);
}
static int snan_d(double v) {
    uint64_t u; memcpy(&u, &v, 8);
    return (u & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL
        && (u & 0x000fffffffffffffULL) && !(u & 0x0008000000000000ULL);
}

/* ARM's FPProcessNaNs, implemented here rather than left to the host — the one piece of floating
      point that must not be inherited. Two reasons:
      1. The optimizer rewrites it. `a1 ? x - y : x + y` becomes `x + (a1 ? -y : y)` at -O1, which is the same
         value and the wrong NaN: negating flips the sign of the operand being propagated, so
         fsub(+inf, qNaN) returned ffc00000 where hardware gives 7fc00000. At -O0 the same source matched
         over 2,072 forms — so the arithmetic was right and the compiler was free to do that. It is only free
         while the NaN cases reach the arithmetic at all.
      2. The host is x86. SSE picks which NaN to return by operand position in its own way, so a
         translator that inherits the host's rule cannot agree with arm64 hardware by construction.
      ARM's definition (shared/functions/float/FPProcessNaNs): a signalling NaN in op1, then op2, then a quiet NaN in
      op1, then op2 — quieted, payload and sign intact. FPCR.DN is 0 on this platform (fpcr reads 0),
      so the default NaN does not come into it; if DN is ever set, this is where that goes. */
static float qt_f(float v) { uint32_t u; memcpy(&u, &v, 4); u |= 0x00400000u; memcpy(&v, &u, 4); return v; }
static double qt_d(double v) { uint64_t u; memcpy(&u, &v, 8); u |= 0x0008000000000000ULL; memcpy(&v, &u, 8); return v; }

int a64_fpnan2f(float a, float b, float *out) {
    if (snan_f(a)) { *out = qt_f(a); return 1; }
    if (snan_f(b)) { *out = qt_f(b); return 1; }
    if (__builtin_isnan(a)) { *out = a; return 1; }
    if (__builtin_isnan(b)) { *out = b; return 1; }
    return 0;
}
int a64_fpnan2d(double a, double b, double *out) {
    if (snan_d(a)) { *out = qt_d(a); return 1; }
    if (snan_d(b)) { *out = qt_d(b); return 1; }
    if (__builtin_isnan(a)) { *out = a; return 1; }
    if (__builtin_isnan(b)) { *out = b; return 1; }
    return 0;
}

#define MINMAX(T, SUF)                                                                              \
T a64_vmax##SUF(T a, T b) {                                                                         \
    T n_;                                                                                           \
    if (a64_fpnan2##SUF(a, b, &n_)) return n_;                                                      \
    if (a > b) return a;                                                                            \
    if (a < b) return b;                                                                            \
    return __builtin_signbit(a) ? b : a;                                                            \
}                                                                                                   \
T a64_vmin##SUF(T a, T b) {                                                                         \
    T n_;                                                                                           \
    if (a64_fpnan2##SUF(a, b, &n_)) return n_;                                                      \
    if (a < b) return a;                                                                            \
    if (a > b) return b;                                                                            \
    return __builtin_signbit(a) ? a : b;                                                            \
}                                                                                                   \
T a64_vmaxnm##SUF(T a, T b) {                                                                       \
    if (snan_##SUF(a) || snan_##SUF(b)) return a64_vmax##SUF(a, b);   /* invalid: propagate, quieted */ \
    if (__builtin_isnan(a)) return b;                                                               \
    if (__builtin_isnan(b)) return a;                                                               \
    return a64_vmax##SUF(a, b);                                                                     \
}                                                                                                   \
T a64_vminnm##SUF(T a, T b) {                                                                       \
    if (snan_##SUF(a) || snan_##SUF(b)) return a64_vmin##SUF(a, b);                                 \
    if (__builtin_isnan(a)) return b;                                                               \
    if (__builtin_isnan(b)) return a;                                                               \
    return a64_vmin##SUF(a, b);                                                                     \
}
MINMAX(float, f)
MINMAX(double, d)

/* the code-change generation the decode caches compare (a64_priv.h, a64_code_changed) */
unsigned a64_code_gen;
