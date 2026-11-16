// a64_ldstruct.c — Advanced SIMD load/store multiple structures and single structure (LD1..LD4, ST1..ST4,
// LD1R..LD4R). Arm ARM C4.1.88, the two "Advanced SIMD load/store" subgroups.
#include "a64_priv.h"

/* Elem[] on a 128-bit register: element `idx` of `eb` bytes */
static inline uint64_t elem_rd(const a64 *s, int t, unsigned idx, unsigned eb) {
    uint64_t v = 0; memcpy(&v, s->v[t] + idx * eb, eb); return v;
}
static inline void elem_wr(a64 *s, int t, unsigned idx, unsigned eb, uint64_t v) {
    memcpy(s->v[t] + idx * eb, &v, eb);
}

int a64_ldstruct(a64 *s, uint32_t i) {
    unsigned rt = B(i, 4, 0), rn = B(i, 9, 5);

    /* ───────── Load/store multiple structures ───────── */
    if (B(i, 29, 24) == 0x0c && !BIT(i, 31) && !BIT(i, 21)) {
        unsigned Q = BIT(i, 30), post = BIT(i, 23), L = BIT(i, 22), rm = B(i, 20, 16);
        unsigned opcode = B(i, 15, 12), size = B(i, 11, 10);
        unsigned rpt, selem;
        switch (opcode) {
        case 0x0: rpt = 1; selem = 4; break;    /* LD4/ST4 */
        case 0x2: rpt = 4; selem = 1; break;    /* LD1/ST1, four registers */
        case 0x4: rpt = 1; selem = 3; break;    /* LD3/ST3 */
        case 0x6: rpt = 3; selem = 1; break;    /* LD1/ST1, three registers */
        case 0x7: rpt = 1; selem = 1; break;    /* LD1/ST1, one register */
        case 0x8: rpt = 1; selem = 2; break;    /* LD2/ST2 */
        case 0xa: rpt = 2; selem = 1; break;    /* LD1/ST1, two registers */
        default: return A64_UNDEF;
        }
        /* The one reserved case: a 3- or 4-element structure of 64-bit elements needs the full 128-bit
              form, so size=11 with Q=0 is undefined (the ARM's own `if size:Q == '110' then UNDEFINED`). */
        if (size == 3 && !Q && selem != 1) return A64_UNDEF;
        if (!post && rm != 0) return A64_UNDEF;

        unsigned eb = 1u << size;                           /* element bytes */
        unsigned elements = (Q ? 16u : 8u) / eb;
        unsigned nregs = rpt * selem;
        uint64_t a = rsp(s, rn), offs = 0;

        /* For the 64-bit forms a load writes the whole register, so the top half becomes zero. Done up front
           because the element loop below writes lanes one at a time. */
        if (L && !Q) for (unsigned k = 0; k < nregs; k++) memset(s->v[(rt + k) & 31] + 8, 0, 8);

        for (unsigned r = 0; r < rpt; r++)
            for (unsigned e = 0; e < elements; e++) {
                unsigned tt = (rt + r) & 31;
                for (unsigned sel = 0; sel < selem; sel++) {
                    if (L) elem_wr(s, (int)tt, e, eb, mld(a + offs, eb));
                    else   mst(a + offs, elem_rd(s, (int)tt, e, eb), eb);
                    offs += eb;
                    tt = (tt + 1) & 31;
                }
            }

        if (post) wsp(s, rn, a + (rm == 0x1f ? offs : rz(s, rm)), 1);
        return A64_NEXT;
    }

    /* ───────── Load/store single structure (including the replicating loads) ───────── */
    if (B(i, 29, 24) == 0x0d && !BIT(i, 31)) {
        unsigned Q = BIT(i, 30), post = BIT(i, 23), L = BIT(i, 22), R = BIT(i, 21);
        unsigned rm = B(i, 20, 16), opcode = B(i, 15, 13), S = BIT(i, 12), size = B(i, 11, 10);
        unsigned scale = opcode >> 1, selem = ((opcode & 1) << 1 | R) + 1;
        unsigned index = 0;
        int replicate = 0;

        switch (scale) {
        case 3:                                             /* LD1R/LD2R/LD3R/LD4R */
            if (!L || S) return A64_UNDEF;
            scale = size; replicate = 1;
            break;
        /* Q:S:size IS A 4-bit field for B AND A 3-bit field for H, not 5 and 4. Shifted one place too far,
              `ld1.b {v1}[15]` wrote byte 27 — into v2 — and every ld1/st1/ld2/ld4 single-lane form differed. */
        case 0: index = (Q << 3) | (S << 2) | size; break;              /* B[0..15] */
        case 1:
            if (size & 1) return A64_UNDEF;
            index = (Q << 2) | (S << 1) | (size >> 1);                  /* H[0..7] */
            break;
        default:
            if (size & 2) return A64_UNDEF;
            if (!(size & 1)) index = (Q << 1) | S;                      /* S[0..3] */
            else { if (S) return A64_UNDEF; index = Q; scale = 3; }     /* D[0..1] */
            break;
        }
        if (!post && rm != 0) return A64_UNDEF;

        unsigned eb = 1u << scale;
        uint64_t a = rsp(s, rn), offs = 0;
        unsigned t = rt;

        for (unsigned sel = 0; sel < selem; sel++) {
            if (replicate) {
                /* LD1R fills every lane from one element — the `dup` of a loaded scalar that every
                      vectorised loop over a constant uses. The 64-bit form leaves the top half zero. */
                uint64_t v = mld(a + offs, eb);
                unsigned lanes = (Q ? 16u : 8u) / eb;
                memset(s->v[t], 0, 16);
                for (unsigned l = 0; l < lanes; l++) elem_wr(s, (int)t, l, eb, v);
            } else if (L) {
                elem_wr(s, (int)t, index, eb, mld(a + offs, eb));
            } else {
                mst(a + offs, elem_rd(s, (int)t, index, eb), eb);
            }
            offs += eb;
            t = (t + 1) & 31;
        }

        if (post) wsp(s, rn, a + (rm == 0x1f ? offs : rz(s, rm)), 1);
        return A64_NEXT;
    }

    return A64_UNDEF;
}
