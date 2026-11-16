// a64_cache.h — the decoded-instruction cache (a64_cache.c). Internal to interp/.
#pragma once
#include "a64_priv.h"
#include <pthread.h>
#include <sys/mman.h>

typedef int (*a64_xfn)(a64 *s, const struct a64_dec *d);
struct a64_ce { uint64_t key; a64_xfn x; struct a64_dec d; };     /* 32 bytes */
#define A64C_N 16384                                               /* 512 KB a thread; 64 KB of straight-line code */
extern const a64_xfn A64C_XFN[A64X_N];
void a64c_fill(struct a64_ce *e, uint64_t key, uint32_t insn);
uint64_t a64c_epoch(unsigned *gen);     /* the current epoch, as key bits (and the generation it was taken against);
                                           starts a new one if a64_code_gen moved */
#define A64_CODECHG 100                 /* a generic entry ran (pc not advanced, as for A64_NEXT) and found
                                           a64_code_gen moved: take a new epoch, then continue */
struct a64_ce *a64c_table(void);        /* this thread's table, made on first use; 0 = none (run uncached) */
void a64c_flush(void);
int a64c_prime(uint64_t pc, uint32_t insn);
void a64c_poison(uint64_t pc);
int a64c_step(a64 *s, uint32_t insn_if_miss, int *hit);
