// a64_simdidx.c — Advanced SIMD "vector x indexed element": one lane of Vm multiplies every lane of Vn.
// Arm ARM C4.1.89. This is one encoding group and 58 of the forms the certifier listed as unimplemented:
// every by-lane fmul/fmla/fmls, mul/mla/mls, the widening smull/umull/smlal/umlal family and sqdmulh.
#include "a64_lane.h"
#include <math.h>

int a64_simdidx(a64 *s, uint32_t i) {
    int scalar = BIT(i, 28);
    if (BIT(i, 31)) return A64_UNDEF;
    if (scalar ? B(i, 28, 24) != 0x1f : B(i, 28, 24) != 0x0f) return A64_UNDEF;
    if (BIT(i, 10)) return A64_UNDEF;           /* bit 10 clear is what separates this from the shifts */

    unsigned rd = B(i, 4, 0), rn = B(i, 9, 5);
    unsigned U = BIT(i, 29), size = B(i, 23, 22), opcode = B(i, 15, 12);
    unsigned L = BIT(i, 21), M = BIT(i, 20), H = BIT(i, 11);
    int Q = scalar ? 1 : (int)BIT(i, 30);

    /* Dot products by element select a four-byte group from Vm. */
    if (!scalar && size == 2 && opcode == 0x0e) {
        unsigned rm = B(i, 20, 16), index = (H << 1) | L;
        unsigned lanes = Q ? 4u : 2u;
        vreg t = (vreg){{0}};
        for (unsigned l = 0; l < lanes; l++) {
            uint32_t sum = (uint32_t)lrd(s->v[rd], l, 4);
            for (unsigned k = 0; k < 4; k++) {
                int a = U ? s->v[rn][4 * l + k] : (int)(int8_t)s->v[rn][4 * l + k];
                int b = U ? s->v[rm][4 * index + k] : (int)(int8_t)s->v[rm][4 * index + k];
                sum += (uint32_t)(a * b);
            }
            lwr(t.b, l, 4, sum);
        }
        vput(s, rd, &t, Q);
        return A64_NEXT;
    }

    /* The element size and the lane index come from one rule for every instruction in the group, integer
          and floating point alike — and for 16-bit elements the index steals Vm's top bit, so Vm is
          restricted to v0..v15 there. Getting this wrong reads the wrong lane of the wrong register. */
    unsigned eb, index, rm;
    switch (size) {
    case 1: eb = 2; index = (H << 2) | (L << 1) | M; rm = B(i, 19, 16); break;
    case 2: eb = 4; index = (H << 1) | L; rm = (M << 4) | B(i, 19, 16); break;
    case 3:
        if (L) return A64_UNDEF;
        eb = 8; index = H; rm = (M << 4) | B(i, 19, 16);
        break;
    default: return A64_UNDEF;
    }

    /* ───── the floating-point forms: FMLA, FMLS, FMUL, FMULX ───── */
    if (opcode == 0x1 || opcode == 0x5 || opcode == 0x9) {
        if (size < 2) return A64_UNDEF;             /* the FP16 by-element forms are a separate table */
        int dbl = (eb == 8);
        if (!scalar && dbl && !Q) return A64_UNDEF;
        unsigned lanes = scalar ? 1 : (Q ? 16u : 8u) / eb;
        if (opcode == 0x9 && U && 0) return A64_UNDEF;
        vreg t = (vreg){{0}};
        for (unsigned l = 0; l < lanes; l++) {
            if (dbl) {
                double x, y, d, r, n_;
                memcpy(&x, s->v[rn] + 8 * l, 8);
                memcpy(&y, s->v[rm] + 8 * index, 8);
                memcpy(&d, s->v[rd] + 8 * l, 8);
                if (opcode == 0x9) {
                    if (a64_fpnan2d(x, y, &n_)) r = n_;
                    else if (U && ((x == 0 && isinf(y)) || (y == 0 && isinf(x))))
                        r = (signbit(x) ^ signbit(y)) ? -2.0 : 2.0;      /* FMULX */
                    else r = x * y;
                } else r = fma(opcode == 0x5 ? -x : x, y, d);            /* FMLS / FMLA, fused */
                memcpy(t.b + 8 * l, &r, 8);
            } else {
                float x, y, d, r, n_;
                memcpy(&x, s->v[rn] + 4 * l, 4);
                memcpy(&y, s->v[rm] + 4 * index, 4);
                memcpy(&d, s->v[rd] + 4 * l, 4);
                if (opcode == 0x9) {
                    if (a64_fpnan2f(x, y, &n_)) r = n_;
                    else if (U && ((x == 0 && isinf(y)) || (y == 0 && isinf(x))))
                        r = (signbit(x) ^ signbit(y)) ? -2.0f : 2.0f;
                    else r = x * y;
                } else r = fmaf(opcode == 0x5 ? -x : x, y, d);
                memcpy(t.b + 4 * l, &r, 4);
            }
        }
        if (scalar) { memset(s->v[rd], 0, 16); memcpy(s->v[rd], t.b, eb); }
        else vput(s, rd, &t, Q);
        return A64_NEXT;
    }

    if (size == 3) return A64_UNDEF;                /* no integer form has a 64-bit element */
    uint64_t y = lrd(s->v[rm], index, eb);
    int64_t sy = sgn(y, eb);

    /* ───── the widening forms: SMLAL/UMLAL, SMLSL/UMLSL, SMULL/UMULL (and their 2 forms) ───── */
    if (opcode == 0x2 || opcode == 0x6 || opcode == 0xa) {
        if (scalar) return A64_UNDEF;
        unsigned lanes = 8u / eb;
        const uint8_t *src = s->v[rn] + (Q ? 8 : 0);        /* the 2 form widens the high half of Vn */
        vreg t = (vreg){{0}};
        for (unsigned l = 0; l < lanes; l++) {
            uint64_t x = lrd(src, l, eb);
            uint64_t p = U ? x * y : (uint64_t)(sgn(x, eb) * sy);
            uint64_t acc = lrd(s->v[rd], l, 2 * eb);
            uint64_t r = opcode == 0x2 ? acc + p : opcode == 0x6 ? acc - p : p;
            lwr(t.b, l, 2 * eb, r & mask_eb(2 * eb));
        }
        memcpy(s->v[rd], t.b, 16);
        return A64_NEXT;
    }

    /* ───── the same-width forms: MUL, MLA, MLS, SQDMULH, SQRDMULH ───── */
    unsigned lanes = scalar ? 1 : (Q ? 16u : 8u) / eb;
    vreg t = (vreg){{0}};
    for (unsigned l = 0; l < lanes; l++) {
        uint64_t x = lrd(s->v[rn], l, eb), m = mask_eb(eb), r;
        uint64_t d = lrd(s->v[rd], l, eb);
        switch (opcode) {
        case 0x0: if (!U) return A64_UNDEF; r = (d + x * y) & m; break;      /* MLA */
        case 0x4: if (!U) return A64_UNDEF; r = (d - x * y) & m; break;      /* MLS */
        case 0x8: if (U) return A64_UNDEF; r = (x * y) & m; break;           /* MUL */
        case 0xc: case 0xd: {                                                /* SQDMULH / SQRDMULH */
            if (U) return A64_UNDEF;
            __int128 p = (__int128)sgn(x, eb) * sy * 2;
            if (opcode == 0xd) p += (__int128)1 << (eb * 8 - 1);             /* the rounding form */
            r = sat_sgn((int64_t)(p >> (eb * 8)), eb);
            break;
        }
        default: return A64_UNDEF;       /* SQDMLAL/SQDMLSL/SQDMULL, SDOT/UDOT */
        }
        lwr(t.b, l, eb, r);
    }
    if (scalar) { memset(s->v[rd], 0, 16); memcpy(s->v[rd], t.b, eb); }
    else vput(s, rd, &t, Q);
    return A64_NEXT;
}
