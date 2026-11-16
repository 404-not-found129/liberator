// a64_simdfp.c — Advanced SIMD floating point: three-same (FP), two-register misc (FP) and the
// narrowing/lengthening converts. Arm ARM C4.1.89. Single and double elements; the FP16 vector forms are a
// separate encoding table and are reported, not guessed.
//
// Every lane is computed in its own type, never widened to double first. Widening looks harmless — float
//    add, subtract and multiply are all exact in double, so the arithmetic survives — but it rewrites NaN
//    payloads on the way in and out, and ARM's rule is that a single NaN operand is returned quieted with its
//    payload intact. fsub.2s/.4s/.2d, fmaxnm, fminnm, frecps and frsqrts all differ from hardware
//    on the NaN states for exactly this reason when widened.
#include "a64_lane.h"
#include <math.h>

/* FPMax/FPMin and their NM forms, per type. The families differ only in NaN handling, which is why both
   exist: FMAX propagates a NaN, FMAXNM returns the other operand (IEEE 754 maxNum). Both separate +0/-0. */

/* FPToFixed at the lane's own width, saturating; NaN gives 0 */
static uint64_t fsat_s(double r, unsigned eb) {
    if (isnan(r)) return 0;
    double hi = eb == 8 ? 9223372036854775808.0 : 2147483648.0;
    if (r >= hi) return eb == 8 ? (uint64_t)INT64_MAX : (uint64_t)(uint32_t)INT32_MAX;
    if (r < -hi) return eb == 8 ? (uint64_t)INT64_MIN : (uint64_t)(uint32_t)INT32_MIN;
    return eb == 8 ? (uint64_t)(int64_t)r : (uint64_t)(uint32_t)(int32_t)r;
}
static uint64_t fsat_u(double r, unsigned eb) {
    if (isnan(r) || r <= 0.0) return 0;
    double hi = eb == 8 ? 18446744073709551616.0 : 4294967296.0;
    if (r >= hi) return eb == 8 ? UINT64_MAX : (uint64_t)UINT32_MAX;
    return eb == 8 ? (uint64_t)r : (uint64_t)(uint32_t)r;
}

/* one three-same FP lane loop, instantiated per element type */
#define FP3(T, FMA_, ABS_, SUF, NAN2_)                                                                     \
    for (unsigned l = 0; l < lanes; l++) {                                                          \
        T x, y, dv, r = 0;                                                                          \
        memcpy(&x, s->v[rn] + sizeof(T) * l, sizeof(T));                                            \
        memcpy(&y, s->v[rm] + sizeof(T) * l, sizeof(T));                                            \
        memcpy(&dv, s->v[rd] + sizeof(T) * l, sizeof(T));                                           \
        uint64_t m = mask_eb(eb);                                                                   \
        int cmp = -1;                                                                               \
        /* ARM's NaN rule is applied before the arithmetic, so no host and no optimizer gets a say. */ \
        T nan_; int hasnan_ = NAN2_(x, y, &nan_);                                                   \
        switch ((U << 5) | opcode) {                                                                \
        case 0x18: r = a1 ? a64_vminnm##SUF(x, y) : a64_vmaxnm##SUF(x, y); break;   /* FMINNM / FMAXNM */    \
        case 0x19: r = FMA_(a1 ? -x : x, y, dv); break;                     /* FMLS / FMLA, fused */ \
        case 0x1a: {                                                        /* FSUB / FADD */        \
            /* Both are computed and one is selected. Written as `a1 ? x - y : x + y` the compiler sinks  \
                  the common add and emits `x + (a1 ? -y : y)` — which is the same value and the wrong     \
                  NaN: negating flips the sign of a propagated NaN, so fsub(+inf, qNaN) returns ffc00000   \
                  where hardware gives 7fc00000. ARM's FPSub propagates the original operand. */            \
            if (hasnan_) { r = nan_; break; }                                                       \
            r = a1 ? x - y : x + y;                                                                 \
            break;                                                                                  \
        }                                                                                           \
        case 0x1b:                                                          /* FMULX (a=0) */        \
            if (a1) return A64_UNDEF;                                                               \
            /* FMULX IS NOT FMUL: 0 x inf is +-2.0, which is the whole reason it exists (it keeps \
                  the FRECPS/FRSQRTS Newton steps finite instead of producing a NaN). */             \
            if (hasnan_) { r = nan_; break; }                                                       \
            if ((x == 0 && isinf(y)) || (y == 0 && isinf(x)))                                       \
                r = (signbit(x) ^ signbit(y)) ? (T)-2 : (T)2;                                       \
            else r = x * y;                                                                         \
            break;                                                                                  \
        case 0x1c:                                                          /* FCMEQ (a=0) */        \
            if (a1) return A64_UNDEF;                                                               \
            cmp = (x == y); break;                                                                  \
        case 0x1e:                                                          /* FMIN / FMAX */        \
            if (hasnan_) { r = nan_; break; }                                                       \
            r = a1 ? a64_vmin##SUF(x, y) : a64_vmax##SUF(x, y); break;                              \
        case 0x1f:                                                          /* FRSQRTS / FRECPS */   \
            /* One fma, not a multiply then an fma. The architecture computes (-op1)*(op2/2) + 1.5 \
                  and 2.0 - op1*op2 as single fused steps; rounding the product first differs from    \
                  hardware. The 0 x inf case is defined to give the step's own constant. */            \
            /* The negate comes first. The architecture computes these as FPMulAdd(.., FPNeg(op1), ..),  \
                  so a NaN propagated out of op1 carries the flipped sign: hardware answers 7fffffff for an  \
                  ffffffff input, and taking the NaN from the un-negated operand gives ffffffff. */          \
            { T nn_; if (NAN2_(-x, y, &nn_)) { r = nn_; break; } }                                  \
            if ((x == 0 && isinf(y)) || (y == 0 && isinf(x))) r = a1 ? (T)1.5 : (T)2;                \
            else if (a1) r = FMA_(-x, y * (T)0.5, (T)1.5);                                           \
            else r = FMA_(-x, y, (T)2);                                                              \
            break;                                                                                  \
        case 0x3a:                                                          /* FABD (a=1) */         \
            if (!a1) return A64_UNDEF;                                      /* a=0 is FADDP */       \
            /* FABD is FPAbs(FPSub(..)): the absolute value applies to a propagated NaN too, which is  \
                  why hardware answers 7fffffff where the raw NaN is ffffffff. */                             \
            if (hasnan_) { r = ABS_(nan_); break; }                                                 \
            r = ABS_(x - y); break;                                                                 \
        case 0x3b:                                                          /* FMUL (a=0) */         \
            if (a1) return A64_UNDEF;                                                               \
            if (hasnan_) { r = nan_; break; }                                                       \
            r = x * y; break;                                                                       \
        case 0x3c: cmp = a1 ? (x > y) : (x >= y); break;                    /* FCMGT / FCMGE */      \
        case 0x3d: cmp = a1 ? (ABS_(x) > ABS_(y)) : (ABS_(x) >= ABS_(y)); break;  /* FACGT / FACGE */ \
        case 0x3f:                                                          /* FDIV (a=0) */         \
            if (a1) return A64_UNDEF;                                                               \
            if (hasnan_) { r = nan_; break; }                                                       \
            r = x / y; break;                                                                       \
        default: return A64_UNDEF;                                                                  \
        }                                                                                           \
        if (cmp >= 0) lwr(t.b, l, eb, cmp ? m : 0);                                                 \
        else memcpy(t.b + sizeof(T) * l, &r, sizeof(T));                                            \
    }

int a64_simdfp(a64 *s, uint32_t i) {
    int scalar = BIT(i, 28);
    if (BIT(i, 31)) return A64_UNDEF;   /* the crypto groups share these fields; see a64_simd.c */
    unsigned rd = B(i, 4, 0), rn = B(i, 9, 5), rm = B(i, 20, 16);
    unsigned U = BIT(i, 29);
    int a1 = (int)BIT(i, 23);                   /* size<1>: which of the two operations this opcode names */
    int dbl = (int)BIT(i, 22);                  /* size<0> = sz: 0 single, 1 double */
    int Q = scalar ? 1 : (int)BIT(i, 30);
    unsigned eb = dbl ? 8u : 4u;

    /* ───────── three same (FP) ───────── */
    if (BIT(i, 21) && BIT(i, 10) && (scalar ? B(i, 28, 24) == 0x1e : B(i, 28, 24) == 0x0e)
        && B(i, 15, 11) >= 0x18) {
        unsigned opcode = B(i, 15, 11);
        unsigned lanes = scalar ? 1 : (Q ? 16u : 8u) / eb;
        if (!scalar && dbl && !Q) return A64_UNDEF;         /* 1 x 64-bit lane is not a vector form */
        vreg t = (vreg){{0}};

        /* the pairwise forms read the concatenation of Vn and Vm, like their integer twins */
        if (U && !scalar && (opcode == 0x1a || opcode == 0x1e) && !(a1 && opcode == 0x1a)) {
            unsigned nb = Q ? 16u : 8u;
            uint8_t cat[32];
            memcpy(cat, s->v[rn], nb);
            memcpy(cat + nb, s->v[rm], nb);
            for (unsigned l = 0; l < lanes; l++) {
                if (dbl) {
                    double p, q, r;
                    memcpy(&p, cat + 8 * (2 * l), 8);
                    memcpy(&q, cat + 8 * (2 * l + 1), 8);
                    r = opcode == 0x1a ? p + q : (a1 ? a64_vmind(p, q) : a64_vmaxd(p, q));
                    memcpy(t.b + 8 * l, &r, 8);
                } else {
                    float p, q, r;
                    memcpy(&p, cat + 4 * (2 * l), 4);
                    memcpy(&q, cat + 4 * (2 * l + 1), 4);
                    r = opcode == 0x1a ? p + q : (a1 ? a64_vminf(p, q) : a64_vmaxf(p, q));
                    memcpy(t.b + 4 * l, &r, 4);
                }
            }
            vput(s, rd, &t, Q);
            return A64_NEXT;
        }

        if (dbl) { FP3(double, fma, fabs, d, a64_fpnan2d) }
        else     { FP3(float, fmaf, fabsf, f, a64_fpnan2f) }

        if (scalar) { memset(s->v[rd], 0, 16); memcpy(s->v[rd], t.b, eb); }
        else vput(s, rd, &t, Q);
        return A64_NEXT;
    }

    /* ───────── two-register misc: the FP forms and the narrowing/lengthening converts ───────── */
    if (B(i, 21, 17) == 0x10 && B(i, 11, 10) == 2 &&
        (scalar ? B(i, 28, 24) == 0x1e : B(i, 28, 24) == 0x0e)) {
        unsigned opcode = B(i, 16, 12), size = B(i, 23, 22);

        if (opcode == 0x13 && U) {                      /* SHLL / SHLL2 */
            if (size == 3 || scalar) return A64_UNDEF;
            unsigned ieb = 1u << size, lanes = 8u / ieb;
            const uint8_t *src = s->v[rn] + (Q ? 8 : 0);
            vreg t = (vreg){{0}};
            for (unsigned l = 0; l < lanes; l++)
                lwr(t.b, l, 2 * ieb, (lrd(src, l, ieb) << (ieb * 8)) & mask_eb(2 * ieb));
            memcpy(s->v[rd], t.b, 16);
            return A64_NEXT;
        }
        /* XTN / SQXTN / UQXTN / SQXTUN — the result is half as wide and fills the low (or, with Q, high) half */
        if (opcode == 0x12 || opcode == 0x14) {
            if (size == 3) return A64_UNDEF;
            unsigned oeb = 1u << size, ieb = oeb * 2;
            unsigned lanes = 8u / oeb;
            vreg t = (vreg){{0}};
            if (Q) memcpy(t.b, s->v[rd], 8);                /* the ...2 forms keep the low half of Vd */
            for (unsigned l = 0; l < lanes; l++) {
                uint64_t x = lrd(s->v[rn], l, ieb), r;
                if (opcode == 0x12 && !U) r = x & mask_eb(oeb);                      /* XTN: truncate */
                else if (opcode == 0x12) r = sat_uns((uint64_t)sgn(x, ieb),          /* SQXTUN */
                                                     sgn(x, ieb) < 0, oeb);
                else if (!U) r = sat_sgn(sgn(x, ieb), oeb);                          /* SQXTN */
                else r = sat_uns(x, 0, oeb);                                         /* UQXTN */
                lwr(t.b + (Q ? 8 : 0), l, oeb, r);
            }
            memcpy(s->v[rd], t.b, 16);
            if (!Q) memset(s->v[rd] + 8, 0, 8);
            return A64_NEXT;
        }
        /* FCVTN / FCVTL — narrow double->single (or single->half) and the reverse */
        if ((opcode == 0x16 || opcode == 0x17) && !U) {
            if (size > 1) return A64_UNDEF;
            vreg t = (vreg){{0}};
            if (opcode == 0x16) {                           /* FCVTN{2} */
                if (Q) memcpy(t.b, s->v[rd], 8);
                if (size) for (unsigned l = 0; l < 2; l++) {
                    double dv; memcpy(&dv, s->v[rn] + 8 * l, 8);
                    float f = (float)dv; memcpy(t.b + (Q ? 8 : 0) + 4 * l, &f, 4);
                } else for (unsigned l = 0; l < 4; l++) {
                    float f; memcpy(&f, s->v[rn] + 4 * l, 4);
                    _Float16 h = (_Float16)f; memcpy(t.b + (Q ? 8 : 0) + 2 * l, &h, 2);
                }
                memcpy(s->v[rd], t.b, 16);
                if (!Q) memset(s->v[rd] + 8, 0, 8);
                return A64_NEXT;
            }
            const uint8_t *src = s->v[rn] + (Q ? 8 : 0);    /* FCVTL2 lengthens the high half */
            if (size) for (unsigned l = 0; l < 2; l++) {
                float f; memcpy(&f, src + 4 * l, 4);
                double dv = (double)f; memcpy(t.b + 8 * l, &dv, 8);
            } else for (unsigned l = 0; l < 4; l++) {
                _Float16 h; memcpy(&h, src + 2 * l, 2);
                float f = (float)h; memcpy(t.b + 4 * l, &f, 4);
            }
            memcpy(s->v[rd], t.b, 16);
            return A64_NEXT;
        }

        if (opcode < 0x0c || (opcode >= 0x10 && opcode < 0x18)) return A64_UNDEF;

        unsigned lanes = scalar ? 1 : (Q ? 16u : 8u) / eb;
        if (!scalar && dbl && !Q) return A64_UNDEF;
        vreg t = (vreg){{0}};

        for (unsigned l = 0; l < lanes; l++) {
            uint64_t m = mask_eb(eb), iv = 0;
            int cmp = -1, isint = 0;
            double x = 0;
            float fx = 0;
            if (dbl) memcpy(&x, s->v[rn] + 8 * l, 8);
            else { memcpy(&fx, s->v[rn] + 4 * l, 4); x = (double)fx; }
            union { float f; double d; } r = {0};
            int rm_ = -1;
            switch ((U << 6) | (a1 << 5) | opcode) {
            case (0 << 6) | (1 << 5) | 0x0c: cmp = (x > 0.0); break;        /* FCMGT #0 */
            case (0 << 6) | (1 << 5) | 0x0d: cmp = (x == 0.0); break;       /* FCMEQ #0 */
            case (0 << 6) | (1 << 5) | 0x0e: cmp = (x < 0.0); break;        /* FCMLT #0 */
            case (1 << 6) | (1 << 5) | 0x0c: cmp = (x >= 0.0); break;       /* FCMGE #0 */
            case (1 << 6) | (1 << 5) | 0x0d: cmp = (x <= 0.0); break;       /* FCMLE #0 */
            /* FABS and FNEG are bit operations, so they are done on the bits: through a double they would
                  canonicalise a signalling NaN, and the architecture says they only touch the sign. */
            case (0 << 6) | (1 << 5) | 0x0f: {
                uint64_t b = lrd(s->v[rn], l, eb);
                lwr(t.b, l, eb, b & (m >> 1));
                continue;
            }
            case (1 << 6) | (1 << 5) | 0x0f: {
                uint64_t b = lrd(s->v[rn], l, eb);
                lwr(t.b, l, eb, b ^ (m ^ (m >> 1)));
                continue;
            }
            case (1 << 6) | (1 << 5) | 0x1f:                                /* FSQRT */
                if (dbl) r.d = sqrt(x); else r.f = sqrtf(fx);
                break;
            /* rounds to an integral value, keeping the FP type. Each is applied IN the lane's own type
               below: through a double, FRINTN of a signalling NaN came back with a rewritten payload. */
            case (0 << 6) | (0 << 5) | 0x18: rm_ = 0; break;                /* FRINTN */
            case (0 << 6) | (0 << 5) | 0x19: rm_ = 1; break;                /* FRINTM */
            case (0 << 6) | (1 << 5) | 0x18: rm_ = 2; break;                /* FRINTP */
            case (0 << 6) | (1 << 5) | 0x19: rm_ = 3; break;                /* FRINTZ */
            case (1 << 6) | (0 << 5) | 0x18: rm_ = 4; break;                /* FRINTA */
            case (1 << 6) | (0 << 5) | 0x19: rm_ = 5; break;                /* FRINTX, the current mode */
            case (1 << 6) | (1 << 5) | 0x19: rm_ = 6; break;                /* FRINTI, the current mode */
            /* float -> integer, with the rounding the mnemonic fixes */
            case (0 << 6) | (0 << 5) | 0x1a: iv = fsat_s(__builtin_roundeven(x), eb); isint = 1; break;
            case (1 << 6) | (0 << 5) | 0x1a: iv = fsat_u(__builtin_roundeven(x), eb); isint = 1; break;
            case (0 << 6) | (0 << 5) | 0x1b: iv = fsat_s(__builtin_floor(x), eb); isint = 1; break;
            case (1 << 6) | (0 << 5) | 0x1b: iv = fsat_u(__builtin_floor(x), eb); isint = 1; break;
            case (0 << 6) | (1 << 5) | 0x1a: iv = fsat_s(__builtin_ceil(x), eb); isint = 1; break;
            case (1 << 6) | (1 << 5) | 0x1a: iv = fsat_u(__builtin_ceil(x), eb); isint = 1; break;
            case (0 << 6) | (1 << 5) | 0x1b: iv = fsat_s(__builtin_trunc(x), eb); isint = 1; break;
            case (1 << 6) | (1 << 5) | 0x1b: iv = fsat_u(__builtin_trunc(x), eb); isint = 1; break;
            case (0 << 6) | (0 << 5) | 0x1c: iv = fsat_s(__builtin_round(x), eb); isint = 1; break;
            case (1 << 6) | (0 << 5) | 0x1c: iv = fsat_u(__builtin_round(x), eb); isint = 1; break;
            /* integer -> float */
            case (0 << 6) | (0 << 5) | 0x1d:                                /* SCVTF */
                if (dbl) r.d = (double)(int64_t)lrd(s->v[rn], l, 8);
                else r.f = (float)(int32_t)(uint32_t)lrd(s->v[rn], l, 4);
                break;
            case (1 << 6) | (0 << 5) | 0x1d:                                /* UCVTF */
                if (dbl) r.d = (double)lrd(s->v[rn], l, 8);
                else r.f = (float)(uint32_t)lrd(s->v[rn], l, 4);
                break;
            /* FRECPE / FRSQRTE: the hardware estimate tables, ported from the lifter (a64_est.c) */
            case (0 << 6) | (1 << 5) | 0x1d:
                lwr(t.b, l, eb, a64_frecpe(lrd(s->v[rn], l, eb), (int)eb * 8));
                continue;
            case (1 << 6) | (1 << 5) | 0x1d:
                lwr(t.b, l, eb, a64_frsqrte(lrd(s->v[rn], l, eb), (int)eb * 8));
                continue;
            /* Any other estimate or crypto form is reported, not guessed. The exact tables were read from hardware;
                  AES and SHA families are each a well-defined block of work; until they are written, an
                  unimplemented report names the instruction and nothing executes a plausible wrong answer. */
            default: return A64_UNDEF;
            }
            if (rm_ >= 0) {
                if (dbl) switch (rm_) {
                    case 0: r.d = __builtin_roundeven(x); break;
                    case 1: r.d = __builtin_floor(x); break;
                    case 2: r.d = __builtin_ceil(x); break;
                    case 3: r.d = __builtin_trunc(x); break;
                    case 4: r.d = __builtin_round(x); break;
                    case 5: r.d = rint(x); break;
                    default: r.d = nearbyint(x); break;
                } else switch (rm_) {
                    case 0: r.f = __builtin_roundevenf(fx); break;
                    case 1: r.f = __builtin_floorf(fx); break;
                    case 2: r.f = __builtin_ceilf(fx); break;
                    case 3: r.f = __builtin_truncf(fx); break;
                    case 4: r.f = __builtin_roundf(fx); break;
                    case 5: r.f = rintf(fx); break;
                    default: r.f = nearbyintf(fx); break;
                }
            }
            if (cmp >= 0) lwr(t.b, l, eb, cmp ? m : 0);
            else if (isint) lwr(t.b, l, eb, iv);
            else if (dbl) memcpy(t.b + 8 * l, &r.d, 8);
            else memcpy(t.b + 4 * l, &r.f, 4);
        }
        if (scalar) { memset(s->v[rd], 0, 16); memcpy(s->v[rd], t.b, eb); }
        else vput(s, rd, &t, Q);
        return A64_NEXT;
    }

    return A64_UNDEF;
}
