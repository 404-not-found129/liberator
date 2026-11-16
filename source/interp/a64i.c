// a64i.c — the top-level A64 decode (Arm ARM C4.1, the table on bits 28:25) and the public entry point.
#include "a64_priv.h"

int a64_step(a64 *s, uint32_t i) {
    unsigned op0 = B(i, 28, 25);

    /* The order here is the arm's own table, not a guess: the five groups are selected by masks on op0 and
          they do not overlap. Written as masks rather than a 16-way switch so each line is checkable against
          the manual's "op0" column (x = don't care). */
    if ((op0 & 0xe) == 0x8) return a64_dp_imm(s, i);        /* 100x */
    if ((op0 & 0xe) == 0xa) return a64_br_sys(s, i);        /* 101x */
    if ((op0 & 0x5) == 0x4) return a64_ldst(s, i);          /* x1x0 */
    if ((op0 & 0x7) == 0x5) return a64_dp_reg(s, i);        /* x101 */
    if ((op0 & 0x7) == 0x7) return a64_fp(s, i);            /* x111 */

    /* op0 = 0000 with the top half clear is UDF, permanently undefined by the architecture. It is a trap,
       not a gap in this interpreter, and the two must not be reported as the same thing: a `udf`-first
       "function" is data that something branched into, and saying "unimplemented" would send the reader
       looking for a missing encoding instead of a wild branch. */
    if (op0 == 0 && B(i, 31, 16) == 0) return A64_BRK;

    return A64_UNDEF;                                       /* 0001/0011 unallocated, 0010 SVE */
}

/* The cache's decode, routed by the same table as a64_step above (the same masks in the same order), then
      the group's own spec. 0 = not a decoded form: the cache stores the word and runs a64_step on it. */
int a64_spec(uint32_t i, struct a64_dec *d) {
    unsigned op0 = B(i, 28, 25);
    if ((op0 & 0xe) == 0x8) return a64_dp_imm_spec(i, d);
    if ((op0 & 0xe) == 0xa) return a64_br_sys_spec(i, d);
    if ((op0 & 0x5) == 0x4) return a64_ldst_spec(i, d);
    if ((op0 & 0x7) == 0x5) return a64_dp_reg_spec(i, d);
    return A64X_NONE;
}

const char *a64_event_name(int ev) {
    switch (ev) {
    case A64_NEXT:  return "next";
    case A64_JUMP:  return "jump";
    case A64_CALL:  return "call";
    case A64_RET:   return "ret";
    case A64_SVC:   return "svc";
    case A64_BRK:   return "trap";
    default:        return "unimplemented";
    }
}
