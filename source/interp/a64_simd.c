// a64_simd.c — the Advanced SIMD dispatcher, plus the groups that move or build lanes rather than compute on
// them: copy (DUP/INS/SMOV/UMOV), modified immediate, permute, EXT and table lookup. Arm ARM C4.1.89.
#include "a64_lane.h"

/* AdvSIMDExpandImm (aarch64/instrs/vector/logical/bitmasks): cmode and op -> the 64-bit pattern to replicate */
static int expand_imm(unsigned cmode, unsigned op, uint8_t imm8, uint64_t *out) {
    uint64_t x;
    unsigned hi3 = cmode >> 1, lo = cmode & 1;
    switch (hi3) {
    case 0: x = ((uint64_t)imm8 << 32) | imm8; break;
    case 1: x = ((uint64_t)imm8 << 40) | ((uint64_t)imm8 << 8); break;
    case 2: x = ((uint64_t)imm8 << 48) | ((uint64_t)imm8 << 16); break;
    case 3: x = ((uint64_t)imm8 << 56) | ((uint64_t)imm8 << 24); break;
    case 4: x = ((uint64_t)imm8 << 48) | ((uint64_t)imm8 << 32) | ((uint64_t)imm8 << 16) | imm8; break;
    case 5: x = ((uint64_t)imm8 << 56) | ((uint64_t)imm8 << 40) | ((uint64_t)imm8 << 24) | ((uint64_t)imm8 << 8); break;
    case 6: {
        /* the "shifting ones" forms (MSL): one 32-bit lane, replicated. Written out as a lane and a
              replicate because the hand-expanded 64-bit constant had 0xffff at the wrong shift and
              movi/mvni #imm, msl #16 came back 007f00ff instead of 007fffff. */
        uint64_t lane = lo ? (((uint64_t)imm8 << 16) | 0xffff) : (((uint64_t)imm8 << 8) | 0xff);
        x = (lane << 32) | lane;
        break;
    }
    default:
        if (!lo && !op) {                               /* MOVI, 8-bit lanes */
            x = 0x0101010101010101ULL * imm8;
        } else if (!lo && op) {                         /* MOVI 64-bit: each imm8 bit becomes a whole byte */
            x = 0;
            for (int k = 0; k < 8; k++) if ((imm8 >> k) & 1) x |= 0xffULL << (8 * k);
        } else if (!op) {                               /* FMOV single: VFPExpandImm into 32-bit lanes */
            uint32_t f = ((uint32_t)(imm8 >> 7) << 31)
                       | ((uint32_t)(((imm8 >> 6) & 1) ? 0 : 1) << 30)
                       | ((uint32_t)(((imm8 >> 6) & 1) ? 0x1f : 0) << 25)
                       | ((uint32_t)(imm8 & 0x3f) << 19);
            x = ((uint64_t)f << 32) | f;
        } else {                                        /* FMOV double */
            x = ((uint64_t)(imm8 >> 7) << 63)
              | ((uint64_t)(((imm8 >> 6) & 1) ? 0 : 1) << 62)
              | ((uint64_t)(((imm8 >> 6) & 1) ? 0xffULL : 0) << 54)
              | ((uint64_t)(imm8 & 0x3f) << 48);
        }
        break;
    }
    *out = x;
    return 1;
}

int a64_simd(a64 *s, uint32_t i) {
    int scalar = BIT(i, 28);
    /* Every Advanced SIMD encoding has bit 31 clear, and the SHA3/SHA512 crypto groups (EOR3, BCAX, RAX1,
          SHA512H...) share bits 29:24 = 001110 with the permute and table-lookup forms. Without this guard
          `eor3.16b` decodes as a permute and writes a plausible wrong answer. A missing instruction must
          report, never improvise. */
    if (BIT(i, 31)) return A64_UNDEF;
    unsigned rd = B(i, 4, 0), rn = B(i, 9, 5), rm = B(i, 20, 16);
    int Q = (int)BIT(i, 30);

    /* ───────── copy: DUP (element/general), SMOV, UMOV, INS (element/general) ───────── */
    if (B(i, 28, 21) == (scalar ? 0xf0u : 0x70u) && !BIT(i, 15) && BIT(i, 10)) {
        unsigned imm5 = B(i, 20, 16), imm4 = B(i, 14, 11), op = BIT(i, 29);
        if (!(imm5 & 0x1f)) return A64_UNDEF;
        unsigned size = (unsigned)__builtin_ctz(imm5 & 0x1f);   /* the lowest set bit names the element size */
        unsigned eb = 1u << size;
        unsigned index = imm5 >> (size + 1);

        if (op) {                                       /* INS (element): Vd[index] = Vn[index2] */
            if (!Q) return A64_UNDEF;
            unsigned index2 = imm4 >> size;
            uint64_t v = lrd(s->v[rn], index2, eb);
            lwr(s->v[rd], index, eb, v);                /* in place: every other lane of Vd is preserved */
            return A64_NEXT;
        }
        switch (imm4) {
        case 0x0: {                                     /* DUP (element) */
            uint64_t v = lrd(s->v[rn], index, eb);
            if (scalar) { memset(s->v[rd], 0, 16); memcpy(s->v[rd], &v, eb); return A64_NEXT; }
            vreg t = (vreg){{0}};
            unsigned lanes = (Q ? 16u : 8u) / eb;
            if (eb == 8 && !Q) return A64_UNDEF;
            for (unsigned l = 0; l < lanes; l++) lwr(t.b, l, eb, v);
            vput(s, rd, &t, Q);
            return A64_NEXT;
        }
        case 0x1: {                                     /* DUP (general): every lane from a GPR */
            if (scalar) return A64_UNDEF;
            if (eb == 8 && !Q) return A64_UNDEF;
            uint64_t v = rz(s, rn) & mask_eb(eb);
            vreg t = (vreg){{0}};
            unsigned lanes = (Q ? 16u : 8u) / eb;
            for (unsigned l = 0; l < lanes; l++) lwr(t.b, l, eb, v);
            vput(s, rd, &t, Q);
            return A64_NEXT;
        }
        case 0x3:                                       /* INS (general): Vd[index] = Rn */
            if (scalar || !Q) return A64_UNDEF;
            lwr(s->v[rd], index, eb, rz(s, rn) & mask_eb(eb));
            return A64_NEXT;
        case 0x5:                                       /* SMOV: sign-extend a lane into a GPR */
            if (scalar) return A64_UNDEF;
            if (eb == 8 || (!Q && eb == 4)) return A64_UNDEF;
            wz(s, rd, (uint64_t)sgn(lrd(s->v[rn], index, eb), eb), Q);
            return A64_NEXT;
        case 0x7:                                       /* UMOV: a lane into a GPR, zero-extended */
            if (scalar) return A64_UNDEF;
            if ((eb == 8) != (unsigned)Q) return A64_UNDEF;     /* the 64-bit form needs Q, and only it */
            wz(s, rd, lrd(s->v[rn], index, eb), Q);
            return A64_NEXT;
        default: return A64_UNDEF;
        }
    }

    /* ───────── modified immediate: MOVI, MVNI, ORR #imm, BIC #imm, FMOV #imm ───────── */
    /* 0111100000 IS 0x1e0, not 0x3c0 — bits 28:19 is a ten-bit field and the constant was written as
      eleven. Every movi/mvni/orr/bic immediate reported itself unimplemented although it was written
      right here; the certifier's unimplemented list is what showed it. */
    if (!scalar && B(i, 28, 19) == 0x1e0 && BIT(i, 10)) {
        unsigned cmode = B(i, 15, 12), op = BIT(i, 29);
        uint8_t imm8 = (uint8_t)((B(i, 18, 16) << 5) | B(i, 9, 5));
        uint64_t x;
        if (!expand_imm(cmode, op, imm8, &x)) return A64_UNDEF;
        unsigned hi3 = cmode >> 1, lo = cmode & 1;
        int logical = (hi3 <= 5) && lo;                 /* ORR/BIC immediate rather than MOVI/MVNI */
        if (hi3 == 7 && lo && op && !Q) return A64_UNDEF;   /* FMOV double needs the 128-bit form */
        vreg t;
        uint64_t lo64, hi64;
        memcpy(&lo64, s->v[rd], 8);
        memcpy(&hi64, s->v[rd] + 8, 8);
        if (logical) {
            if (op) { lo64 &= ~x; hi64 &= ~x; }         /* BIC */
            else    { lo64 |= x;  hi64 |= x;  }         /* ORR */
        } else {
            int invert = op && !(hi3 == 7);             /* MVNI; the cmode=111x forms are never inverted */
            lo64 = hi64 = invert ? ~x : x;
        }
        memcpy(t.b, &lo64, 8);
        memcpy(t.b + 8, &hi64, 8);
        vput(s, rd, &t, Q);
        return A64_NEXT;
    }

    /* ───────── permute: UZP1/2, TRN1/2, ZIP1/2 ───────── */
    if (!scalar && B(i, 29, 24) == 0x0e && !BIT(i, 21) && !BIT(i, 15) && B(i, 11, 10) == 2) {
        unsigned opcode = B(i, 14, 12), size = B(i, 23, 22);
        unsigned eb = 1u << size;
        if (eb == 8 && !Q) return A64_UNDEF;
        unsigned lanes = (Q ? 16u : 8u) / eb;
        vreg t = (vreg){{0}};
        /* Read both sources into the temporary's terms first — every one of these has Vd aliasing a source
              in real code (`zip1 v0.4s, v0.4s, v1.4s` is how a transpose is written). */
        for (unsigned l = 0; l < lanes; l++) {
            uint64_t v;
            switch (opcode) {
            case 1: v = (2 * l < lanes) ? lrd(s->v[rn], 2 * l, eb)                      /* UZP1: even lanes */
                                        : lrd(s->v[rm], 2 * l - lanes, eb); break;
            case 5: v = (2 * l < lanes) ? lrd(s->v[rn], 2 * l + 1, eb)                  /* UZP2: odd lanes  */
                                        : lrd(s->v[rm], 2 * l + 1 - lanes, eb); break;
            case 2: v = (l & 1) ? lrd(s->v[rm], l - 1, eb) : lrd(s->v[rn], l, eb); break;       /* TRN1 */
            case 6: v = (l & 1) ? lrd(s->v[rm], l, eb) : lrd(s->v[rn], l + 1, eb); break;       /* TRN2 */
            case 3: v = (l & 1) ? lrd(s->v[rm], l / 2, eb) : lrd(s->v[rn], l / 2, eb); break;   /* ZIP1 */
            case 7: v = (l & 1) ? lrd(s->v[rm], lanes / 2 + l / 2, eb)                          /* ZIP2 */
                                : lrd(s->v[rn], lanes / 2 + l / 2, eb); break;
            default: return A64_UNDEF;
            }
            lwr(t.b, l, eb, v);
        }
        vput(s, rd, &t, Q);
        return A64_NEXT;
    }

    /* ───────── EXT: a window across the concatenation of Vn and Vm ───────── */
    if (!scalar && B(i, 29, 24) == 0x2e && !BIT(i, 21) && !BIT(i, 15) && !BIT(i, 10)) {
        unsigned pos = B(i, 14, 11);
        if (B(i, 23, 22)) return A64_UNDEF;
        if (!Q && pos >= 8) return A64_UNDEF;
        unsigned nb = Q ? 16u : 8u;
        uint8_t cat[32];
        memcpy(cat, s->v[rn], nb);
        memcpy(cat + nb, s->v[rm], nb);
        vreg t = (vreg){{0}};
        memcpy(t.b, cat + pos, nb);
        vput(s, rd, &t, Q);
        return A64_NEXT;
    }

    /* ───────── table lookup: TBL, TBX (1 to 4 consecutive table registers) ───────── */
    if (!scalar && B(i, 29, 24) == 0x0e && !BIT(i, 21) && !BIT(i, 15) && B(i, 11, 10) == 0) {
        unsigned len = B(i, 14, 13), op = BIT(i, 12);
        if (B(i, 23, 22)) return A64_UNDEF;
        unsigned nb = Q ? 16u : 8u, tbytes = 16u * (len + 1);
        uint8_t tab[64];
        /* The table is Vn and the index is Vm. Swapped, every tbl form gives wrong results —
              `tbl.16b v23, {v22}, v20` indexes v22 by v20, not the other way round. */
        for (unsigned r = 0; r <= len; r++) memcpy(tab + 16 * r, s->v[(rn + r) & 31], 16);
        vreg t = (vreg){{0}};
        for (unsigned k = 0; k < nb; k++) {
            unsigned idx = s->v[rm][k];
            /* An out-of-range index gives zero for TBL and LEAVES Vd for TBX — that difference is the only
                  thing separating the two instructions, and it is how a vectorised lookup handles misses. */
            if (idx < tbytes) t.b[k] = tab[idx];
            else t.b[k] = op ? s->v[rd][k] : 0;
        }
        vput(s, rd, &t, Q);
        return A64_NEXT;
    }

    int r = a64_simdidx(s, i);
    if (r != A64_UNDEF) return r;
    r = a64_simdsh(s, i);
    if (r != A64_UNDEF) return r;
    r = a64_simd3(s, i);
    if (r != A64_UNDEF) return r;
    return a64_simdfp(s, i);
}
