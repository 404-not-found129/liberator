// a64_simd3.c — Advanced SIMD three-same (integer) and two-register misc (integer). Arm ARM C4.1.89.
// Both the vector forms (bits 28:24 = 01110) and the scalar forms (bits 31:30 = 01, bits 28:24 = 11110)
// land here: the scalar form is the same operation on one 64-bit lane, so it shares every case below.
#include "a64_lane.h"

/* the shift-by-register family: the amount is the signed low byte of the second operand's lane */
static uint64_t shl_sgn(int64_t e, int sh, unsigned eb, int round) {
    unsigned w = eb * 8;
    if (sh >= 0) return (sh >= (int)w) ? 0 : ((uint64_t)(e << sh) & mask_eb(eb));
    int n = -sh;
    if (round) {
        if (n > (int)w) return 0;
        int64_t add = (int64_t)1 << (n - 1);
        return (uint64_t)((e + add) >> n) & mask_eb(eb);
    }
    if (n >= (int)w) n = (int)w - 1;    /* an arithmetic right shift saturates to the sign */
    return (uint64_t)(e >> n) & mask_eb(eb);
}
static uint64_t shl_uns(uint64_t e, int sh, unsigned eb, int round) {
    unsigned w = eb * 8;
    if (sh >= 0) {
        return (sh >= (int)w) ? 0 : ((e << sh) & mask_eb(eb));
    }
    int n = -sh;
    if (n > (int)w) return 0;
    if (round) {
        if (n == (int)w) return (e >> (n - 1)) & 1 ? 1 : 0;
        return ((e + (1ULL << (n - 1))) >> n) & mask_eb(eb);
    }
    return n == (int)w ? 0 : (e >> n);
}

/* A saturating left shift must be judged at the element width, not at 64 bits. Checking
      `(e << sh) >> sh != e` in int64 lets a 32-bit lane that overflowed its own 32 bits but still fits in 64
      go unsaturated, and sqshl.4s then differs from hardware. Computed in 128 bits and saturated once. */
static uint64_t sq_shift(int64_t e, int sh, unsigned eb, int round) {
    if (sh >= 0) {
        /* sh comes from A SIGNED BYTE, so it reaches 127: `(__int128)e << sh` is itself undefined past
              127 bits, and any shift of at least the element width overflows unless the lane is zero.
              sqshl.4s still differed at state 12 with only a `sh >= 127` guard. */
        if (sh >= (int)(eb * 8)) return e == 0 ? 0 : sat_sgn(e < 0 ? INT64_MIN : INT64_MAX, eb);
        __int128 r = (__int128)e << sh;
        __int128 hi = ((__int128)1 << (eb * 8 - 1)) - 1, lo = -((__int128)1 << (eb * 8 - 1));
        if (r > hi) return (uint64_t)hi & mask_eb(eb);
        if (r < lo) return (uint64_t)lo & mask_eb(eb);
        return (uint64_t)r & mask_eb(eb);
    }
    unsigned n = (unsigned)(-sh), w = eb * 8;
    if (round) {
        if (n > w) return 0;
        return (uint64_t)((e + ((int64_t)1 << (n - 1))) >> n) & mask_eb(eb);
    }
    if (n >= w) n = w - 1;
    return (uint64_t)(e >> n) & mask_eb(eb);
}
static uint64_t uq_shift(uint64_t a, int sh, unsigned eb, int round) {
    if (sh >= 0) {
        if (sh >= (int)(eb * 8)) return a ? mask_eb(eb) : 0;
        unsigned __int128 r = (unsigned __int128)a << sh;
        unsigned __int128 hi = mask_eb(eb);
        return (uint64_t)(r > hi ? hi : r);
    }
    unsigned n = (unsigned)(-sh), w = eb * 8;
    if (n > w) return 0;
    if (round) {
        if (n == w) return (a >> (n - 1)) & 1;
        return ((a + (1ULL << (n - 1))) >> n) & mask_eb(eb);
    }
    return n == w ? 0 : (a >> n);
}

/* polynomial multiply, 8x8 -> 8 (PMUL) */
static uint64_t pmul8(uint64_t a, uint64_t b) {
    uint64_t r = 0;
    for (int k = 0; k < 8; k++) if ((b >> k) & 1) r ^= a << k;
    return r & 0xff;
}

int a64_simd3(a64 *s, uint32_t i) {
    int scalar = BIT(i, 28);                /* the Advanced SIMD *scalar* encodings */
    if (BIT(i, 31)) return A64_UNDEF;   /* the crypto groups share these fields; see a64_simd.c */
    unsigned rd = B(i, 4, 0), rn = B(i, 9, 5), rm = B(i, 20, 16);
    unsigned U = BIT(i, 29), size = B(i, 23, 22);
    int Q = scalar ? 1 : (int)BIT(i, 30);

    /* Integer dot products accumulate four byte products into each 32-bit lane. */
    if ((i & 0x9fe0fc00u) == 0x0e809400u) {
        vreg t = (vreg){{0}};
        unsigned lanes = Q ? 4u : 2u;
        for (unsigned l = 0; l < lanes; l++) {
            uint32_t sum = (uint32_t)lrd(s->v[rd], l, 4);
            for (unsigned k = 0; k < 4; k++) {
                unsigned byte = 4 * l + k;
                int a = U ? s->v[rn][byte] : (int)(int8_t)s->v[rn][byte];
                int b = U ? s->v[rm][byte] : (int)(int8_t)s->v[rm][byte];
                sum += (uint32_t)(a * b);
            }
            lwr(t.b, l, 4, sum);
        }
        vput(s, rd, &t, Q);
        return A64_NEXT;
    }

    /* ───────── three same ───────── */
    if (BIT(i, 21) && BIT(i, 10) && (scalar ? B(i, 28, 24) == 0x1e : B(i, 28, 24) == 0x0e)) {
        unsigned opcode = B(i, 15, 11);
        if (opcode >= 0x18) return A64_UNDEF;           /* the FP forms: a64_simdfp.c */

        unsigned eb = 1u << size;
        unsigned lanes = scalar ? 1 : (Q ? 16u : 8u) / eb;
        if (scalar) {
            /* ONLY A few operations have A scalar form, and all but the comparisons are 64-bit only.
                  Accepting any size here would execute `add b0, b1, b2`, which does not exist. */
            if (size != 3 && !(opcode == 0x06 || opcode == 0x07 || opcode == 0x11 || opcode == 0x16))
                return A64_UNDEF;
            eb = 1u << size;
            lanes = 1;
        } else if (size == 3 && !Q && opcode != 0x03) {
            return A64_UNDEF;                           /* 1 x 64-bit lane is not a vector form */
        }

        vreg t = (vreg){{0}};

        /* the logical group is one opcode with four meanings per U, on the whole register */
        if (opcode == 0x03) {
            unsigned nb = Q ? 16u : 8u;
            for (unsigned k = 0; k < nb; k++) {
                uint8_t a = s->v[rn][k], b = s->v[rm][k], d = s->v[rd][k];
                switch ((U << 2) | size) {
                case 0: t.b[k] = a & b; break;                      /* AND */
                case 1: t.b[k] = a & (uint8_t)~b; break;            /* BIC */
                case 2: t.b[k] = a | b; break;                      /* ORR */
                case 3: t.b[k] = a | (uint8_t)~b; break;            /* ORN */
                case 4: t.b[k] = a ^ b; break;                      /* EOR */
                /* BSL/BIT/BIF all read Vd AS AN OPERAND. BSL selects with Vd, BIT/BIF insert into Vd under
                      Vm's mask — three instructions that are wrong in three different ways if Vd is treated
                      as write-only, and all three are what the compiler emits for a vector select. */
                case 5: t.b[k] = (uint8_t)((a & d) | (b & (uint8_t)~d)); break;    /* BSL */
                case 6: t.b[k] = (uint8_t)((d & (uint8_t)~b) | (a & b)); break;    /* BIT */
                case 7: t.b[k] = (uint8_t)((d & b) | (a & (uint8_t)~b)); break;    /* BIF */
                default: return A64_UNDEF;
                }
            }
            vput(s, rd, &t, Q);
            return A64_NEXT;
        }

        /* the pairwise group reads the concatenation of Vn and Vm */
        if (opcode == 0x14 || opcode == 0x15 || opcode == 0x17) {
            if (scalar) return A64_UNDEF;
            unsigned nb = Q ? 16u : 8u;
            uint8_t cat[32];
            memcpy(cat, s->v[rn], nb);
            memcpy(cat + nb, s->v[rm], nb);
            for (unsigned l = 0; l < lanes; l++) {
                uint64_t a = lrd(cat, 2 * l, eb), b = lrd(cat, 2 * l + 1, eb), r;
                switch (opcode) {
                case 0x14: r = U ? (a > b ? a : b) : (uint64_t)(sgn(a, eb) > sgn(b, eb) ? a : b); break;
                case 0x15: r = U ? (a < b ? a : b) : (uint64_t)(sgn(a, eb) < sgn(b, eb) ? a : b); break;
                default:   r = (a + b) & mask_eb(eb); break;         /* ADDP */
                }
                lwr(t.b, l, eb, r);
            }
            vput(s, rd, &t, Q);
            return A64_NEXT;
        }

        for (unsigned l = 0; l < lanes; l++) {
            uint64_t a = lrd(s->v[rn], l, eb), b = lrd(s->v[rm], l, eb);
            uint64_t d = lrd(s->v[rd], l, eb);
            int64_t sa = sgn(a, eb), sb = sgn(b, eb);
            uint64_t m = mask_eb(eb), r;
            int sh = (int)(int8_t)(b & 0xff);
            switch (opcode) {
            case 0x00: r = U ? ((a + b) >> 1) : (uint64_t)((sa + sb) >> 1); break;          /* S/UHADD */
            case 0x01: r = U ? sat_uns(a + b, 0, eb) : sat_sgn(sa + sb, eb); break;         /* S/UQADD */
            case 0x02: r = U ? ((a + b + 1) >> 1) : (uint64_t)((sa + sb + 1) >> 1); break;  /* S/URHADD */
            case 0x04: r = U ? ((a - b) >> 1) : (uint64_t)((sa - sb) >> 1); break;          /* S/UHSUB */
            case 0x05: r = U ? (a >= b ? a - b : 0) : sat_sgn(sa - sb, eb); break;          /* S/UQSUB */
            case 0x06: r = (U ? a > b : sa > sb) ? m : 0; break;                            /* CMGT/CMHI */
            case 0x07: r = (U ? a >= b : sa >= sb) ? m : 0; break;                          /* CMGE/CMHS */
            case 0x08: r = U ? shl_uns(a, sh, eb, 0) : shl_sgn(sa, sh, eb, 0); break; /* S/USHL */
            case 0x09: r = U ? uq_shift(a, sh, eb, 0) : sq_shift(sa, sh, eb, 0); break;     /* S/UQSHL */
            case 0x0a: r = U ? shl_uns(a, sh, eb, 1) : shl_sgn(sa, sh, eb, 1); break; /* S/URSHL */
            case 0x0b: r = U ? uq_shift(a, sh, eb, 1) : sq_shift(sa, sh, eb, 1); break;     /* S/UQRSHL */
            case 0x0c: r = U ? (a > b ? a : b) : (uint64_t)(sa > sb ? sa : sb) & m; break;  /* S/UMAX */
            case 0x0d: r = U ? (a < b ? a : b) : (uint64_t)(sa < sb ? sa : sb) & m; break;  /* S/UMIN */
            case 0x0e: r = U ? (a > b ? a - b : b - a)                                      /* S/UABD */
                             : (uint64_t)(sa > sb ? sa - sb : sb - sa) & m; break;
            case 0x0f: r = (d + (U ? (a > b ? a - b : b - a)                                /* S/UABA */
                                   : (uint64_t)(sa > sb ? sa - sb : sb - sa))) & m; break;
            case 0x10: r = (U ? a - b : a + b) & m; break;                                  /* SUB / ADD */
            case 0x11: r = U ? (a == b ? m : 0) : ((a & b) ? m : 0); break;                 /* CMEQ / CMTST */
            case 0x12: r = (U ? d - a * b : d + a * b) & m; break;                          /* MLS / MLA */
            case 0x13:                                                                      /* PMUL / MUL */
                if (U) { if (size) return A64_UNDEF; r = pmul8(a, b); }
                else r = (a * b) & m;
                break;
            case 0x16: {                                                                    /* SQDMULH/SQRDMULH */
                if (size == 0 || size == 3) return A64_UNDEF;
                __int128 p = (__int128)sa * sb * 2;
                if (U) p += (__int128)1 << (eb * 8 - 1);                                    /* rounding */
                r = sat_sgn((int64_t)(p >> (eb * 8)), eb);
                break; }
            default: return A64_UNDEF;
            }
            lwr(t.b, l, eb, r);
        }
        if (scalar) { memset(s->v[rd], 0, 16); memcpy(s->v[rd], t.b, eb); }
        else vput(s, rd, &t, Q);
        return A64_NEXT;
    }

    /* ───────── two-register misc ───────── */
    if (B(i, 21, 17) == 0x10 && B(i, 11, 10) == 2 &&
        (scalar ? B(i, 28, 24) == 0x1e : B(i, 28, 24) == 0x0e)) {
        unsigned opcode = B(i, 16, 12);
        if (opcode >= 0x0c) return A64_UNDEF;           /* the FP and narrowing forms: a64_simdfp.c */

        unsigned eb = 1u << size;
        unsigned lanes = scalar ? 1 : (Q ? 16u : 8u) / eb;
        if (scalar) {
            if (size != 3 && !(opcode >= 0x08 && opcode <= 0x0b)) return A64_UNDEF;
            lanes = 1;
        } else if (size == 3 && !Q) return A64_UNDEF;

        vreg t = (vreg){{0}};

        if (opcode == 0x00) {                           /* REV64 (U=0) / REV32 (U=1): reverse bytes in a group */
            unsigned group = U ? 4u : 8u;
            if (size >= (U ? 2u : 3u)) return A64_UNDEF;
            unsigned nb = Q ? 16u : 8u;
            for (unsigned g = 0; g < nb; g += group)
                for (unsigned k = 0; k < group; k += eb)
                    memcpy(t.b + g + k, s->v[rn] + g + group - eb - k, eb);
            vput(s, rd, &t, Q);
            return A64_NEXT;
        }
        if (opcode == 0x01 && !U) {                     /* REV16: reverse bytes within each halfword */
            if (size) return A64_UNDEF;
            unsigned nb = Q ? 16u : 8u;
            for (unsigned k = 0; k < nb; k += 2) { t.b[k] = s->v[rn][k + 1]; t.b[k + 1] = s->v[rn][k]; }
            vput(s, rd, &t, Q);
            return A64_NEXT;
        }
        if (opcode == 0x05) {                           /* CNT (U=0) / NOT, RBIT (U=1) */
            unsigned nb = Q ? 16u : 8u;
            if (!U) {
                if (size) return A64_UNDEF;
                for (unsigned k = 0; k < nb; k++) t.b[k] = (uint8_t)__builtin_popcount(s->v[rn][k]);
            } else if (size == 0) {
                for (unsigned k = 0; k < nb; k++) t.b[k] = (uint8_t)~s->v[rn][k];
            } else if (size == 1) {
                for (unsigned k = 0; k < nb; k++) {
                    uint8_t x = s->v[rn][k], r = 0;
                    for (int bb = 0; bb < 8; bb++) if ((x >> bb) & 1) r |= (uint8_t)(1u << (7 - bb));
                    t.b[k] = r;
                }
            } else return A64_UNDEF;
            vput(s, rd, &t, Q);
            return A64_NEXT;
        }

        /* The long pairwise forms have their own lane count — half as many lanes, each twice as wide — so
              they cannot ride the loop below. Written separately after the compiler caught the first attempt
              storing an uninitialised result for the lanes that do not exist. */
        if (opcode == 0x02 || opcode == 0x06) {
            if (size == 3 || scalar) return A64_UNDEF;
            unsigned olanes = (Q ? 16u : 8u) / (2 * eb);
            for (unsigned l = 0; l < olanes; l++) {
                uint64_t x = lrd(s->v[rn], 2 * l, eb), y = lrd(s->v[rn], 2 * l + 1, eb);
                uint64_t sum = U ? (x + y) : (uint64_t)(sgn(x, eb) + sgn(y, eb));
                if (opcode == 0x06) sum += lrd(s->v[rd], l, 2 * eb);   /* SADALP/UADALP accumulate */
                lwr(t.b, l, 2 * eb, sum & mask_eb(2 * eb));
            }
            vput(s, rd, &t, Q);
            return A64_NEXT;
        }

        for (unsigned l = 0; l < lanes; l++) {
            uint64_t a = lrd(s->v[rn], l, eb), d = lrd(s->v[rd], l, eb);
            int64_t sa = sgn(a, eb);
            uint64_t m = mask_eb(eb), r;
            unsigned w = eb * 8;
            switch (opcode) {
            case 0x03: r = U ? sat_uns(a + (uint64_t)sgn(d, eb), sgn(d, eb) < 0 && a < (uint64_t)(-sgn(d, eb)), eb)
                             : sat_sgn(sgn(d, eb) + (int64_t)a, eb); break;      /* USQADD / SUQADD */
            case 0x04:                                                            /* CLS (U=0) / CLZ (U=1) */
                if (U) { uint64_t v = a & m; r = v == 0 ? w : (uint64_t)(__builtin_clzll(v) - (64 - w)); }
                else { uint64_t v = (a ^ (a >> 1)) & (m >> 1);
                       r = v == 0 ? w - 1 : (uint64_t)(__builtin_clzll(v) - (64 - w) - 1); }
                break;
            case 0x07: r = U ? sat_sgn(-sa, eb) : sat_sgn(sa < 0 ? -sa : sa, eb); break;  /* SQNEG / SQABS */
            case 0x08: r = (U ? sa >= 0 : sa > 0) ? m : 0; break;                /* CMGE 0 / CMGT 0 */
            case 0x09: r = (U ? sa <= 0 : a == 0) ? m : 0; break;                /* CMLE 0 / CMEQ 0 */
            case 0x0a: if (U) return A64_UNDEF; r = sa < 0 ? m : 0; break;       /* CMLT 0 */
            case 0x0b: r = (U ? (uint64_t)(-sa) : (uint64_t)(sa < 0 ? -sa : sa)) & m; break;  /* NEG / ABS */
            default: return A64_UNDEF;
            }
            lwr(t.b, l, eb, r);
        }
        if (scalar) { memset(s->v[rd], 0, 16); memcpy(s->v[rd], t.b, eb); }
        else vput(s, rd, &t, Q);
        return A64_NEXT;
    }

    return A64_UNDEF;
}
