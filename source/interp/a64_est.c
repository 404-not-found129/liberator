// a64_est.c — FRECPE and FRSQRTE: the reciprocal and reciprocal-square-root ESTIMATES.
//
// These are 8-bit table lookups, not 1/x and 1/sqrt(x). A true reciprocal is more accurate and still
//    wrong, and it passes every eyeball check — which is why the tables in a64_esttab.h were read from
//    hardware rather than written from the pseudocode. This is a line-for-line port of the lifter's own
//    frecpe_ir / frsqrte_ir (arm2x86.py), so the interpreted and translated forms of the same instruction
//    cannot disagree.
#include "a64_priv.h"
#include "a64_esttab.h"

/* (fraction bits, bias, max exponent) per width */
static void widths(int n, unsigned *fb, uint64_t *bias, uint64_t *emax) {
    switch (n) {
    case 16: *fb = 10; *bias = 15; *emax = 31; break;
    case 32: *fb = 23; *bias = 127; *emax = 255; break;
    default: *fb = 52; *bias = 1023; *emax = 2047; break;
    }
}
#define M52 0xfffffffffffffULL

uint64_t a64_frecpe(uint64_t bits, int n) {
    unsigned fb; uint64_t bias, emax;
    widths(n, &fb, &bias, &emax);
    uint64_t sign = bits >> (n - 1);
    uint64_t exp = (bits >> fb) & emax;
    uint64_t frac = bits & ((1ULL << fb) - 1);
    uint64_t sb = sign << (n - 1);
    uint64_t rinf = sb | (emax << fb);

    if (exp == emax && frac) return bits | (1ULL << (fb - 1));      /* NaN, quieted */
    if (exp == emax) return sb;                                     /* +-inf -> +-0 */
    if (!exp && !frac) return rinf;                                 /* +-0 -> +-inf */

    uint64_t fin = frac << (52 - fb), e, f;
    if (!exp) {                                                     /* subnormal: normalise by one or two */
        int hib = ((fin >> 51) & 1) == 1;
        e = hib ? 0 : (uint64_t)-1;
        f = hib ? ((fin << 1) & M52) : ((fin << 2) & M52);
    } else { e = exp; f = fin; }

    uint64_t est = A64_RECIP_TAB[(f >> 44) & 0xff];
    int64_t rexp = (int64_t)(2 * bias - 1) - (int64_t)e;
    uint64_t fr0 = est << 44, fr;
    if (rexp == 0) fr = (fr0 >> 1) | (1ULL << 51);
    else if (rexp == -1) fr = (fr0 >> 2) | (1ULL << 50);
    else fr = fr0;
    uint64_t rex = (rexp == -1) ? 0 : (uint64_t)rexp;
    uint64_t norm = sb | (rex << fb) | (fr >> (52 - fb));

    /* 532 of 20,000 f32 hardware results need this. The architecture tests overflow before
          normalising, so a deeply subnormal input reciprocates to infinity, not to the largest normal. */
    if (!exp && frac < (1ULL << (fb - 2))) return rinf;
    if (rexp >= (int64_t)emax) return rinf;
    if (rexp < -1) return sb;
    return norm;
}

uint64_t a64_frsqrte(uint64_t bits, int n) {
    unsigned fb; uint64_t bias, emax;
    widths(n, &fb, &bias, &emax);
    uint64_t sign = bits >> (n - 1);
    uint64_t exp = (bits >> fb) & emax;
    uint64_t frac = bits & ((1ULL << fb) - 1);
    uint64_t sb = sign << (n - 1);
    uint64_t dnan = (emax << fb) | (1ULL << (fb - 1));

    if (exp == emax && frac) return bits | (1ULL << (fb - 1));      /* NaN, quieted */
    if (!exp && !frac) return sb | (emax << fb);                    /* +-0 -> +-inf */
    if (exp == emax) return sign ? dnan : 0;                        /* +inf -> +0, -inf is invalid */
    if (sign) return dnan;                                          /* a negative input is invalid */

    uint64_t fin = frac << (52 - fb), e, f;
    if (!exp) {
        /* the architecture's `while fraction<51> == 0` loop is a count-leading-zeros */
        int64_t k = (int64_t)(uint64_t)__builtin_clzll(fin) - 12;
        e = (uint64_t)(-k);
        f = (fin << (k + 1)) & M52;
    } else { e = exp; f = fin; }

    unsigned idx = (e & 1) ? (unsigned)((f >> 45) & 0x7f) : (unsigned)(((f >> 44) & 0xff) + 128);
    uint64_t est = A64_RSQRT_TAB[idx];
    uint64_t rexp = ((uint64_t)(3 * bias - 1) - e) >> 1;
    uint64_t fr0 = est << 44;
    return (rexp << fb) | (fr0 >> (52 - fb));
}
