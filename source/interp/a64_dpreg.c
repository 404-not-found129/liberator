// a64_dpreg.c — Data processing (register): Arm ARM C4.1.87. Decoded on exact field patterns rather than a
// nested op0/op1/op2 walk, so each `if` can be read straight off the instruction's own encoding diagram.
#include "a64_priv.h"

/* ARM's CRC32 is the reflected CRC with poly 0xEDB88320 (0x82F63B78 for the C forms) — the standard
   byte-at-a-time update is bit-for-bit the architecture's BitReverse/Poly32Mod2 definition. */
static uint32_t crc32_step(uint32_t acc, uint64_t val, unsigned bytes, uint32_t poly) {
    for (unsigned k = 0; k < bytes; k++) {
        acc ^= (uint32_t)((val >> (8 * k)) & 0xff);
        for (int b = 0; b < 8; b++) acc = (acc >> 1) ^ (poly & (uint32_t)(-(int32_t)(acc & 1)));
    }
    return acc;
}

/* The decoded forms of this group (see a64_priv.h): logical (shifted register), add/subtract (shifted and
      extended register), conditional select, MADD/MSUB. Why claiming them first cannot change the routing: they are
      the first two tests of the group (bits 28:24 = 0x0a / 0x0b), and the other two (28:21 = 0xd4, 28:24 = 0x1b) have
      bits 28:24 = 0x1a / 0x1b, which no test before them in the group matches (0x0a, 0x0b, 28:21 = 0xd0, 0xd2). */
int a64_dp_reg_spec(uint32_t i, struct a64_dec *d) {
    memset(d, 0, sizeof *d);
    int sf = BIT(i, 31);
    d->sf = (uint8_t)sf; d->rd = (uint8_t)B(i, 4, 0); d->rn = (uint8_t)B(i, 9, 5); d->rm = (uint8_t)B(i, 20, 16);
    if (B(i, 28, 24) == 0x0a) {                             /* Logical (shifted register) */
        unsigned amt = B(i, 15, 10);
        if (!sf && amt >= 32) return A64X_NONE;
        d->op = (uint8_t)B(i, 30, 29); d->S = (uint8_t)B(i, 23, 22); d->f = (uint8_t)BIT(i, 21); d->ra = (uint8_t)amt;
        return A64X_LOGIC_REG;
    }
    if (B(i, 28, 24) == 0x0b) {                             /* Add/subtract (shifted / extended register) */
        d->op = (uint8_t)BIT(i, 30); d->S = (uint8_t)BIT(i, 29); d->f = (uint8_t)BIT(i, 21);
        if (!BIT(i, 21)) {
            unsigned sh = B(i, 23, 22), amt = B(i, 15, 10);
            if (sh == 3 || (!sf && amt >= 32)) return A64X_NONE;
            d->ra = (uint8_t)sh; d->imm = amt;
        } else {
            unsigned option = B(i, 15, 13), imm3 = B(i, 12, 10);
            if (B(i, 23, 22) || imm3 > 4) return A64X_NONE;
            d->ra = (uint8_t)option; d->imm = imm3;
        }
        return A64X_ADDSUB_REG;
    }
    if (B(i, 28, 21) == 0xd4 && !BIT(i, 29)) {              /* Conditional select */
        unsigned op2 = B(i, 11, 10);
        if (op2 > 1) return A64X_NONE;
        d->ra = (uint8_t)B(i, 15, 12); d->op = (uint8_t)BIT(i, 30); d->S = (uint8_t)op2;
        return A64X_CSEL;
    }
    if (B(i, 28, 24) == 0x1b && !B(i, 30, 29) && B(i, 23, 21) == 0) {   /* MADD / MSUB */
        d->ra = (uint8_t)B(i, 14, 10); d->op = (uint8_t)BIT(i, 15);
        return A64X_MADD;
    }
    return A64X_NONE;
}

int a64x_logic_reg(a64 *s, const struct a64_dec *d) {
    int sf = d->sf; unsigned dd = sf ? 64 : 32;
    uint64_t b = a64_shift(rz(s, d->rm), d->S, d->ra, sf);
    if (d->f) b = trw(~b, sf);
    uint64_t a = rz(s, d->rn), r;
    switch (d->op) {
    case 0: r = a & b; break;
    case 1: r = a | b; break;
    case 2: r = a ^ b; break;
    default:
        r = trw(a & b, sf);
        setf(s, (int)(r >> (dd - 1)), r == 0, 0, 0);
        wz(s, d->rd, r, sf);
        return A64_NEXT;
    }
    wz(s, d->rd, r, sf);
    return A64_NEXT;
}

int a64x_addsub_reg(a64 *s, const struct a64_dec *d) {
    int sf = d->sf, S = d->S;
    uint64_t a, b, r;
    if (!d->f) {                                            /* shifted */
        a = rz(s, d->rn);
        b = a64_shift(rz(s, d->rm), d->ra, (unsigned)d->imm, sf);
    } else {                                                /* extended */
        /* Rn is SP-capable here and Rm is not: `add x0, sp, x1` is the whole point of the extended form. */
        a = rsp(s, d->rn);
        b = trw(a64_extend(rz(s, d->rm), d->ra, (unsigned)d->imm), sf);
    }
    if (d->op) r = a64_awc(s, a, trw(~b, sf), 1, sf, S);
    else       r = a64_awc(s, a, b, 0, sf, S);
    if (S) wz(s, d->rd, r, sf);
    else   wsp(s, d->rd, r, sf);                            /* ADD/SUB write SP; ADDS/SUBS write ZR */
    return A64_NEXT;
}

int a64x_csel(a64 *s, const struct a64_dec *d) {
    int sf = d->sf;
    uint64_t r;
    if (a64_cond(s, d->ra)) r = rz(s, d->rn);
    else {
        uint64_t m = rz(s, d->rm);
        if (d->op) m = d->S ? trw(~m + 1, sf) : trw(~m, sf);        /* CSNEG : CSINV */
        else if (d->S) m = m + 1;                                   /* CSINC */
        r = m;
    }
    wz(s, d->rd, r, sf);
    return A64_NEXT;
}

int a64x_madd(a64 *s, const struct a64_dec *d) {
    int sf = d->sf;
    uint64_t n = rz(s, d->rn), m = rz(s, d->rm), acc = rz(s, d->ra);
    uint64_t p = trw(trw(n, sf) * trw(m, sf), sf);          /* MADD / MSUB, at the operation's width */
    wz(s, d->rd, d->op ? acc - p : acc + p, sf);
    return A64_NEXT;
}

int a64_dp_reg(a64 *s, uint32_t i) {
    int sf = BIT(i, 31);
    unsigned rd = B(i, 4, 0), rn = B(i, 9, 5), rm = B(i, 20, 16);
    unsigned d = sf ? 64 : 32;

    { struct a64_dec dd_; int id = a64_dp_reg_spec(i, &dd_); if (id) return a64x_run(id, s, &dd_); }
    /* the decoded forms' invalid encodings: what the old inline code returned for them */
    if (B(i, 28, 24) == 0x0a || B(i, 28, 24) == 0x0b) return A64_UNDEF;

    /* Add/subtract (with carry): ADC, ADCS, SBC, SBCS */
    if (B(i, 28, 21) == 0xd0 && B(i, 15, 10) == 0) {
        int op = BIT(i, 30), S = BIT(i, 29);
        uint64_t a = rz(s, rn), b = rz(s, rm);
        uint64_t r = op ? a64_awc(s, a, trw(~b, sf), (unsigned)fC(s), sf, S)
                        : a64_awc(s, a, b, (unsigned)fC(s), sf, S);
        wz(s, rd, r, sf);
        return A64_NEXT;
    }

    /* Conditional compare (register and immediate): CCMN, CCMP */
    if (B(i, 28, 21) == 0xd2 && !BIT(i, 10) && BIT(i, 29)) {
        unsigned cond = B(i, 15, 12);
        uint64_t a = rz(s, rn);
        uint64_t b = BIT(i, 11) ? (uint64_t)B(i, 20, 16) : rz(s, rm);
        if (a64_cond(s, cond)) {
            if (BIT(i, 30)) a64_awc(s, a, trw(~b, sf), 1, sf, 1);   /* CCMP */
            else            a64_awc(s, a, b, 0, sf, 1);             /* CCMN */
        } else {
            /* the instruction's own nzcv field becomes the flags */
            unsigned nz = B(i, 3, 0);
            setf(s, (int)BIT(nz, 3), (int)BIT(nz, 2), (int)BIT(nz, 1), (int)BIT(nz, 0));
        }
        return A64_NEXT;
    }

    /* Conditional select: decoded form (a64x_csel); op2 > 1 is the only encoding left here */
    if (B(i, 28, 21) == 0xd4 && !BIT(i, 29)) return A64_UNDEF;

    /* Data-processing (3 source): MADD, MSUB, SMADDL, SMSUBL, SMULH, UMADDL, UMSUBL, UMULH */
    if (B(i, 28, 24) == 0x1b) {
        unsigned op31 = B(i, 23, 21), o0 = BIT(i, 15), ra = B(i, 14, 10);
        if (B(i, 30, 29)) return A64_UNDEF;
        uint64_t n = rz(s, rn), m = rz(s, rm), acc = rz(s, ra);
        switch (op31) {
        /* case 0, MADD / MSUB: a decoded form (a64x_madd), claimed by the spec above */
        case 1: {                                           /* SMADDL / SMSUBL: 32x32 signed -> 64 */
            if (!sf) return A64_UNDEF;
            int64_t p = (int64_t)(int32_t)(uint32_t)n * (int64_t)(int32_t)(uint32_t)m;
            wz(s, rd, o0 ? acc - (uint64_t)p : acc + (uint64_t)p, 1);
            return A64_NEXT;
        }
        case 2: {                                           /* SMULH */
            if (!sf || o0) return A64_UNDEF;
            __int128 p = (__int128)(int64_t)n * (__int128)(int64_t)m;
            wz(s, rd, (uint64_t)(p >> 64), 1);
            return A64_NEXT;
        }
        case 5: {                                           /* UMADDL / UMSUBL */
            if (!sf) return A64_UNDEF;
            uint64_t p = (uint64_t)(uint32_t)n * (uint64_t)(uint32_t)m;
            wz(s, rd, o0 ? acc - p : acc + p, 1);
            return A64_NEXT;
        }
        case 6: {                                           /* UMULH */
            if (!sf || o0) return A64_UNDEF;
            unsigned __int128 p = (unsigned __int128)n * m;
            wz(s, rd, (uint64_t)(p >> 64), 1);
            return A64_NEXT;
        }
        default: return A64_UNDEF;
        }
    }

    /* Data-processing (2 source) */
    if (B(i, 30, 21) == 0x0d6) {
        unsigned opcode = B(i, 15, 10);
        if (BIT(i, 29)) return A64_UNDEF;
        uint64_t n = trw(rz(s, rn), sf), m = trw(rz(s, rm), sf);
        switch (opcode) {
        case 0x02:                                          /* UDIV: divide by zero gives 0, no trap */
            wz(s, rd, m == 0 ? 0 : (sf ? n / m : (uint64_t)((uint32_t)n / (uint32_t)m)), sf);
            return A64_NEXT;
        case 0x03: {                                        /* SDIV: INT_MIN / -1 saturates to INT_MIN */
            if (m == 0) { wz(s, rd, 0, sf); return A64_NEXT; }
            if (sf) {
                int64_t a = (int64_t)n, b = (int64_t)m;
                wz(s, rd, (a == INT64_MIN && b == -1) ? (uint64_t)INT64_MIN : (uint64_t)(a / b), 1);
            } else {
                int32_t a = (int32_t)(uint32_t)n, b = (int32_t)(uint32_t)m;
                wz(s, rd, (uint64_t)(uint32_t)((a == INT32_MIN && b == -1) ? INT32_MIN : a / b), 0);
            }
            return A64_NEXT;
        }
        /* The variable shifts take the amount modulo the width (a64_shift masks), unlike the immediate
              forms where an out-of-range amount is undefined. `lsl x0, x1, x2` with x2 = 64 shifts by 0. */
        case 0x08: wz(s, rd, a64_shift(n, 0, (unsigned)(m & (d - 1)), sf), sf); return A64_NEXT;   /* LSLV */
        case 0x09: wz(s, rd, a64_shift(n, 1, (unsigned)(m & (d - 1)), sf), sf); return A64_NEXT;   /* LSRV */
        case 0x0a: wz(s, rd, a64_shift(n, 2, (unsigned)(m & (d - 1)), sf), sf); return A64_NEXT;   /* ASRV */
        case 0x0b: wz(s, rd, a64_shift(n, 3, (unsigned)(m & (d - 1)), sf), sf); return A64_NEXT;   /* RORV */
        case 0x10: case 0x11: case 0x12: case 0x13:         /* CRC32B/H/W/X */
        case 0x14: case 0x15: case 0x16: case 0x17: {       /* CRC32CB/CH/CW/CX */
            unsigned sz = opcode & 3;                       /* 0=B 1=H 2=W 3=X */
            if ((sz == 3) != (unsigned)sf) return A64_UNDEF;
            uint32_t poly = (opcode & 4) ? 0x82f63b78u : 0xedb88320u;
            wz(s, rd, crc32_step((uint32_t)rz(s, rn), m, 1u << sz, poly), 0);
            return A64_NEXT;
        }
        /* FEAT_CSSC max/min — one instruction each here, where the lifter needs a compare and a select */
        case 0x18: wz(s, rd, sf ? ((int64_t)n > (int64_t)m ? n : m)
                                : (uint64_t)(uint32_t)((int32_t)(uint32_t)n > (int32_t)(uint32_t)m ? n : m), sf); return A64_NEXT;
        case 0x19: wz(s, rd, n > m ? n : m, sf); return A64_NEXT;
        case 0x1a: wz(s, rd, sf ? ((int64_t)n < (int64_t)m ? n : m)
                                : (uint64_t)(uint32_t)((int32_t)(uint32_t)n < (int32_t)(uint32_t)m ? n : m), sf); return A64_NEXT;
        case 0x1b: wz(s, rd, n < m ? n : m, sf); return A64_NEXT;
        default: return A64_UNDEF;                          /* SUBP/IRG/GMI (MTE), PACGA */
        }
    }

    /* Data-processing (1 source) */
    if (B(i, 30, 21) == 0x2d6) {
        unsigned opcode2 = B(i, 20, 16), opcode = B(i, 15, 10);
        if (BIT(i, 29)) return A64_UNDEF;
        if (opcode2 == 1) {
            /* The whole PAC family is identity on the value (see a64_brsys.c): Rd already holds it, so
                  these are architecturally observable only through a signature an x86 host does not have. */
            if (opcode <= 0x11) return A64_NEXT;
            return A64_UNDEF;
        }
        if (opcode2) return A64_UNDEF;
        uint64_t n = trw(rz(s, rn), sf);
        switch (opcode) {
        case 0x00: {                                        /* RBIT */
            uint64_t r = 0;
            for (unsigned k = 0; k < d; k++) if ((n >> k) & 1) r |= 1ULL << (d - 1 - k);
            wz(s, rd, r, sf);
            return A64_NEXT;
        }
        case 0x01: {                                        /* REV16: byte-swap within each halfword */
            uint64_t r = 0;
            for (unsigned k = 0; k < d; k += 16) {
                uint64_t h = (n >> k) & 0xffff;
                r |= (((h >> 8) & 0xff) | ((h & 0xff) << 8)) << k;
            }
            wz(s, rd, r, sf);
            return A64_NEXT;
        }
        case 0x02:                                          /* REV32 (sf=1) / REV (sf=0) */
            if (sf) {
                uint64_t r = 0;
                for (unsigned k = 0; k < 64; k += 32) r |= (uint64_t)__builtin_bswap32((uint32_t)(n >> k)) << k;
                wz(s, rd, r, 1);
            } else wz(s, rd, __builtin_bswap32((uint32_t)n), 0);
            return A64_NEXT;
        case 0x03:                                          /* REV (sf=1 only) */
            if (!sf) return A64_UNDEF;
            wz(s, rd, __builtin_bswap64(n), 1);
            return A64_NEXT;
        case 0x04:                                          /* CLZ */
            wz(s, rd, n == 0 ? d : (uint64_t)(sf ? __builtin_clzll(n) : __builtin_clz((uint32_t)n)), sf);
            return A64_NEXT;
        case 0x05: {                                        /* CLS: leading sign bits, excluding the sign itself */
            uint64_t v = n ^ (n >> 1);                      /* a 1 marks where the run of equal bits breaks */
            v = trw(v, sf) & (d == 64 ? ~(1ULL << 63) : 0x7fffffffULL);
            wz(s, rd, v == 0 ? d - 1 : (uint64_t)((sf ? __builtin_clzll(v) : __builtin_clz((uint32_t)v)) - 1), sf);
            return A64_NEXT;
        }
        case 0x06:                                          /* CTZ (FEAT_CSSC) */
            wz(s, rd, n == 0 ? d : (uint64_t)(sf ? __builtin_ctzll(n) : __builtin_ctz((uint32_t)n)), sf);
            return A64_NEXT;
        case 0x07:                                          /* CNT (FEAT_CSSC) */
            wz(s, rd, (uint64_t)(sf ? __builtin_popcountll(n) : __builtin_popcount((uint32_t)n)), sf);
            return A64_NEXT;
        case 0x08:                                          /* ABS (FEAT_CSSC): INT_MIN stays INT_MIN */
            wz(s, rd, sf ? ((int64_t)n < 0 ? (uint64_t)(-(int64_t)n) : n)
                         : (uint64_t)(uint32_t)((int32_t)(uint32_t)n < 0 ? -(int32_t)(uint32_t)n : (int32_t)(uint32_t)n), sf);
            return A64_NEXT;
        default: return A64_UNDEF;
        }
    }

    return A64_UNDEF;
}
