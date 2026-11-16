// a64_brsys.c — Branches, Exception generating and System instructions: Arm ARM C4.1.65.
#include "a64_priv.h"

/* Pointer authentication is identity, exactly as the lifter treats it (arm2x86.py, the pac/aut family):
      x86_64 has no signature bits, so sign, authenticate and strip all leave the value alone. The REASON it
      is safe here is that both sides agree — translated code never produces a signed pointer either, so an
      authenticate never meets one. */
static inline uint64_t strip(uint64_t v) { return v; }

/* MRS/MSR encode the register as op0:op1:CRn:CRm:op2; this packs them the way the Arm ARM writes them. */
#define SR(op0, op1, crn, crm, op2) (((op0) << 16) | ((op1) << 13) | ((crn) << 9) | ((crm) << 5) | (op2))

/* The decoded forms of this group (see a64_priv.h): B/BL, CBZ/CBNZ, TBZ/TBNZ, B.cond. They are the group's
      first four tests, in the same order, so claiming them first is the group's own routing. The offset is decoded
      as a signed byte offset; the target is still pc-relative at execution, exactly as before. */
int a64_br_sys_spec(uint32_t i, struct a64_dec *d) {
    memset(d, 0, sizeof *d);
    if (B(i, 30, 26) == 0x05) {                             /* Unconditional branch (immediate): B, BL */
        d->imm = sx(B(i, 25, 0), 26) << 2; d->op = (uint8_t)BIT(i, 31);
        return A64X_B;
    }
    if (B(i, 30, 25) == 0x1a) {                             /* CBZ, CBNZ */
        d->sf = (uint8_t)BIT(i, 31); d->rd = (uint8_t)B(i, 4, 0); d->op = (uint8_t)BIT(i, 24);
        d->imm = sx(B(i, 23, 5), 19) << 2;
        return A64X_CB;
    }
    if (B(i, 30, 25) == 0x1b) {                             /* TBZ, TBNZ */
        d->ra = (uint8_t)((BIT(i, 31) << 5) | B(i, 23, 19)); d->rd = (uint8_t)B(i, 4, 0); d->op = (uint8_t)BIT(i, 24);
        d->imm = sx(B(i, 18, 5), 14) << 2;
        return A64X_TB;
    }
    if (B(i, 31, 25) == 0x2a) {                             /* B.cond (o1 = 0); BC.cond (o1 = 1) branches the same way */
        if (BIT(i, 4)) return A64X_NONE;
        d->ra = (uint8_t)B(i, 3, 0); d->imm = sx(B(i, 23, 5), 19) << 2;
        return A64X_BCOND;
    }
    return A64X_NONE;
}

int a64x_b(a64 *s, const struct a64_dec *d) {
    uint64_t t = s->pc + d->imm;
    if (d->op) { s->x[30] = s->pc + 4; s->pc = t; return A64_CALL; }
    s->pc = t;
    return A64_JUMP;
}
int a64x_cb(a64 *s, const struct a64_dec *d) {
    uint64_t v = trw(rz(s, d->rd), d->sf);
    if ((d->op && v) || (!d->op && !v)) { s->pc += d->imm; return A64_JUMP; }
    return A64_NEXT;
}
int a64x_tb(a64 *s, const struct a64_dec *d) {
    uint64_t v = rz(s, d->rd);
    if ((int)((v >> d->ra) & 1) == d->op) { s->pc += d->imm; return A64_JUMP; }
    return A64_NEXT;
}
int a64x_bcond(a64 *s, const struct a64_dec *d) {
    if (a64_cond(s, d->ra)) { s->pc += d->imm; return A64_JUMP; }
    return A64_NEXT;
}

int a64_br_sys(a64 *s, uint32_t i) {
    { struct a64_dec d; int id = a64_br_sys_spec(i, &d); if (id) return a64x_run(id, s, &d); }
    if (B(i, 31, 25) == 0x2a) return A64_UNDEF;             /* B.cond with bit 4 set: the one invalid encoding left */

    /* Exception generation: SVC, BRK, HLT (and HVC/SMC, which userland cannot reach) */
    if (B(i, 31, 24) == 0xd4) {
        unsigned opc = B(i, 23, 21), ll = B(i, 1, 0);
        if (opc == 0 && ll == 1) { s->pc += 4; return A64_SVC; }    /* the caller performs the syscall */
        if ((opc == 1 || opc == 2) && ll == 0) return A64_BRK;      /* BRK / HLT: a trap, pc unchanged */
        return A64_UNDEF;
    }

    /* Unconditional branch (register): BR, BLR, RET and their authenticated forms */
    if (B(i, 31, 25) == 0x6b) {
        unsigned opc = B(i, 24, 21), op2 = B(i, 20, 16), op3 = B(i, 15, 10), rn = B(i, 9, 5), op4 = B(i, 4, 0);
        int auth = (op3 == 2 || op3 == 3);                          /* ...AA / ...AB */
        if (op2 != 0x1f) return A64_UNDEF;
        if (!auth && op3 != 0) return A64_UNDEF;
        switch (opc) {
        case 0:                                                     /* BR, BRAAZ, BRABZ */
            if (op4 != (auth ? 0x1f : 0)) return A64_UNDEF;
            s->pc = strip(rz(s, rn));
            return A64_JUMP;
        case 1: {                                                   /* BLR, BLRAAZ, BLRABZ */
            if (op4 != (auth ? 0x1f : 0)) return A64_UNDEF;
            uint64_t t = strip(rz(s, rn));
            s->x[30] = s->pc + 4; s->pc = t;
            return A64_CALL;
        }
        case 2:                                                     /* RET, RETAA, RETAB */
            if (auth) { if (rn != 0x1f || op4 != 0x1f) return A64_UNDEF; s->pc = strip(s->x[30]); }
            else      { if (op4 != 0) return A64_UNDEF;              s->pc = strip(rz(s, rn)); }
            return A64_RET;
        case 8:                                                     /* BRAA, BRAB (modifier in op4) */
            if (!auth) return A64_UNDEF;
            s->pc = strip(rz(s, rn));
            return A64_JUMP;
        case 9: {                                                   /* BLRAA, BLRAB */
            if (!auth) return A64_UNDEF;
            uint64_t t = strip(rz(s, rn));
            s->x[30] = s->pc + 4; s->pc = t;
            return A64_CALL;
        }
        default: return A64_UNDEF;                                  /* ERET, DRPS: EL1+ only */
        }
    }

    /* System: hints, barriers, PSTATE, SYS/SYSL, MRS/MSR (register) */
    if (B(i, 31, 22) == 0x354) {
        unsigned L = BIT(i, 21), op0 = B(i, 20, 19), op1 = B(i, 18, 16);
        unsigned crn = B(i, 15, 12), crm = B(i, 11, 8), op2 = B(i, 7, 5), rt = B(i, 4, 0);

        if (op0 == 0 && rt == 0x1f) {
            /* Hints are architecturally NOPS for us, including the BTI and PAC-on-LR hints: nothing in
                  this interpreter has a branch-target or signature state for them to affect. */
            if (crn == 2) return A64_NEXT;
            if (crn == 3) {                                          /* barriers */
                if (op2 == 2) { s->excl_size = 0; return A64_NEXT; } /* CLREX drops the reservation */
                /* DSB/DMB/ISB/SB need no fence on x86: TSO is strictly stronger than arm64's model, which is
                      why the lifter drops them too. On an arm64 host (where the instruction tests run
                      their reference side) the compiler's own seq_cst atomics above carry the ordering. */
                if (op2 >= 4) return A64_NEXT;
                return A64_UNDEF;
            }
            if (crn == 4) {                                          /* MSR (immediate): PSTATE fields */
                /* Only the flag fields are ours. CFINV/XAFLAG/AXFLAG and the DAIF/SSBS/TCO writes change
                      state this interpreter does not model, so they are refused rather than ignored: ignoring
                      a PSTATE write makes a later conditional branch silently take the wrong edge. */
                if (op1 == 0 && op2 == 1) { s->nzcv ^= 1ULL << 29; return A64_NEXT; }   /* CFINV */
                return A64_UNDEF;
            }
            return A64_UNDEF;
        }

        if (op0 == 1 && !L) {                                        /* SYS: cache and TLB maintenance */
            /* DC ZVA (op1=3, CRn=7, CRm=4, op2=1) zeroes one cache line. Everything else here (IC IVAU,
               DC CVAU/CIVAC, the prefetch hints) is a no-op on a coherent, unified-cache host. */
            if (op1 == 3 && crn == 7 && crm == 4 && op2 == 1) {
                uint64_t a = rz(s, rt) & ~63ULL;                     /* 64-byte line, = the DCZID_EL0 we report */
                memset((void *)(uintptr_t)a, 0, 64);
                return A64_NEXT;
            }
            /* IC IVAU (op1 3, CRm 5, op2 1), IC IALLU (op1 0, CRm 5, op2 0), IC IALLUIS (op1 0, CRm 1, op2 0):
                  the guest says it changed code. Still a no-op for the data side; every decode cache starts a new
                  epoch, which is what the instruction means for a cache of decoded instructions. */
            if (crn == 7 && ((op1 == 3 && crm == 5 && op2 == 1) || (op1 == 0 && (crm == 5 || crm == 1) && op2 == 0)))
                a64_code_changed();
            if (crn == 7 || crn == 8) return A64_NEXT;
            return A64_UNDEF;
        }

        if (op0 >= 2) {                                              /* MRS / MSR (register) */
            unsigned r = SR(op0, op1, crn, crm, op2);
            if (L) {                                                 /* MRS: read */
                switch (r) {
                case SR(3, 3, 13, 0, 3):                             /* TPIDRRO_EL0 */
                case SR(3, 3, 13, 0, 2):                             /* TPIDR_EL0   */
                    /* The TSD base, the one system register userland really reads: _pthread_getspecific,
                          the stack protector and the ObjC fast paths all come through here. */
                    wz(s, rt, s->tsd, 1); return A64_NEXT;
                case SR(3, 3, 13, 0, 5): wz(s, rt, s->tpidr2, 1); return A64_NEXT;       /* TPIDR2_EL0 */
                case SR(3, 3, 4, 2, 0):  wz(s, rt, s->nzcv, 1); return A64_NEXT;         /* NZCV */
                case SR(3, 3, 4, 4, 0):  wz(s, rt, s->fpcr, 1); return A64_NEXT;         /* FPCR */
                case SR(3, 3, 4, 4, 1):  wz(s, rt, s->fpsr, 1); return A64_NEXT;         /* FPSR */
                case SR(3, 3, 14, 0, 0): wz(s, rt, 24000000, 1); return A64_NEXT;        /* CNTFRQ_EL0: 24 MHz */
                case SR(3, 3, 14, 0, 2):                                                 /* CNTVCT_EL0 */
                    if (!s->cntvct) return A64_UNDEF;
                    wz(s, rt, s->cntvct(), 1); return A64_NEXT;
                /* CTR_EL0 and DCZID_EL0 are read from real hardware, not invented numbers: 64-byte I and D
                      lines (L1Ip = 3, physically-indexed) and a 64-byte DC ZVA block, which is what the
                      DC ZVA above actually does. They must agree or a hand-written bzero reads the size
                      here and clears a different amount. */
                case SR(3, 3, 0, 0, 1):  wz(s, rt, 0x8444c004, 1); return A64_NEXT;      /* CTR_EL0 */
                case SR(3, 3, 0, 0, 7):  wz(s, rt, 4, 1); return A64_NEXT;               /* DCZID_EL0 */
                default: return A64_UNDEF;                           /* every other sysreg: say so, never guess */
                }
            }
            switch (r) {                                             /* MSR: write */
            case SR(3, 3, 4, 2, 0): s->nzcv = rz(s, rt) & 0xf0000000ULL; return A64_NEXT;
            case SR(3, 3, 4, 4, 0): s->fpcr = rz(s, rt); return A64_NEXT;
            case SR(3, 3, 4, 4, 1): s->fpsr = rz(s, rt); return A64_NEXT;
            case SR(3, 3, 13, 0, 5): s->tpidr2 = rz(s, rt); return A64_NEXT;
            default: return A64_UNDEF;
            }
        }
        return A64_UNDEF;
    }

    return A64_UNDEF;
}
