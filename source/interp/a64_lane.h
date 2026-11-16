// a64_lane.h — lane access and saturation for the Advanced SIMD groups. Internal to interp/.
#pragma once
#include "a64_priv.h"

/* A vector result is built in A temporary and stored once. Writing lanes of Vd in place is wrong whenever
      Vd is also a source — `add v0.4s, v0.4s, v0.4s` is fine, but `zip1 v0.4s, v0.4s, v1.4s` and every
      pairwise, permute and narrowing form read lanes they have already overwritten. */
typedef struct { uint8_t b[16]; } vreg;

static inline uint64_t lrd(const uint8_t *v, unsigned idx, unsigned eb) {
    uint64_t x = 0; memcpy(&x, v + idx * eb, eb); return x;
}
static inline void lwr(uint8_t *v, unsigned idx, unsigned eb, uint64_t x) {
    memcpy(v + idx * eb, &x, eb);
}
static inline int64_t sgn(uint64_t x, unsigned eb) { return (int64_t)sx(x, (int)eb * 8); }
static inline uint64_t mask_eb(unsigned eb) { return eb == 8 ? ~0ULL : ((1ULL << (eb * 8)) - 1); }

/* the result register: lanes above the operation's datasize are zero (every 64-bit form clears the top half) */
static inline void vput(a64 *s, unsigned rd, const vreg *t, int Q) {
    memcpy(s->v[rd], t->b, 16);
    if (!Q) memset(s->v[rd] + 8, 0, 8);
}

/* signed and unsigned saturation at eb bytes */
static inline uint64_t sat_sgn(int64_t x, unsigned eb) {
    int64_t hi = eb == 8 ? INT64_MAX : (int64_t)((1ULL << (eb * 8 - 1)) - 1);
    int64_t lo = eb == 8 ? INT64_MIN : -(int64_t)(1ULL << (eb * 8 - 1));
    if (x > hi) x = hi;
    if (x < lo) x = lo;
    return (uint64_t)x & mask_eb(eb);
}
static inline uint64_t sat_uns(uint64_t x, int neg, unsigned eb) {
    if (neg) return 0;
    uint64_t hi = mask_eb(eb);
    return x > hi ? hi : x;
}
