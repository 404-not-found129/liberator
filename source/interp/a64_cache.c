// a64_cache.c — the decoded-instruction cache: one direct-mapped table per thread, keyed by pc and an epoch.
//
// What it saves. a64_step re-decodes every instruction every time it runs: the op0 table, the group's own
//    tests, the field extraction, and for logical immediates the whole DecodeBitMasks. An entry holds the decoded
//    form (struct a64_dec) and the exec function, so a hit is one index, one compare and one call.
// One source of semantics: the exec functions are what a64_step runs for those forms (a64_priv.h). Everything not
//    decoded is a generic entry: the stored instruction word handed to a64_step, i.e. exactly the uncached path.
// Invalidation: a key is pc | epoch << 48 (guest addresses are below 2^47, and an epoch is never 0, so a zeroed
//    slot cannot match). A new epoch — taken when a64_code_gen moves — orphans every older entry at once; at the
//    16-bit wrap the table is cleared. The cache never decides whether code changed; a64_code_gen does.
// Per thread, never shared: two threads filling one slot would need a lock or a seqlock on every hit.
#include "a64_cache.h"
#include <stdlib.h>

static int x_generic(a64 *s, const struct a64_dec *d) {
    int ev = a64_step(s, (uint32_t)d->imm);
    /* An `ic ivau` followed by a direct call into the rewritten function would still run the old code if
          the epoch were only re-read after a native call. Checked here, on the generic path only
          (where IC and ISB live), so a decoded form pays nothing. */
    if (ev == A64_NEXT && __atomic_load_n(&a64_code_gen, __ATOMIC_ACQUIRE) != s->cgen) return A64_CODECHG;
    return ev;
}
#define X(id, fn) [id] = fn
static int x_none(a64 *s, const struct a64_dec *d) { (void)s; (void)d; return A64_UNDEF; }
const a64_xfn A64C_XFN[A64X_N] = {
    X(A64X_NONE, x_none),
    X(A64X_ADR, a64x_adr), X(A64X_ADDSUB_IMM, a64x_addsub_imm), X(A64X_LOGIC_IMM, a64x_logic_imm), X(A64X_MOVW, a64x_movw),
    X(A64X_LOGIC_REG, a64x_logic_reg), X(A64X_ADDSUB_REG, a64x_addsub_reg), X(A64X_CSEL, a64x_csel), X(A64X_MADD, a64x_madd),
    X(A64X_B, a64x_b), X(A64X_CB, a64x_cb), X(A64X_TB, a64x_tb), X(A64X_BCOND, a64x_bcond),
    X(A64X_LDST_PAIR, a64x_ldst_pair), X(A64X_LDST_REG, a64x_ldst_reg),
    X(A64X_GENERIC, x_generic),
};

void a64c_fill(struct a64_ce *e, uint64_t key, uint32_t insn) {
    int id = a64_spec(insn, &e->d);
    if (!id) { memset(&e->d, 0, sizeof e->d); e->d.imm = insn; id = A64X_GENERIC; }
    e->x = A64C_XFN[id];
    e->key = key;
}

/* the thread's table and epoch. tl_gen is the a64_code_gen this thread's epoch was taken against. */
static __thread struct a64_ce *tl_ct;
static __thread unsigned tl_ep, tl_gen;

uint64_t a64c_epoch(unsigned *gen) {
    unsigned g = __atomic_load_n(&a64_code_gen, __ATOMIC_ACQUIRE);
    if (gen) *gen = g;
    if (!tl_ep || g != tl_gen) {
        tl_gen = g;
        if (++tl_ep > 0xffff) {                             /* 16-bit wrap: every key is ambiguous, so clear */
            if (tl_ct) memset(tl_ct, 0, sizeof(struct a64_ce) * A64C_N);
            tl_ep = 1;
        }
    }
    return (uint64_t)tl_ep << 48;
}

static pthread_key_t g_ctkey; static pthread_once_t g_ctonce = PTHREAD_ONCE_INIT;
static void ct_free(void *p) { if (p) munmap(p, sizeof(struct a64_ce) * A64C_N); }
static void ct_key(void) { pthread_key_create(&g_ctkey, ct_free); }

struct a64_ce *a64c_table(void) {
    if (tl_ct) return tl_ct;
    /* mmap, not malloc: 512 KB a thread, and only the pages its code actually touches become resident */
    void *p = mmap(0, sizeof(struct a64_ce) * A64C_N, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return 0;                          /* no table: the caller runs uncached, nothing breaks */
    pthread_once(&g_ctonce, ct_key);
    pthread_setspecific(g_ctkey, p);
    tl_ct = p;
    return tl_ct;
}

/* ─── the instruction tester's view: one instruction through the cache, cold, warm or poisoned ─── */
void a64c_flush(void) { if (tl_ct) memset(tl_ct, 0, sizeof(struct a64_ce) * A64C_N); tl_ep = 0; }
int a64c_prime(uint64_t pc, uint32_t insn) {
    struct a64_ce *t = a64c_table(); if (!t) return -1;
    uint64_t ek = a64c_epoch(0);
    struct a64_ce *e = &t[(pc >> 2) & (A64C_N - 1)];
    a64c_fill(e, pc | ek, insn);
    return e->x == A64C_XFN[A64X_GENERIC] ? 0 : 1;          /* 1 = a decoded form, 0 = generic */
}
/* Negative control only: make the entry at pc wrong in a way a re-decode would never be — Rd moved by one for a decoded
   form, bit 0 of the stored word flipped for a generic one. The instruction tester must then differ, which proves a warm pass really
   executes the stored entry and does not quietly decode again. */
void a64c_poison(uint64_t pc) {
    struct a64_ce *e = &tl_ct[(pc >> 2) & (A64C_N - 1)];
    if (e->x == A64C_XFN[A64X_GENERIC]) e->d.imm ^= 1; else e->d.rd = (uint8_t)((e->d.rd + 1) & 31);
}
int a64c_step(a64 *s, uint32_t insn_if_miss, int *hit) {
    struct a64_ce *t = a64c_table();
    uint64_t ek = a64c_epoch(&s->cgen);
    struct a64_ce *e = &t[(s->pc >> 2) & (A64C_N - 1)];
    *hit = e->key == (s->pc | ek);
    if (!*hit) a64c_fill(e, s->pc | ek, insn_if_miss);
    int ev = e->x(s, &e->d);
    return ev == A64_CODECHG ? A64_NEXT : ev;
}
