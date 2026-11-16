// a64_simdsh.c — Advanced SIMD shift by immediate, across lanes, and three-different (the widening and
// narrowing forms). Arm ARM C4.1.89.
#include "a64_lane.h"
#include <math.h>

int a64_simdsh(a64 *s, uint32_t i) {
    int scalar = BIT(i, 28);
    if (BIT(i, 31)) return A64_UNDEF;   /* the crypto groups share these fields; see a64_simd.c */
    unsigned rd = B(i, 4, 0), rn = B(i, 9, 5), rm = B(i, 20, 16);
    unsigned U = BIT(i, 29);
    int Q = scalar ? 1 : (int)BIT(i, 30);

    /* ───────── shift by immediate ───────── */
    if (B(i, 28, 23) == (scalar ? 0x3eu : 0x1eu) && BIT(i, 10) && B(i, 22, 19)) {
        unsigned immh = B(i, 22, 19), immb = B(i, 18, 16), opcode = B(i, 15, 11);
        unsigned hsb = (unsigned)(31 - __builtin_clz(immh));    /* HighestSetBit(immh) */
        unsigned eb = 1u << hsb;                                /* element bytes, 1/2/4/8 */
        unsigned imm = (immh << 3) | immb;

        /* the narrowing and lengthening forms cannot have a 64-bit element on the narrow side */
        if ((opcode >= 0x10 && opcode <= 0x14) && immh >= 8) return A64_UNDEF;
        if (!scalar && eb == 8 && !Q && opcode < 0x10) return A64_UNDEF;

        if (opcode == 0x14) {                                   /* SSHLL / USHLL (SXTL / UXTL at shift 0) */
            if (scalar) return A64_UNDEF;
            unsigned sh = imm - eb * 8;
            unsigned lanes = 8u / eb;
            const uint8_t *src = s->v[rn] + (Q ? 8 : 0);        /* the 2 form widens the high half */
            vreg t = (vreg){{0}};
            for (unsigned l = 0; l < lanes; l++) {
                uint64_t x = lrd(src, l, eb);
                uint64_t w = U ? x : (uint64_t)sgn(x, eb);
                lwr(t.b, l, 2 * eb, (w << sh) & mask_eb(2 * eb));
            }
            memcpy(s->v[rd], t.b, 16);
            return A64_NEXT;
        }

        if (opcode >= 0x10 && opcode <= 0x13 && !(opcode <= 0x11 && !U)) {
            /* SQSHRUN/SQRSHRUN (U=1, 0x10/0x11) and SQSHRN/UQSHRN/SQRSHRN/UQRSHRN (0x12/0x13).
               The source element is twice the output's and the saturation is at the output width; the
                  three-way split (signed in/out, signed in + unsigned out, unsigned both) is what separates
                  these six mnemonics from one another. */
            unsigned sh = eb * 16 - imm;
            unsigned lanes = scalar ? 1 : 8u / eb;
            int round = (opcode == 0x11 || opcode == 0x13);
            int uns_in = (opcode >= 0x12) && U;
            int uns_out = U;
            vreg t = (vreg){{0}};
            if (!scalar && Q) memcpy(t.b, s->v[rd], 8);
            for (unsigned l = 0; l < lanes; l++) {
                uint64_t x = lrd(s->v[rn], l, 2 * eb), r;
                unsigned w2 = 2 * eb * 8;
                if (uns_in) {
                    if (round && sh) x += 1ULL << (sh - 1);
                    r = sat_uns(sh >= w2 ? 0 : (x >> sh), 0, eb);
                } else {
                    int64_t v = sgn(x, 2 * eb);
                    if (round && sh) v += (int64_t)1 << (sh - 1);
                    v >>= (sh >= w2 ? w2 - 1 : sh);
                    r = uns_out ? sat_uns((uint64_t)v, v < 0, eb) : sat_sgn(v, eb);
                }
                lwr(t.b + ((!scalar && Q) ? 8 : 0), l, eb, r);
            }
            if (scalar) { memset(s->v[rd], 0, 16); memcpy(s->v[rd], t.b, eb); return A64_NEXT; }
            memcpy(s->v[rd], t.b, 16);
            if (!Q) memset(s->v[rd] + 8, 0, 8);
            return A64_NEXT;
        }
        if (opcode == 0x10 || opcode == 0x11) {                 /* SHRN / RSHRN */
            if (scalar || U) return A64_UNDEF;
            unsigned sh = eb * 16 - imm;                        /* esize is the output size here */
            unsigned lanes = 8u / eb;
            vreg t = (vreg){{0}};
            if (Q) memcpy(t.b, s->v[rd], 8);
            for (unsigned l = 0; l < lanes; l++) {
                uint64_t x = lrd(s->v[rn], l, 2 * eb);
                if (opcode == 0x11 && sh) x += 1ULL << (sh - 1);    /* RSHRN rounds */
                lwr(t.b + (Q ? 8 : 0), l, eb, (x >> sh) & mask_eb(eb));
            }
            memcpy(s->v[rd], t.b, 16);
            if (!Q) memset(s->v[rd] + 8, 0, 8);
            return A64_NEXT;
        }

        unsigned lanes = scalar ? 1 : (Q ? 16u : 8u) / eb;
        if (scalar && eb != 8 && opcode != 0x0a) return A64_UNDEF;
        vreg t = (vreg){{0}};
        unsigned w = eb * 8;

        for (unsigned l = 0; l < lanes; l++) {
            uint64_t x = lrd(s->v[rn], l, eb), d = lrd(s->v[rd], l, eb), m = mask_eb(eb), r;
            switch (opcode) {
            case 0x00: case 0x02: case 0x04: case 0x06: {       /* S/USHR, S/USRA, S/URSHR, S/URSRA */
                unsigned sh = 2 * w - imm;
                int round = (opcode == 0x04 || opcode == 0x06);
                int acc = (opcode == 0x02 || opcode == 0x06);
                if (U) {
                    uint64_t v = x;
                    if (round && sh <= w) v += (sh == 0 ? 0 : (1ULL << (sh - 1)));
                    r = sh >= w + 1 ? 0 : (sh == w ? (round ? (v >> sh) : 0) : (v >> sh));
                    if (sh >= w && !round) r = 0;
                } else {
                    int64_t v = sgn(x, eb);
                    if (round && sh >= 1 && sh <= w) v += (int64_t)1 << (sh - 1);
                    unsigned n = sh >= w ? w - 1 : sh;          /* an arithmetic shift saturates to the sign */
                    r = (uint64_t)(v >> n) & m;
                    if (sh >= w && round) r = 0;
                }
                if (acc) r = (d + r) & m;
                break;
            }
            case 0x08: {                                        /* SRI: insert the shifted bits, keeping Vd's */
                if (!U) return A64_UNDEF;
                unsigned sh = 2 * w - imm;
                if (sh >= w) { r = d; break; }
                uint64_t keep = sh == 0 ? 0 : (m << (w - sh)) & m;
                r = ((x >> sh) | (d & keep)) & m;
                break;
            }
            case 0x0a: {                                        /* SHL (U=0) / SLI (U=1) */
                unsigned sh = imm - w;
                if (sh >= w) return A64_UNDEF;
                if (!U) r = (x << sh) & m;
                else {
                    uint64_t keep = sh == 0 ? 0 : (m >> (w - sh));
                    r = ((x << sh) | (d & keep)) & m;
                }
                break;
            }
            case 0x1c: {                                        /* SCVTF / UCVTF (fixed point) */
                if (eb != 4 && eb != 8) return A64_UNDEF;
                unsigned fbits = 2 * w - imm;
                double v = (U ? (double)x : (double)sgn(x, eb)) / ldexp(1.0, (int)fbits);
                if (eb == 8) { memcpy(t.b + 8 * l, &v, 8); }
                else { float f = (float)v; memcpy(t.b + 4 * l, &f, 4); }
                continue;
            }
            case 0x1f: {                                        /* FCVTZS / FCVTZU (fixed point) */
                if (eb != 4 && eb != 8) return A64_UNDEF;
                unsigned fbits = 2 * w - imm;
                double v;
                if (eb == 8) memcpy(&v, s->v[rn] + 8 * l, 8);
                else { float f; memcpy(&f, s->v[rn] + 4 * l, 4); v = (double)f; }
                v = __builtin_trunc(v * ldexp(1.0, (int)fbits));
                if (isnan(v)) r = 0;
                else if (U) r = v <= 0 ? 0 : (v >= ldexp(1.0, (int)w) ? m : (uint64_t)v);
                else {
                    double hi = ldexp(1.0, (int)w - 1);
                    r = v >= hi ? (uint64_t)sat_sgn(INT64_MAX, eb)
                      : (v < -hi ? (uint64_t)sat_sgn(INT64_MIN, eb) : ((uint64_t)(int64_t)v & m));
                }
                break;
            }
            default: return A64_UNDEF;                          /* the saturating shifts */
            }
            lwr(t.b, l, eb, r);
        }
        if (scalar) { memset(s->v[rd], 0, 16); memcpy(s->v[rd], t.b, eb); }
        else vput(s, rd, &t, Q);
        return A64_NEXT;
    }

    /* ───── scalar pairwise: ADDP, FADDP, FMAXP/FMINP, FMAXNMP/FMINNMP — the two lanes of one register ───── */
    if (scalar && B(i, 28, 24) == 0x1e && B(i, 21, 17) == 0x18 && B(i, 11, 10) == 2) {
        unsigned opcode = B(i, 16, 12);
        if (!U) {                                            /* ADDP Dd, Vn.2D */
            if (opcode != 0x1b || B(i, 23, 22) != 3) return A64_UNDEF;
            uint64_t r = lrd(s->v[rn], 0, 8) + lrd(s->v[rn], 1, 8);
            memset(s->v[rd], 0, 16);
            memcpy(s->v[rd], &r, 8);
            return A64_NEXT;
        }
        int dbl = (int)BIT(i, 22), mn = (int)BIT(i, 23);      /* size<1> picks the min form */
        if (dbl) {
            double p, q, r;
            memcpy(&p, s->v[rn], 8);
            memcpy(&q, s->v[rn] + 8, 8);
            switch (opcode) {
            case 0x0c: r = mn ? a64_vminnmd(p, q) : a64_vmaxnmd(p, q); break;
            case 0x0d: if (mn) return A64_UNDEF; r = p + q; break;          /* FADDP */
            case 0x0f: r = mn ? a64_vmind(p, q) : a64_vmaxd(p, q); break;
            default: return A64_UNDEF;
            }
            memset(s->v[rd], 0, 16);
            memcpy(s->v[rd], &r, 8);
        } else {
            float p, q, r;
            memcpy(&p, s->v[rn], 4);
            memcpy(&q, s->v[rn] + 4, 4);
            switch (opcode) {
            case 0x0c: r = mn ? a64_vminnmf(p, q) : a64_vmaxnmf(p, q); break;
            case 0x0d: if (mn) return A64_UNDEF; r = p + q; break;
            case 0x0f: r = mn ? a64_vminf(p, q) : a64_vmaxf(p, q); break;
            default: return A64_UNDEF;
            }
            memset(s->v[rd], 0, 16);
            memcpy(s->v[rd], &r, 4);
        }
        return A64_NEXT;
    }

    /* ───────── across lanes: ADDV, SADDLV/UADDLV, SMAXV/UMAXV, SMINV/UMINV, FMAXV/FMINV/FMAXNMV/FMINNMV ───── */
    if (!scalar && B(i, 28, 24) == 0x0e && B(i, 21, 17) == 0x18 && B(i, 11, 10) == 2) {
        unsigned opcode = B(i, 16, 12), size = B(i, 23, 22);

        if (U && (opcode == 0x0c || opcode == 0x0f)) {           /* the FP reductions */
            int dbl = (int)BIT(i, 22);
            if (dbl || !Q) return A64_UNDEF;                     /* only 4 x single is defined */
            int nm = (opcode == 0x0c);
            int mn = (int)BIT(i, 23);                            /* size<1> picks min rather than max */
            float acc; memcpy(&acc, s->v[rn], 4);
            for (unsigned l = 1; l < 4; l++) {
                float x; memcpy(&x, s->v[rn] + 4 * l, 4);
                if (isnan(x) || isnan(acc)) {
                    if (nm) acc = isnan(acc) ? x : acc;          /* the NM forms ignore a NaN operand */
                    else acc = acc + x;                          /* FMAXV/FMINV propagate it */
                } else acc = mn ? (x < acc ? x : acc) : (x > acc ? x : acc);
            }
            memset(s->v[rd], 0, 16);
            memcpy(s->v[rd], &acc, 4);
            return A64_NEXT;
        }

        unsigned eb = 1u << size;
        if (size == 3 || (size == 2 && !Q)) return A64_UNDEF;    /* a reduction needs at least 4 lanes */
        unsigned lanes = (Q ? 16u : 8u) / eb;

        if (opcode == 0x03) {                                    /* SADDLV / UADDLV: the sum is twice as wide */
            uint64_t acc = 0;
            for (unsigned l = 0; l < lanes; l++) {
                uint64_t x = lrd(s->v[rn], l, eb);
                acc += U ? x : (uint64_t)sgn(x, eb);
            }
            memset(s->v[rd], 0, 16);
            acc &= mask_eb(2 * eb);
            memcpy(s->v[rd], &acc, 2 * eb);
            return A64_NEXT;
        }

        uint64_t acc = lrd(s->v[rn], 0, eb);
        for (unsigned l = 1; l < lanes; l++) {
            uint64_t x = lrd(s->v[rn], l, eb);
            switch (opcode) {
            case 0x0a: acc = U ? (x > acc ? x : acc) : (uint64_t)(sgn(x, eb) > sgn(acc, eb) ? x : acc); break;
            case 0x1a: acc = U ? (x < acc ? x : acc) : (uint64_t)(sgn(x, eb) < sgn(acc, eb) ? x : acc); break;
            case 0x1b: if (U) return A64_UNDEF; acc = (acc + x) & mask_eb(eb); break;   /* ADDV */
            default: return A64_UNDEF;
            }
        }
        memset(s->v[rd], 0, 16);
        acc &= mask_eb(eb);
        memcpy(s->v[rd], &acc, eb);
        return A64_NEXT;
    }

    /* ───────── three different: the widening and high-narrowing forms ───────── */
    if (!scalar && B(i, 28, 24) == 0x0e && BIT(i, 21) && B(i, 11, 10) == 0) {
        unsigned opcode = B(i, 15, 12), size = B(i, 23, 22);
        if (opcode == 0x0e && size == 3 && !U) {                 /* PMULL / PMULL2, 64 x 64 -> 128 */
            uint64_t x = lrd(s->v[rn], Q ? 1 : 0, 8), y = lrd(s->v[rm], Q ? 1 : 0, 8);
            unsigned __int128 r = 0;
            for (int k = 0; k < 64; k++) if ((y >> k) & 1) r ^= (unsigned __int128)x << k;
            uint64_t lo = (uint64_t)r, hi = (uint64_t)(r >> 64);
            memcpy(s->v[rd], &lo, 8);
            memcpy(s->v[rd] + 8, &hi, 8);
            return A64_NEXT;
        }
        if (size == 3) return A64_UNDEF;
        unsigned eb = 1u << size;                                /* the narrow element */
        unsigned lanes = 8u / eb;
        /* Q IS NOT A lane count here — it picks which half of the narrow source is used ("the 2 forms").
              The result is always the full 128 bits, 2*eb per lane. */
        const uint8_t *sn = s->v[rn] + (Q ? 8 : 0), *sm = s->v[rm] + (Q ? 8 : 0);
        vreg t = (vreg){{0}};

        if (opcode == 0x04 || opcode == 0x06) {                  /* ADDHN / SUBHN: narrow, keep the high half */
            if (Q) memcpy(t.b, s->v[rd], 8);
            for (unsigned l = 0; l < lanes; l++) {
                uint64_t x = lrd(s->v[rn], l, 2 * eb), y = lrd(s->v[rm], l, 2 * eb);
                uint64_t r = (opcode == 0x04 ? x + y : x - y) & mask_eb(2 * eb);
                lwr(t.b + (Q ? 8 : 0), l, eb, (r >> (eb * 8)) & mask_eb(eb));
            }
            memcpy(s->v[rd], t.b, 16);
            if (!Q) memset(s->v[rd] + 8, 0, 8);
            return A64_NEXT;
        }

        for (unsigned l = 0; l < lanes; l++) {
            uint64_t wm = mask_eb(2 * eb), r;
            uint64_t xa = lrd(sn, l, eb), ya = lrd(sm, l, eb);
            uint64_t wx = U ? xa : (uint64_t)sgn(xa, eb);        /* widened operands */
            uint64_t wy = U ? ya : (uint64_t)sgn(ya, eb);
            uint64_t dw = lrd(s->v[rd], l, 2 * eb);
            uint64_t nw = lrd(s->v[rn], l, 2 * eb);              /* for the W forms Vn is already wide */
            switch (opcode) {
            case 0x00: r = wx + wy; break;                                           /* SADDL / UADDL */
            case 0x01: r = nw + wy; break;                                           /* SADDW / UADDW */
            case 0x02: r = wx - wy; break;                                           /* SSUBL / USUBL */
            case 0x03: r = nw - wy; break;                                           /* SSUBW / USUBW */
            case 0x05: case 0x07: {                                                  /* SABAL/UABAL, SABDL/UABDL */
                uint64_t ad = U ? (xa > ya ? xa - ya : ya - xa)
                                : (uint64_t)(sgn(xa, eb) > sgn(ya, eb) ? sgn(xa, eb) - sgn(ya, eb)
                                                                       : sgn(ya, eb) - sgn(xa, eb));
                r = opcode == 0x05 ? dw + ad : ad;
                break;
            }
            case 0x08: r = dw + wx * wy; break;                                      /* SMLAL / UMLAL */
            case 0x0a: r = dw - wx * wy; break;                                      /* SMLSL / UMLSL */
            case 0x0c: r = wx * wy; break;                                           /* SMULL / UMULL */
            case 0x0e: {                                                             /* PMULL: 8x8 -> 16 */
                if (U || size != 0) return A64_UNDEF;
                r = 0;
                for (int k = 0; k < 8; k++) if ((ya >> k) & 1) r ^= xa << k;
                break;
            }
            default: return A64_UNDEF;                                               /* the SQDMULL family */
            }
            lwr(t.b, l, 2 * eb, r & wm);
        }
        memcpy(s->v[rd], t.b, 16);
        return A64_NEXT;
    }

    return A64_UNDEF;
}
