// a64_dpimm.c — Data processing (immediate): Arm ARM C4.1.86, selected by bits 25:23.
#include "a64_priv.h"

/* The decoded forms of this group (see a64_priv.h): ADR/ADRP, ADD/SUB (immediate), logical (immediate),
      move wide. This group is a switch on bits 25:23 with no earlier tests, so the spec's routing is that switch. */
int a64_dp_imm_spec(uint32_t i, struct a64_dec *d) {
    memset(d, 0, sizeof *d);
    d->sf = (uint8_t)BIT(i, 31); d->rd = (uint8_t)B(i, 4, 0); d->rn = (uint8_t)B(i, 9, 5);
    switch (B(i, 25, 23)) {
    case 0: case 1:     /* PC-rel. addressing: ADR, ADRP */
        d->imm = sx(((uint64_t)B(i, 23, 5) << 2) | B(i, 30, 29), 21);
        d->op = (uint8_t)BIT(i, 31);
        return A64X_ADR;
    case 2:             /* Add/subtract (immediate) */
        if (BIT(i, 22) && B(i, 23, 22) == 3) return A64X_NONE;      /* sh = 0b1x reserved */
        d->op = (uint8_t)BIT(i, 30); d->S = (uint8_t)BIT(i, 29);
        d->imm = (uint64_t)B(i, 21, 10) << (BIT(i, 22) ? 12 : 0);
        return A64X_ADDSUB_IMM;
    case 4: {           /* Logical (immediate): the mask is decoded here, once, not on every execution */
        uint64_t m;
        if (!a64_bitmask(BIT(i, 22), B(i, 15, 10), B(i, 21, 16), d->sf, &m)) return A64X_NONE;
        d->op = (uint8_t)B(i, 30, 29); d->imm = m;
        return A64X_LOGIC_IMM;
    }
    case 5: {           /* Move wide (immediate): MOVN, MOVZ, MOVK */
        unsigned opc = B(i, 30, 29), hw = B(i, 22, 21);
        if (!d->sf && (hw & 2)) return A64X_NONE;
        if (opc == 1) return A64X_NONE;
        d->op = (uint8_t)opc; d->ra = (uint8_t)hw;
        d->imm = (uint64_t)B(i, 20, 5) << (16 * hw);
        return A64X_MOVW;
    }
    default: return A64X_NONE;
    }
}

int a64x_adr(a64 *s, const struct a64_dec *d) {
    /* ADRP's base is the pc with its low 12 bits cleared, and the immediate is pages. */
    wz(s, d->rd, d->op ? ((s->pc & ~0xfffULL) + (d->imm << 12)) : (s->pc + d->imm), 1);
    return A64_NEXT;
}

int a64x_addsub_imm(a64 *s, const struct a64_dec *d) {
    int sf = d->sf, S = d->S;
    uint64_t a = rsp(s, d->rn), r;
    /* SUB is AddWithCarry(a, not imm, 1) at the operation's width — that is what makes borrow land in C. */
    if (d->op) r = a64_awc(s, a, trw(~d->imm, sf), 1, sf, S);
    else       r = a64_awc(s, a, trw(d->imm, sf), 0, sf, S);
    if (S) wz(s, d->rd, r, sf);         /* ADDS/SUBS: Rd is ZR when 31 */
    else   wsp(s, d->rd, r, sf);        /* ADD/SUB:   Rd is SP when 31 */
    return A64_NEXT;
}

int a64x_logic_imm(a64 *s, const struct a64_dec *d) {
    int sf = d->sf;
    uint64_t a = rz(s, d->rn), m = d->imm, r;
    switch (d->op) {
    case 0: r = a & m; break;                                       /* AND */
    case 1: r = a | m; break;                                       /* ORR */
    case 2: r = a ^ m; break;                                       /* EOR */
    default: r = a & m;                                             /* ANDS */
        r = trw(r, sf);
        setf(s, (int)(r >> (sf ? 63 : 31)), r == 0, 0, 0);          /* C and V are cleared */
        wz(s, d->rd, r, sf);
        return A64_NEXT;
    }
    /* AND/ORR/EOR (immediate) write SP when Rd IS 31; only ands makes it ZR. */
    wsp(s, d->rd, r, sf);
    return A64_NEXT;
}

int a64x_movw(a64 *s, const struct a64_dec *d) {
    int sf = d->sf;
    switch (d->op) {
    case 0: wz(s, d->rd, trw(~d->imm, sf), sf); return A64_NEXT;    /* MOVN */
    case 2: wz(s, d->rd, d->imm, sf); return A64_NEXT;              /* MOVZ */
    default: {                                                      /* MOVK: keep the other halfwords */
        uint64_t keep = ~(0xffffULL << (16 * d->ra));
        wz(s, d->rd, (rz(s, d->rd) & keep) | d->imm, sf);
        return A64_NEXT;
    }
    }
}

int a64_dp_imm(a64 *s, uint32_t i) {
    int sf = BIT(i, 31), rd = B(i, 4, 0), rn = B(i, 9, 5);
    { struct a64_dec d; int id = a64_dp_imm_spec(i, &d); if (id) return a64x_run(id, s, &d); }

    switch (B(i, 25, 23)) {

    case 0: case 1: case 2: case 4: case 5:
        return A64_UNDEF;   /* every valid encoding of these is a decoded form, claimed above */

    case 3:             /* Add/subtract (immediate, with tags): ADDG/SUBG, MTE — not implemented */
        return A64_UNDEF;

    case 6: {           /* Bitfield: SBFM, BFM, UBFM */
        unsigned opc = B(i, 30, 29), N = BIT(i, 22), immr = B(i, 21, 16), imms = B(i, 15, 10);
        unsigned d = sf ? 64 : 32;
        if (N != (unsigned)sf) return A64_UNDEF;
        if (immr >= d || imms >= d) return A64_UNDEF;
        uint64_t src = trw(rz(s, rn), sf);
        /* Written as the two real cases rather than wmask/tmask: imms >= immr is a plain field extract,
              imms < immr is a rotate-and-place. Both come straight from the Arm ARM's own description and
              each is one expression, where the mask form is four and hides which branch is wrong. */
        uint64_t bits, nbits;
        if (imms >= immr) { nbits = imms - immr + 1; bits = (src >> immr) & (nbits == 64 ? ~0ULL : ((1ULL << nbits) - 1)); }
        else              { nbits = imms + 1;       bits = src & (nbits == 64 ? ~0ULL : ((1ULL << nbits) - 1)); }
        unsigned pos = imms >= immr ? 0 : (d - immr);
        switch (opc) {
        case 0: {       /* SBFM: sign-extend from the top of the field, after placing it */
            uint64_t r = trw(bits << pos, sf);
            unsigned top = pos + (unsigned)nbits;
            if (top < d) r = trw(sx(r, (int)top), sf);
            wz(s, rd, r, sf);
            return A64_NEXT;
        }
        case 1: {       /* BFM: insert, leaving the rest of Rd */
            uint64_t fmask = (nbits == 64 ? ~0ULL : ((1ULL << nbits) - 1)) << pos;
            wz(s, rd, (trw(rz(s, rd), sf) & ~fmask) | ((bits << pos) & fmask), sf);
            return A64_NEXT;
        }
        case 2: wz(s, rd, bits << pos, sf); return A64_NEXT;        /* UBFM */
        default: return A64_UNDEF;
        }
    }

    default: {          /* Extract: EXTR (and its ROR alias) */
        unsigned N = BIT(i, 22), o0 = BIT(i, 21), imms = B(i, 15, 10), rm = B(i, 20, 16);
        unsigned d = sf ? 64 : 32;
        if (B(i, 30, 29) || o0 || N != (unsigned)sf || imms >= d) return A64_UNDEF;
        uint64_t hi = trw(rz(s, rn), sf), lo = trw(rz(s, rm), sf), r;
        if (!imms) r = lo;
        else r = trw((lo >> imms) | (hi << (d - imms)), sf);
        wz(s, rd, r, sf);
        return A64_NEXT;
    }
    }
}
