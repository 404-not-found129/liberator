// a64_fpsc.c — scalar floating point: Arm ARM C4.1.89, the subgroups with bits 28:24 = 11110 / 11111.
//
// Rounding mode and flush-to-zero are the host's, deliberately. FPCR.RMode and FPCR.FZ are already mirrored
//    into MXCSR by the runtime (the shim's per-thread FPCR shadow) and on an arm64 host FPCR *is* the register,
//    so ordinary C arithmetic here rounds exactly as the compiled translation of the same instruction does.
//    Setting the mode per operation would be slower and would disagree with the translator on the very cases
//    that matter.
// The instructions whose rounding the architecture fixes regardless of FPCR — FCVTNS/NU, FCVTAS/AU,
//    FCVTPS/PU, FCVTMS/MU, FCVTZS/ZU, FRINTN/A/P/M/Z — use the mode-independent builtins, never rint().
#include "a64_priv.h"
#include <math.h>

typedef _Float16 f16;

/* a scalar FP register as a value of the instruction's type, and back (a scalar write zeroes the rest) */
static inline f16    rh(const a64 *s, int r) { f16 v; memcpy(&v, s->v[r], 2); return v; }
static inline float  rf(const a64 *s, int r) { float v; memcpy(&v, s->v[r], 4); return v; }
static inline double rdd(const a64 *s, int r) { double v; memcpy(&v, s->v[r], 8); return v; }
static inline void   wh(a64 *s, int r, f16 v) { vzw(s, r, &v, 2); }
static inline void   wf(a64 *s, int r, float v) { vzw(s, r, &v, 4); }
static inline void   wd(a64 *s, int r, double v) { vzw(s, r, &v, 8); }

/* FPCompare (shared/functions/float/fpcompare): unordered gives N=0 Z=0 C=1 V=1 */
static void fpcmp(a64 *s, double a, double b) {
    if (isnan(a) || isnan(b)) setf(s, 0, 0, 1, 1);
    else if (a == b) setf(s, 0, 1, 1, 0);
    else if (a < b)  setf(s, 1, 0, 0, 0);
    else             setf(s, 0, 0, 1, 0);
}

/* FPToFixed: NaN -> 0, out of range -> saturate. The caller has already applied the rounding. */
static uint64_t sat_s(double r, int bits) {
    if (isnan(r)) return 0;
    if (bits == 64) {
        if (r >= 9223372036854775808.0) return (uint64_t)INT64_MAX;
        if (r < -9223372036854775808.0) return (uint64_t)INT64_MIN;
        return (uint64_t)(int64_t)r;
    }
    if (r >= 2147483648.0) return (uint64_t)(uint32_t)INT32_MAX;
    if (r < -2147483648.0) return (uint64_t)(uint32_t)INT32_MIN;
    return (uint64_t)(uint32_t)(int32_t)r;
}
static uint64_t sat_u(double r, int bits) {
    if (isnan(r) || r <= 0.0) return 0;                 /* negatives and -0.0 clamp to 0 */
    if (bits == 64) {
        if (r >= 18446744073709551616.0) return UINT64_MAX;
        return (uint64_t)r;
    }
    if (r >= 4294967296.0) return (uint64_t)UINT32_MAX;
    return (uint64_t)(uint32_t)r;
}

/* the architecture's fixed rounding modes, as mode-independent operations */
static double rnd(double v, unsigned mode) {
    switch (mode) {
    case 0: return __builtin_roundeven(v);   /* N: nearest, ties to even */
    case 1: return __builtin_ceil(v);        /* P: toward +inf */
    case 2: return __builtin_floor(v);       /* M: toward -inf */
    case 3: return __builtin_trunc(v);       /* Z: toward zero */
    default: return __builtin_round(v);      /* A: nearest, ties away from zero */
    }
}

int a64_fpsc(a64 *s, uint32_t i) {
    unsigned rd = B(i, 4, 0), rn = B(i, 9, 5), rm = B(i, 20, 16), type = B(i, 23, 22);
    int sf = BIT(i, 31);

    if (BIT(i, 29)) return A64_UNDEF;                   /* S must be 0 in every form below */

    /* ───── Floating-point data-processing (3 source): FMADD, FMSUB, FNMADD, FNMSUB ───── */
    if (B(i, 28, 24) == 0x1f) {
        unsigned ra = B(i, 14, 10), o1 = BIT(i, 21), o0 = BIT(i, 15);
        if (sf) return A64_UNDEF;
        /* FMADD = Ra + Rn*Rm · FMSUB = Ra - Rn*Rm · FNMADD = -Ra - Rn*Rm · FNMSUB = -Ra + Rn*Rm,
           i.e. the product is negated when o0 != o1 and the addend when o1 is set. */
        int negp = (int)(o0 ^ o1), nega = (int)o1;
        /* FUSED, so one rounding for the whole a*b+c: fma(), never a*b then +c. The difference is a
              last-bit disagreement with arm64 hardware on a large fraction of inputs. */
        switch (type) {
        case 0: { float a = rf(s, rn), b = rf(s, rm), c = rf(s, ra);
            wf(s, rd, fmaf(negp ? -a : a, b, nega ? -c : c)); return A64_NEXT; }
        case 1: { double a = rdd(s, rn), b = rdd(s, rm), c = rdd(s, ra);
            wd(s, rd, fma(negp ? -a : a, b, nega ? -c : c)); return A64_NEXT; }
        case 3: { /* half in double is exact (11+11 significand bits, and half's exponent span fits), so
                        this is one correctly-rounded result, not a double rounding. */
            double a = (double)rh(s, rn), b = (double)rh(s, rm), c = (double)rh(s, ra);
            wh(s, rd, (f16)((negp ? -a : a) * b + (nega ? -c : c))); return A64_NEXT; }
        default: return A64_UNDEF;
        }
    }

    if (B(i, 28, 24) != 0x1e) return A64_UNDEF;

    /* ───── Conversion between floating-point and fixed-point ───── */
    if (!BIT(i, 21)) {
        unsigned rmode = B(i, 20, 19), opcode = B(i, 18, 16), scale = B(i, 15, 10);
        unsigned fbits = 64 - scale;
        if (!sf && scale < 32) return A64_UNDEF;
        double mul = ldexp(1.0, (int)fbits);
        if (rmode == 0 && (opcode == 2 || opcode == 3)) {           /* SCVTF / UCVTF (fixed) */
            double v = (opcode == 2
                        ? (double)(sf ? (int64_t)rz(s, rn) : (int64_t)(int32_t)(uint32_t)rz(s, rn))
                        : (double)(sf ? rz(s, rn) : (uint64_t)(uint32_t)rz(s, rn))) / mul;
            if (type == 0) wf(s, rd, (float)v);
            else if (type == 1) wd(s, rd, v);
            else if (type == 3) wh(s, rd, (f16)v);
            else return A64_UNDEF;
            return A64_NEXT;
        }
        if (rmode == 3 && opcode <= 1) {                            /* FCVTZS / FCVTZU (fixed) */
            if (type == 2) return A64_UNDEF;
            double v = type == 0 ? (double)rf(s, rn) : type == 1 ? rdd(s, rn) : (double)rh(s, rn);
            v = __builtin_trunc(v * mul);
            wz(s, rd, opcode ? sat_u(v, sf ? 64 : 32) : sat_s(v, sf ? 64 : 32), sf);
            return A64_NEXT;
        }
        return A64_UNDEF;
    }

    /* ───── Floating-point data-processing (1 source) ───── */
    if (BIT(i, 14) && !B(i, 13, 10)) {
        unsigned opcode = B(i, 20, 15);
        if (sf) return A64_UNDEF;
        if (opcode >= 4 && opcode <= 7) {                           /* FCVT between the three sizes */
            unsigned dt = opcode & 3;
            if (dt == type || dt == 2 || type == 2) return A64_UNDEF;
            double v = type == 0 ? (double)rf(s, rn) : type == 1 ? rdd(s, rn) : (double)rh(s, rn);
            if (dt == 0) wf(s, rd, (float)v);
            else if (dt == 1) wd(s, rd, v);
            else wh(s, rd, (f16)v);
            return A64_NEXT;
        }
#define ONE_SRC(T, RD, WR, ABS_, SQRT_)                                               \
        do { T v = RD(s, rn), r;                                                      \
            switch (opcode) {                                                         \
            case 0x00: r = v; break;                                   /* FMOV  */    \
            case 0x01: r = ABS_(v); break;                             /* FABS  */    \
            case 0x02: r = -v; break;                                  /* FNEG  */    \
            case 0x03: r = SQRT_(v); break;                            /* FSQRT */    \
            case 0x08: r = (T)__builtin_roundeven((double)v); break;   /* FRINTN */   \
            case 0x09: r = (T)__builtin_ceil((double)v); break;        /* FRINTP */   \
            case 0x0a: r = (T)__builtin_floor((double)v); break;       /* FRINTM */   \
            case 0x0b: r = (T)__builtin_trunc((double)v); break;       /* FRINTZ */   \
            case 0x0c: r = (T)__builtin_round((double)v); break;       /* FRINTA */   \
            case 0x0e: r = (T)rint((double)v); break;                  /* FRINTX */   \
            case 0x0f: r = (T)nearbyint((double)v); break;             /* FRINTI */   \
            default: return A64_UNDEF;                                                \
            }                                                                         \
            WR(s, rd, r); return A64_NEXT; } while (0)
        switch (type) {
        case 0: ONE_SRC(float, rf, wf, fabsf, sqrtf);
        case 1: ONE_SRC(double, rdd, wd, fabs, sqrt);
        case 3: ONE_SRC(f16, rh, wh, __builtin_fabsf16, (f16)sqrt);
        default: return A64_UNDEF;
        }
#undef ONE_SRC
    }

    /* ───── Floating-point compare: FCMP, FCMPE, and the #0.0 forms ───── */
    if (BIT(i, 13) && !B(i, 12, 10)) {
        unsigned opcode2 = B(i, 4, 0);
        if (sf || (opcode2 & 7) || type == 2) return A64_UNDEF;
        int zero = (int)BIT(i, 3);
        double a = type == 0 ? (double)rf(s, rn) : type == 1 ? rdd(s, rn) : (double)rh(s, rn);
        double b = zero ? 0.0 : (type == 0 ? (double)rf(s, rm) : type == 1 ? rdd(s, rm) : (double)rh(s, rm));
        fpcmp(s, a, b);
        return A64_NEXT;
    }

    /* ───── Floating-point immediate: FMOV <Vd>, #imm ───── */
    if (BIT(i, 12) && !B(i, 11, 10)) {
        unsigned imm8 = B(i, 20, 13);
        if (sf || B(i, 9, 5)) return A64_UNDEF;
        /* VFPExpandImm: sign, a 3-bit exponent written as NOT(imm<6>):imm<5:4>, and a 4-bit mantissa */
        unsigned sign = BIT(imm8, 7), exp3 = B(imm8, 6, 4), man4 = B(imm8, 3, 0);
        double v = ldexp((double)(man4 + 16) / 16.0, (int)(exp3 ^ 4) - 3);
        if (sign) v = -v;
        if (type == 0) wf(s, rd, (float)v);
        else if (type == 1) wd(s, rd, v);
        else if (type == 3) wh(s, rd, (f16)v);
        else return A64_UNDEF;
        return A64_NEXT;
    }

    switch (B(i, 11, 10)) {
    case 1:                                             /* Floating-point conditional compare */
        if (sf || type == 2) return A64_UNDEF;
        if (a64_cond(s, B(i, 15, 12))) {
            double a = type == 0 ? (double)rf(s, rn) : type == 1 ? rdd(s, rn) : (double)rh(s, rn);
            double b = type == 0 ? (double)rf(s, rm) : type == 1 ? rdd(s, rm) : (double)rh(s, rm);
            fpcmp(s, a, b);
        } else {
            unsigned nz = B(i, 3, 0);
            setf(s, (int)BIT(nz, 3), (int)BIT(nz, 2), (int)BIT(nz, 1), (int)BIT(nz, 0));
        }
        return A64_NEXT;

    case 2: {                                           /* Floating-point data-processing (2 source) */
        unsigned opcode = B(i, 15, 12);
        if (sf) return A64_UNDEF;
#define TWO_SRC(T, RD, WR, MAX_, MIN_, MAXNM_, MINNM_)                                                            \
        do { T a = RD(s, rn), b = RD(s, rm), r;                                       \
            switch (opcode) {                                                         \
            case 0: r = a * b; break;                                                 \
            case 1: r = a / b; break;                                                 \
            case 2: r = a + b; break;                                                 \
            case 3: r = a - b; break;                                                 \
            case 4: r = MAX_(a, b); break;                                           \
            case 5: r = MIN_(a, b); break;                                            \
            case 6: r = MAXNM_(a, b); break;                                           \
            case 7: r = MINNM_(a, b); break;                                            \
            case 8: r = -(a * b); break;                               /* FNMUL */    \
            default: return A64_UNDEF;                                                \
            }                                                                         \
            WR(s, rd, r); return A64_NEXT; } while (0)
        switch (type) {
        case 0: TWO_SRC(float, rf, wf, a64_vmaxf, a64_vminf, a64_vmaxnmf, a64_vminnmf);
        case 1: TWO_SRC(double, rdd, wd, a64_vmaxd, a64_vmind, a64_vmaxnmd, a64_vminnmd);
        /* the half forms promote to double for min/max only: exact for every finite value, and the
   one thing it can disturb is an f16 NaN payload. Noted rather than hidden. */
        case 3: TWO_SRC(f16, rh, wh, (f16)a64_vmaxd, (f16)a64_vmind, (f16)a64_vmaxnmd, (f16)a64_vminnmd);
        default: return A64_UNDEF;
        }
#undef TWO_SRC
    }

    case 3:                                             /* Floating-point conditional select: FCSEL */
        if (sf) return A64_UNDEF;
        else {
            unsigned src = a64_cond(s, B(i, 15, 12)) ? rn : rm;
            switch (type) {
            case 0: wf(s, rd, rf(s, src)); return A64_NEXT;
            case 1: wd(s, rd, rdd(s, src)); return A64_NEXT;
            case 3: wh(s, rd, rh(s, src)); return A64_NEXT;
            default: return A64_UNDEF;
            }
        }

    default: break;
    }

    /* ───── Conversion between floating-point and integer (bits 15:10 = 0) ───── */
    if (!B(i, 15, 10)) {
        unsigned rmode = B(i, 20, 19), opcode = B(i, 18, 16);

        if (rmode == 0 && opcode >= 6) {                /* FMOV between a GPR and an FP register */
            int to_gpr = (opcode == 6);
            if (type == 0 && !sf) {
                if (to_gpr) { uint32_t b; memcpy(&b, s->v[rn], 4); wz(s, rd, b, 0); }
                else { uint32_t b = (uint32_t)rz(s, rn); vzw(s, (int)rd, &b, 4); }
                return A64_NEXT;
            }
            if (type == 1 && sf) {
                if (to_gpr) { uint64_t b; memcpy(&b, s->v[rn], 8); wz(s, rd, b, 1); }
                else { uint64_t b = rz(s, rn); vzw(s, (int)rd, &b, 8); }
                return A64_NEXT;
            }
            if (type == 3) {
                /* The width follows the V register, not sf — the h-form read as 32 bits was a real
                      lifter defect this week (Chess, the fmov fix). 16 bits in, zero-extended to W or X. */
                if (to_gpr) { uint16_t b; memcpy(&b, s->v[rn], 2); wz(s, rd, b, sf); }
                else { uint16_t b = (uint16_t)rz(s, rn); vzw(s, (int)rd, &b, 2); }
                return A64_NEXT;
            }
            return A64_UNDEF;
        }
        if (rmode == 1 && opcode >= 6 && type == 2 && sf) {
            /* FMOV to/from the high 64 bits of a Q register — how a 128-bit value crosses to GPRs */
            if (opcode == 6) { uint64_t b; memcpy(&b, s->v[rn] + 8, 8); wz(s, rd, b, 1); }
            else { uint64_t b = rz(s, rn); memcpy(s->v[rd] + 8, &b, 8); }
            return A64_NEXT;
        }

        if (opcode == 2 || opcode == 3) {               /* SCVTF / UCVTF (integer) */
            if (rmode) return A64_UNDEF;
            double v = opcode == 2
                     ? (double)(sf ? (int64_t)rz(s, rn) : (int64_t)(int32_t)(uint32_t)rz(s, rn))
                     : (double)(sf ? rz(s, rn) : (uint64_t)(uint32_t)rz(s, rn));
            if (type == 0) wf(s, rd, (float)v);
            else if (type == 1) wd(s, rd, v);
            else if (type == 3) wh(s, rd, (f16)v);
            else return A64_UNDEF;
            return A64_NEXT;
        }

        if (opcode <= 1 || opcode == 4 || opcode == 5) { /* FCVT{N,P,M,Z,A}{S,U} */
            if (type == 2) return A64_UNDEF;
            double v = type == 0 ? (double)rf(s, rn) : type == 1 ? rdd(s, rn) : (double)rh(s, rn);
            unsigned mode;
            int uns;
            if (opcode >= 4) { if (rmode) return A64_UNDEF; mode = 4; uns = (int)(opcode & 1); }
            else { mode = rmode; uns = (int)(opcode & 1); }
            v = rnd(v, mode);
            wz(s, rd, uns ? sat_u(v, sf ? 64 : 32) : sat_s(v, sf ? 64 : 32), sf);
            return A64_NEXT;
        }
        return A64_UNDEF;
    }

    return A64_UNDEF;
}
