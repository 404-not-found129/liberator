// a64_priv.h — helpers shared by the decode groups. Internal to interp/.
#pragma once
#include "a64i.h"
#include <string.h>

#define B(v, hi, lo) ((uint32_t)(((v) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1)))
#define BIT(v, n)    (((v) >> (n)) & 1u)

static inline uint64_t sx(uint64_t v, int bits) { return (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits)); }
static inline uint64_t trw(uint64_t v, int sf) { return sf ? v : (uint64_t)(uint32_t)v; }

/* REGISTER 31 is ZR in most fields and SP in A FEW, and which one is per-field, not per-instruction:
      `add sp, sp, #16` has SP in both Rd and Rn while `adds xzr, x0, #1` has ZR in Rd. Two accessor pairs,
      and each decode site picks the right one from the Arm ARM's register-name column. */
static inline uint64_t rz(a64 *s, int r) { return r == 31 ? 0 : s->x[r]; }
static inline uint64_t rsp(a64 *s, int r) { return r == 31 ? s->sp : s->x[r]; }
static inline void wz(a64 *s, int r, uint64_t v, int sf) { if (r != 31) { s->x[r] = trw(v, sf); if (r == 6 || r == 7) s->argw |= (uint8_t)(1u << (r - 6)); } }
static inline void wsp(a64 *s, int r, uint64_t v, int sf) { v = trw(v, sf); if (r == 31) s->sp = v; else { s->x[r] = v; if (r == 6 || r == 7) s->argw |= (uint8_t)(1u << (r - 6)); } }

static inline void setf(a64 *s, int n, int z, int c, int v) {
    s->nzcv = ((uint64_t)(n & 1) << 31) | ((uint64_t)(z & 1) << 30) | ((uint64_t)(c & 1) << 29) | ((uint64_t)(v & 1) << 28);
}
static inline int fN(const a64 *s) { return (int)(s->nzcv >> 31) & 1; }
static inline int fZ(const a64 *s) { return (int)(s->nzcv >> 30) & 1; }
static inline int fC(const a64 *s) { return (int)(s->nzcv >> 29) & 1; }
static inline int fV(const a64 *s) { return (int)(s->nzcv >> 28) & 1; }

int a64_cond(const a64 *s, unsigned c);
uint64_t a64_awc(a64 *s, uint64_t a, uint64_t b, unsigned carry, int sf, int flags);
int a64_bitmask(unsigned N, unsigned imms, unsigned immr, int sf, uint64_t *wmask);
uint64_t a64_shift(uint64_t v, unsigned type, unsigned amt, int sf);
uint64_t a64_extend(uint64_t v, unsigned option, unsigned sh);

/* memory. The arm64 images are mapped at their own addresses in this process, so a load is a load.
   memcpy, not a cast: these addresses are frequently unaligned and a typed store would be UB. */
/* The size is A run-time value (1u << size), so a bare memcpy was a call through the dyld stub into libsystem
      for every guest load and store: 16 such call sites in a64_ldst alone. Each constant-size arm below is a
      single mov. Any other size keeps the memcpy. */
/* The five memcpy arms above were folded by the optimizer back into one memcpy(d, s, n) call, which
      showed up as 30 of 264 profile samples in _platform_memmove from mld/mst. Unaligned may_alias typed
      accesses cannot be merged that way: each arm is one mov. x86 allows unaligned access, and a single mov is
      single-copy atomic for an aligned access, as the arm64 load/store it stands for. */
typedef uint16_t __attribute__((aligned(1), may_alias)) a64_u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) a64_u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) a64_u64u;
static inline void cpyn(void *d, const void *s, unsigned n) {
    switch (n) {
    case 1:  *(uint8_t *)d = *(const uint8_t *)s; break;
    case 2:  *(a64_u16u *)d = *(const a64_u16u *)s; break;
    case 4:  *(a64_u32u *)d = *(const a64_u32u *)s; break;
    case 8:  *(a64_u64u *)d = *(const a64_u64u *)s; break;
    case 16: ((a64_u64u *)d)[0] = ((const a64_u64u *)s)[0]; ((a64_u64u *)d)[1] = ((const a64_u64u *)s)[1]; break;
    default: memcpy(d, s, n); break;
    }
}
static inline uint64_t mld(uint64_t a, unsigned n) {
    uint64_t v = 0; cpyn(&v, (const void *)(uintptr_t)a, n); return v;
}
static inline void mst(uint64_t a, uint64_t v, unsigned n) { cpyn((void *)(uintptr_t)a, &v, n); }

/* a scalar SIMD&FP write zeroes the rest of the 128 bits (Arm ARM: "the upper bits are set to zero") */
static inline void vzw(a64 *s, int t, const void *p, unsigned n) { memset(s->v[t], 0, 16); cpyn(s->v[t], p, n); }
static inline uint64_t vrd(const a64 *s, int t, unsigned n) { uint64_t v = 0; cpyn(&v, s->v[t], n); return v; }

int a64_fpnan2f(float, float, float *);     /* ARM's FPProcessNaNs; 1 = the NaN result is the answer */
int a64_fpnan2d(double, double, double *);

float  a64_vmaxf(float, float);    float  a64_vminf(float, float);
float  a64_vmaxnmf(float, float);  float  a64_vminnmf(float, float);
double a64_vmaxd(double, double);  double a64_vmind(double, double);
double a64_vmaxnmd(double, double); double a64_vminnmd(double, double);

uint64_t a64_rmw(uint64_t a, unsigned n, unsigned op, uint64_t v);
uint64_t a64_swp(uint64_t a, unsigned n, uint64_t v);
uint64_t a64_cas(uint64_t a, unsigned n, uint64_t expect, uint64_t nw);
int a64_casp(uint64_t a, unsigned n, uint64_t *e0, uint64_t *e1, uint64_t n0, uint64_t n1);

/* the decode groups; each returns an A64_* event or A64_UNDEF */
int a64_dp_imm(a64 *s, uint32_t i);
int a64_dp_reg(a64 *s, uint32_t i);
int a64_br_sys(a64 *s, uint32_t i);
int a64_ldst(a64 *s, uint32_t i);
int a64_ldstruct(a64 *s, uint32_t i);    /* the Advanced SIMD structure forms, reached from a64_ldst */
int a64_fp(a64 *s, uint32_t i);
int a64_fpsc(a64 *s, uint32_t i);      /* scalar floating point */
int a64_simd(a64 *s, uint32_t i);      /* Advanced SIMD: copy, immediate, shifts, permutes, reductions */
int a64_simd3(a64 *s, uint32_t i);     /* Advanced SIMD: three-same and two-register misc, integer */
int a64_simdsh(a64 *s, uint32_t i);    /* Advanced SIMD: shift by immediate, across lanes, three-different */
int a64_simdfp(a64 *s, uint32_t i);    /* Advanced SIMD: the floating-point forms */
int a64_simdidx(a64 *s, uint32_t i);   /* Advanced SIMD: vector x indexed element */

uint64_t a64_frecpe(uint64_t bits, int n);    /* the 8-bit estimate tables read from hardware, shared with the lifter */
uint64_t a64_frsqrte(uint64_t bits, int n);

int a64_crypto(a64 *s, uint32_t i);    /* the AES and SHA3 groups (bits 31:24 = 0x4e / 0xce) */
int a64_sha(a64 *s, uint32_t i);       /* the SHA-1 and SHA-256 round instructions (0x5e) */

/* ───────── decoded forms: decode once, execute many (the block cache in a64_rt.c / a64_cache.c) ─────────
   The hottest integer forms are split into a decode (a64_*_spec: fields -> struct a64_dec, 0 = not one of these
   forms or an invalid encoding of one) and an exec (a64x_*: the semantics, reading only the struct and the state).
   One source: a64_step reaches these forms only through the same spec + exec pair the cache uses — each group
      calls its spec first and runs the exec — so the instruction tester's plain a64_step pass certifies the very code the cache runs.
      The old inline code for these forms is gone from the groups; an encoding the spec rejects falls through to the
      group's own remaining code, which returns A64_UNDEF for it exactly as before.
   The routing is the group's own: a64_spec() (a64i.c) uses the same op0 table as a64_step, and each spec claims
   only encodings its group would have reached that form for (each spec says why its tests are disjoint from the
   branches that came before it in the group). */
struct a64_dec {
    uint64_t imm;                       /* immediate, offset, logical mask, or (generic) the instruction word */
    uint8_t rd, rn, rm, ra;             /* ra doubles as Rt2 / cond / bit / hw / option, per form */
    uint8_t sf, op, S, f;               /* sf doubles as the access size in bytes for loads and stores */
};
enum {
    A64X_NONE = 0,
    A64X_ADR, A64X_ADDSUB_IMM, A64X_LOGIC_IMM, A64X_MOVW,          /* a64_dpimm.c */
    A64X_LOGIC_REG, A64X_ADDSUB_REG, A64X_CSEL, A64X_MADD,         /* a64_dpreg.c */
    A64X_B, A64X_CB, A64X_TB, A64X_BCOND,                          /* a64_brsys.c */
    A64X_LDST_PAIR, A64X_LDST_REG,                                 /* a64_ldst.c  */
    A64X_GENERIC,                                                  /* a64_cache.c: a64_step on the stored word */
    A64X_N
};
int a64_dp_imm_spec(uint32_t i, struct a64_dec *d);
int a64_dp_reg_spec(uint32_t i, struct a64_dec *d);
int a64_br_sys_spec(uint32_t i, struct a64_dec *d);
int a64_ldst_spec(uint32_t i, struct a64_dec *d);
int a64_spec(uint32_t i, struct a64_dec *d);        /* a64i.c: route like a64_step, then the group's spec */
int a64x_adr(a64 *s, const struct a64_dec *d);
int a64x_addsub_imm(a64 *s, const struct a64_dec *d);
int a64x_logic_imm(a64 *s, const struct a64_dec *d);
int a64x_movw(a64 *s, const struct a64_dec *d);
int a64x_logic_reg(a64 *s, const struct a64_dec *d);
int a64x_addsub_reg(a64 *s, const struct a64_dec *d);
int a64x_csel(a64 *s, const struct a64_dec *d);
int a64x_madd(a64 *s, const struct a64_dec *d);
int a64x_b(a64 *s, const struct a64_dec *d);
int a64x_cb(a64 *s, const struct a64_dec *d);
int a64x_tb(a64 *s, const struct a64_dec *d);
int a64x_bcond(a64 *s, const struct a64_dec *d);
int a64x_ldst_pair(a64 *s, const struct a64_dec *d);
int a64x_ldst_reg(a64 *s, const struct a64_dec *d);
/* one switch, so a group's call to its exec is a direct (inlinable) call, not an indirect one */
static inline int a64x_run(int id, a64 *s, const struct a64_dec *d) {
    switch (id) {
    case A64X_ADR:        return a64x_adr(s, d);
    case A64X_ADDSUB_IMM: return a64x_addsub_imm(s, d);
    case A64X_LOGIC_IMM:  return a64x_logic_imm(s, d);
    case A64X_MOVW:       return a64x_movw(s, d);
    case A64X_LOGIC_REG:  return a64x_logic_reg(s, d);
    case A64X_ADDSUB_REG: return a64x_addsub_reg(s, d);
    case A64X_CSEL:       return a64x_csel(s, d);
    case A64X_MADD:       return a64x_madd(s, d);
    case A64X_B:          return a64x_b(s, d);
    case A64X_CB:         return a64x_cb(s, d);
    case A64X_TB:         return a64x_tb(s, d);
    case A64X_BCOND:      return a64x_bcond(s, d);
    case A64X_LDST_PAIR:  return a64x_ldst_pair(s, d);
    case A64X_LDST_REG:   return a64x_ldst_reg(s, d);
    default:              return A64_UNDEF;
    }
}
/* Code-change generation. Every per-thread decode cache compares this at its run's entry and after every return
      from native code; a change starts a new epoch, so no entry decoded before it can match again. Bumped by the
      host when guest code bytes can have changed (an image sealed/loaded, a re-seal whose refreshed bytes differ)
      and by the guest's own IC IVAU / IC IALLU — the arm64 rule for code that writes code. */
extern unsigned a64_code_gen;
static inline void a64_code_changed(void) { __atomic_add_fetch(&a64_code_gen, 1, __ATOMIC_RELEASE); }
