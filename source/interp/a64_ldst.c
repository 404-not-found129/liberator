// a64_ldst.c — Loads and stores: Arm ARM C4.1.88, except the Advanced SIMD structure forms (a64_ldstruct.c).
#include "a64_priv.h"

/* a load's destination: W-sized loads zero the top half, the signed forms sign-extend to W or X */
static void ldwrite(a64 *s, unsigned rt, uint64_t v, unsigned nbytes, int sign, int to64) {
    if (sign) {
        v = sx(v, (int)nbytes * 8);
        wz(s, rt, v, to64);
    } else {
        wz(s, rt, v, nbytes == 8);
    }
}

/* The decoded forms of this group (see a64_priv.h): load/store pair (bits 29:27 = 101) and load/store register
      in every addressing form (bits 29:27 = 111) except the atomics. Why claiming them first cannot change the
      routing: the tests that come before them in the group are 29:24 = 001000 (exclusive), 29:27 = 011 with 25:24 = 00
      (literal) and 29:24 = 011001 (RCPC2) — 29:27 = 001 / 011 — disjoint from 101 and 111; and the atomics, which come
      first inside the 111 block, are excluded here by their own test.
   f bits: 1 V (SIMD&FP register) · 2 load · 4 sign-extend · 8 to 64 bits · 16 PRFM · 32 write back. sf = bytes.
   op (register forms): 0 base+imm · 1 post-index · 2 pre-index · 3 register offset (ra = option, S = shift). */
int a64_ldst_spec(uint32_t i, struct a64_dec *d) {
    memset(d, 0, sizeof *d);
    d->rd = (uint8_t)B(i, 4, 0); d->rn = (uint8_t)B(i, 9, 5);
    if (B(i, 29, 27) == 0x05) {                             /* Load/store register pair */
        unsigned opc = B(i, 31, 30), V = BIT(i, 26), form = B(i, 25, 23), L = BIT(i, 22);
        unsigned n, scale; int sign = 0;
        if (V) { if (opc > 2) return A64X_NONE; scale = 2 + opc; n = 1u << scale; }
        else {
            switch (opc) {
            case 0: scale = 2; n = 4; break;                /* STP/LDP 32-bit */
            case 1: scale = 2; n = 4; sign = 1; break;      /* LDPSW (L must be 1) */
            case 2: scale = 3; n = 8; break;
            default: return A64X_NONE;
            }
            if (sign && !L) return A64X_NONE;
        }
        if (form > 3) return A64X_NONE;
        d->ra = (uint8_t)B(i, 14, 10); d->sf = (uint8_t)n; d->op = (uint8_t)form;
        d->f = (uint8_t)(V | (L << 1) | (sign << 2));
        d->imm = (uint64_t)((int64_t)sx(B(i, 21, 15), 7) << scale);
        return A64X_LDST_PAIR;
    }
    if (B(i, 29, 27) == 0x07) {                             /* Load/store register — all the addressing forms */
        unsigned size = B(i, 31, 30), V = BIT(i, 26), opc = B(i, 23, 22);
        if (!V && B(i, 25, 24) == 0 && BIT(i, 21) && B(i, 11, 10) == 0) return A64X_NONE;   /* the atomics */
        unsigned n, scale;
        int is_load, sign = 0, to64 = 1, prfm = 0;
        if (V) {
            scale = ((opc & 2) << 1) | size;
            if (scale > 4) return A64X_NONE;
            n = 1u << scale;
            is_load = (int)(opc & 1);
        } else {
            scale = size; n = 1u << size;
            switch (opc) {
            case 0: is_load = 0; break;
            case 1: is_load = 1; break;
            case 2:
                if (size == 3) { is_load = 1; prfm = 1; }   /* PRFM: a hint, no access */
                else { is_load = 1; sign = 1; to64 = 1; }   /* LDRSB/LDRSH/LDRSW -> X */
                break;
            default:
                if (size >= 2) return A64X_NONE;
                is_load = 1; sign = 1; to64 = 0; break;     /* LDRSB/LDRSH -> W */
            }
        }
        if (B(i, 25, 24) == 1) {                            /* unsigned immediate (scaled) */
            d->op = 0; d->imm = (uint64_t)B(i, 21, 10) << scale;
        } else if (B(i, 25, 24) == 0) {
            if (!BIT(i, 21)) {
                int64_t imm9 = (int64_t)sx(B(i, 20, 12), 9);
                d->imm = (uint64_t)imm9;
                switch (B(i, 11, 10)) {
                case 1: d->op = 1; break;                   /* post-index */
                case 3: d->op = 2; break;                   /* pre-index */
                default: d->op = 0; break;                  /* LDUR/STUR, LDTR/STTR (EL0 = plain) */
                }
            } else if (B(i, 11, 10) == 2) {                 /* register offset */
                unsigned option = B(i, 15, 13), S = BIT(i, 12);
                if ((option & 3) != 2 && (option & 3) != 3) return A64X_NONE;   /* only W/X index forms */
                d->op = 3; d->rm = (uint8_t)B(i, 20, 16); d->ra = (uint8_t)option; d->S = (uint8_t)(S ? scale : 0);
            } else return A64X_NONE;                        /* LDRAA/LDRAB (pointer-authenticated) */
        } else return A64X_NONE;
        d->sf = (uint8_t)n;
        d->f = (uint8_t)(V | (is_load << 1) | (sign << 2) | (to64 << 3) | (prfm << 4));
        return A64X_LDST_REG;
    }
    return A64X_NONE;
}

int a64x_ldst_pair(a64 *s, const struct a64_dec *d) {
    unsigned rt = d->rd, rt2 = d->ra, rn = d->rn, n = d->sf, form = d->op;
    int V = d->f & 1, L = (d->f >> 1) & 1, sign = (d->f >> 2) & 1;
    uint64_t base = rsp(s, rn), off = d->imm, a;
    a = form == 1 ? base : base + off;                      /* post-index uses the base; offset, no-allocate, pre add */
    if (V) {
        if (L) { vzw(s, (int)rt, (const void *)(uintptr_t)a, n);
                 vzw(s, (int)rt2, (const void *)(uintptr_t)(a + n), n); }
        else   { cpyn((void *)(uintptr_t)a, s->v[rt], n);
                 cpyn((void *)(uintptr_t)(a + n), s->v[rt2], n); }
    } else if (L) {
        /* Both loads happen before either write-back. `ldp x0, x1, [x0]` must read through the old x0;
              writing Rt first would change the base for Rt2 when they overlap. */
        uint64_t v0 = mld(a, n), v1 = mld(a + n, n);
        ldwrite(s, rt, v0, n, sign, 1);
        ldwrite(s, rt2, v1, n, sign, 1);
    } else {
        mst(a, trw(rz(s, rt), n == 8), n);
        mst(a + n, trw(rz(s, rt2), n == 8), n);
    }
    if (form == 1 || form == 3) wsp(s, rn, base + off, 1);
    return A64_NEXT;
}

int a64x_ldst_reg(a64 *s, const struct a64_dec *d) {
    unsigned rt = d->rd, rn = d->rn, n = d->sf;
    int V = d->f & 1, is_load = (d->f >> 1) & 1, sign = (d->f >> 2) & 1, to64 = (d->f >> 3) & 1, prfm = (d->f >> 4) & 1;
    uint64_t a, base = rsp(s, rn);
    int wb = 0;
    uint64_t wbval = 0;
    switch (d->op) {
    case 0: a = base + d->imm; break;
    case 1: a = base; wb = 1; wbval = base + d->imm; break;                 /* post-index */
    case 2: a = base + d->imm; wb = 1; wbval = a; break;                    /* pre-index */
    default: a = base + a64_extend(rz(s, d->rm), d->ra, d->S); break;       /* register offset */
    }
    if (prfm) { if (wb) wsp(s, rn, wbval, 1); return A64_NEXT; }
    if (V) {
        if (is_load) vzw(s, (int)rt, (const void *)(uintptr_t)a, n);
        else cpyn((void *)(uintptr_t)a, s->v[rt], n);
    } else if (is_load) {
        ldwrite(s, rt, mld(a, n), n, sign, to64);
    } else {
        mst(a, trw(rz(s, rt), n == 8), n);
    }
    if (wb) wsp(s, rn, wbval, 1);
    return A64_NEXT;
}

int a64_ldst(a64 *s, uint32_t i) {
    unsigned rt = B(i, 4, 0), rn = B(i, 9, 5);
    { struct a64_dec d; int id = a64_ldst_spec(i, &d); if (id) return a64x_run(id, s, &d); }

    /* ───────── Load/store exclusive, load-acquire/store-release, CAS (bits 29:24 = 001000) ───────── */
    if (B(i, 29, 24) == 0x08) {
        unsigned size = B(i, 31, 30), o2 = BIT(i, 23), L = BIT(i, 22), o1 = BIT(i, 21);
        unsigned rs = B(i, 20, 16), rt2 = B(i, 14, 10);
        /* The acquire/release qualifier (o0, and L's A bit) needs no code here: every access below is
              seq_cst, which is stronger than any of the qualified forms asks for. It is read and ignored
              on purpose — the alternative, relaxing to match, would be wrong on an arm64 host. */
        unsigned n = 1u << size;
        uint64_t a = rsp(s, rn);

        if (!o2 && !o1) {                                   /* LDXR/LDAXR, STXR/STLXR */
            if (rs != 0x1f && L) return A64_UNDEF;
            if (L) {
                uint64_t v = mld(a, n);
                s->excl_addr = a; s->excl_val[0] = v; s->excl_size = (int)n; s->excl_pair = 0;
                ldwrite(s, rt, v, n, 0, 0);
                return A64_NEXT;
            }
            /* STXR IS A compare-and-swap against what LDXR read. That is what makes it atomic against the
                  native x86 threads in this same process — a plain store would let another thread's write
                  between the pair vanish. It differs from a real monitor in one way, and only one: a value
                  written and restored (ABA) succeeds here where the hardware would fail. Every use of the pair
                  in libobjc, libdispatch and os_unfair_lock is a CAS loop that is correct under ABA; a
                  sequence that is not would need the host's own reservation, which x86 does not have. */
            int ok = 0;
            if (s->excl_size == (int)n && s->excl_addr == a && !s->excl_pair)
                ok = a64_cas(a, n, s->excl_val[0], trw(rz(s, rt), size == 3)) == s->excl_val[0];
            s->excl_size = 0;
            wz(s, rs, ok ? 0 : 1, 0);
            return A64_NEXT;
        }

        if (!o2 && o1) {
            if (BIT(i, 31)) {                               /* LDXP/LDAXP, STXP/STLXP */
                if (size < 2) return A64_UNDEF;
                if (L) {
                    uint64_t v0 = mld(a, n), v1 = mld(a + n, n);
                    s->excl_addr = a; s->excl_val[0] = v0; s->excl_val[1] = v1;
                    s->excl_size = (int)n; s->excl_pair = 1;
                    ldwrite(s, rt, v0, n, 0, 0);
                    ldwrite(s, rt2, v1, n, 0, 0);
                    return A64_NEXT;
                }
                int ok = 0;
                if (s->excl_size == (int)n && s->excl_addr == a && s->excl_pair) {
                    uint64_t e0 = s->excl_val[0], e1 = s->excl_val[1];
                    ok = a64_casp(a, n, &e0, &e1, trw(rz(s, rt), size == 3), trw(rz(s, rt2), size == 3));
                }
                s->excl_size = 0;
                wz(s, rs, ok ? 0 : 1, 0);
                return A64_NEXT;
            }
            /* CASP, CASPA, CASPL, CASPAL — Rs and Rs+1 hold the expected pair, Rt and Rt+1 the new one */
            if (rt2 != 0x1f || (rs & 1) || (rt & 1)) return A64_UNDEF;
            unsigned pn = BIT(i, 30) ? 8 : 4;
            uint64_t e0 = trw(rz(s, rs), pn == 8), e1 = trw(rz(s, rs + 1), pn == 8);
            a64_casp(a, pn, &e0, &e1, trw(rz(s, rt), pn == 8), trw(rz(s, rt + 1), pn == 8));
            wz(s, rs, e0, pn == 8); wz(s, rs + 1, e1, pn == 8);   /* the old values, success or not */
            return A64_NEXT;
        }

        if (o2 && !o1) {                                    /* LDAR/LDLAR, STLR/STLLR — no reservation */
            if (rs != 0x1f || rt2 != 0x1f) return A64_UNDEF;
            if (L) { ldwrite(s, rt, mld(a, n), n, 0, 0); return A64_NEXT; }
            mst(a, trw(rz(s, rt), size == 3), n);
            return A64_NEXT;
        }

        /* CAS, CASA, CASL, casal, CASB, cash */
        if (rt2 != 0x1f) return A64_UNDEF;
        uint64_t old = a64_cas(a, n, trw(rz(s, rs), size == 3), trw(rz(s, rt), size == 3));
        ldwrite(s, rs, old, n, 0, 0);
        return A64_NEXT;
    }

    /* ───────── Load register (literal) ───────── */
    if (B(i, 29, 27) == 0x03 && B(i, 25, 24) == 0) {
        unsigned opc = B(i, 31, 30), V = BIT(i, 26);
        uint64_t a = s->pc + (sx(B(i, 23, 5), 19) << 2);
        if (V) {
            unsigned n = 4u << opc;                         /* S, D, Q */
            if (opc > 2) return A64_UNDEF;
            vzw(s, (int)rt, (const void *)(uintptr_t)a, n);
            return A64_NEXT;
        }
        switch (opc) {
        case 0: ldwrite(s, rt, mld(a, 4), 4, 0, 0); return A64_NEXT;      /* LDR Wt */
        case 1: ldwrite(s, rt, mld(a, 8), 8, 0, 1); return A64_NEXT;      /* LDR Xt */
        case 2: ldwrite(s, rt, mld(a, 4), 4, 1, 1); return A64_NEXT;      /* LDRSW */
        default: return A64_NEXT;                                         /* PRFM literal: a hint */
        }
    }

    /* Load/store register pair: a decoded form (a64x_ldst_pair); what reaches here is an invalid encoding of it */
    if (B(i, 29, 27) == 0x05) return A64_UNDEF;

    /* ───────── Load-acquire/store-release register, unscaled offset: LDAPUR, LDAPURS{B,H,W}, STLUR (RCPC2, bits 29:24 = 011001) ─────────
       A guest built for armv8.4 (the vendored libc++ is) uses these for atomic acquire loads at a small offset;
       LDAPR (no offset) is handled with the atomics below, this is the same family with an imm9. */
    if (B(i, 29, 24) == 0x19 && !BIT(i, 26) && !BIT(i, 21) && B(i, 11, 10) == 0) {
        unsigned size = B(i, 31, 30), opc = B(i, 23, 22), n = 1u << size;
        uint64_t a = rsp(s, rn) + (uint64_t)(int64_t)sx(B(i, 20, 12), 9);
        switch (opc) {
        case 0: mst(a, trw(rz(s, rt), size == 3), n); break;            /* STLUR */
        case 1: ldwrite(s, rt, mld(a, n), n, 0, 0); break;              /* LDAPUR */
        case 2: if (size == 3) return A64_UNDEF; ldwrite(s, rt, mld(a, n), n, 1, 1); break;   /* LDAPURSB/SH/SW -> X */
        default: if (size >= 2) return A64_UNDEF; ldwrite(s, rt, mld(a, n), n, 1, 0); break;  /* LDAPURSB/SH -> W */
        }
        return A64_NEXT;
    }

    /* ───────── Load/store register — all the addressing forms (bits 29:27 = 111) ───────── */
    if (B(i, 29, 27) == 0x07) {
        unsigned size = B(i, 31, 30), V = BIT(i, 26), opc = B(i, 23, 22);

        /* The atomics come first. bits 23:22 are A and R (acquire/release) for them, not the opc that
              names a width and a direction — decoded in the wrong order, every ...AL form (ldaddal,
              ldsetal, ldclral, swpal) was rejected as a reserved size/opc pair before reaching its case. */
        if (!V && B(i, 25, 24) == 0 && BIT(i, 21) && B(i, 11, 10) == 0) {
            unsigned n = 1u << size, rs = B(i, 20, 16), o3 = BIT(i, 15), aop = B(i, 14, 12);
            uint64_t addr = rsp(s, rn);
            if (!o3) {
                ldwrite(s, rt, a64_rmw(addr, n, aop, trw(rz(s, rs), size == 3)), n, 0, 0);
                return A64_NEXT;
            }
            if (aop == 0) {                                 /* SWP */
                ldwrite(s, rt, a64_swp(addr, n, trw(rz(s, rs), size == 3)), n, 0, 0);
                return A64_NEXT;
            }
            if (aop == 4 && rs == 0x1f && BIT(i, 23) && !BIT(i, 22)) {      /* LDAPR */
                ldwrite(s, rt, mld(addr, n), n, 0, 0);
                return A64_NEXT;
            }
            return A64_UNDEF;                               /* the ST64B family */
        }

        /* every other encoding here is a decoded form (a64x_ldst_reg), claimed above when valid */
        (void)size; (void)V; (void)opc;
        return A64_UNDEF;
    }

    return a64_ldstruct(s, i);
}
