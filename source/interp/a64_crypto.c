// a64_crypto.c — the AES and SHA3 instruction groups. Arm ARM C4.1.89 ("Cryptographic AES",
// "Cryptographic three-register SHA 512", "Cryptographic four-register").
//
// THE S-BOX is computed, not typed. It is defined as the multiplicative inverse in GF(2^8) followed by a
//    fixed affine map, so deriving it is both shorter than 256 constants and impossible to mistype — and a
//    single wrong byte in a hand-copied table is a bug that only shows up on one input in 256.
#include "a64_lane.h"

static uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (int k = 0; k < 8; k++) {
        if (b & 1) r ^= a;
        uint8_t hi = a & 0x80;
        a = (uint8_t)(a << 1);
        if (hi) a ^= 0x1b;                      /* the AES field polynomial x^8+x^4+x^3+x+1 */
        b >>= 1;
    }
    return r;
}

static uint8_t SBOX[256], ISBOX[256];
static int sbox_ready;

static void sbox_init(void) {
    if (sbox_ready) return;
    for (int x = 0; x < 256; x++) {
        uint8_t inv = 0;
        if (x) for (int y = 1; y < 256; y++) if (gmul((uint8_t)x, (uint8_t)y) == 1) { inv = (uint8_t)y; break; }
        uint8_t s = (uint8_t)(inv ^ 0x63);
        for (int r = 1; r <= 4; r++) s ^= (uint8_t)((inv << r) | (inv >> (8 - r)));
        SBOX[x] = s;
    }
    for (int x = 0; x < 256; x++) ISBOX[SBOX[x]] = (uint8_t)x;
    sbox_ready = 1;
}

/* The AES state is column-major: byte r + 4c is row r, column c. ShiftRows rotates row r left by r. */
static void shift_rows(const uint8_t *in, uint8_t *out, int inverse) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++)
            out[r + 4 * c] = in[r + 4 * ((inverse ? (c - r + 4) : (c + r)) % 4)];
}

static void mix_columns(const uint8_t *in, uint8_t *out, int inverse) {
    static const uint8_t F[4] = {2, 3, 1, 1}, I[4] = {14, 11, 13, 9};
    const uint8_t *M = inverse ? I : F;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            uint8_t v = 0;
            for (int k = 0; k < 4; k++) v ^= gmul(in[((r + k) % 4) + 4 * c], M[k]);
            out[r + 4 * c] = v;
        }
}

static inline uint64_t ror64_(uint64_t v, unsigned n) { return n ? ((v >> n) | (v << (64 - n))) : v; }
static inline uint64_t rol64(uint64_t v, unsigned n) { return n ? ((v << n) | (v >> (64 - n))) : v; }
/* the SHA-512 round and schedule functions */
static inline uint64_t SIG0(uint64_t x) { return ror64_(x, 28) ^ ror64_(x, 34) ^ ror64_(x, 39); }
static inline uint64_t SIG1(uint64_t x) { return ror64_(x, 14) ^ ror64_(x, 18) ^ ror64_(x, 41); }
static inline uint64_t sig0_512(uint64_t x) { return ror64_(x, 1) ^ ror64_(x, 8) ^ (x >> 7); }
static inline uint64_t sig1_512(uint64_t x) { return ror64_(x, 19) ^ ror64_(x, 61) ^ (x >> 6); }

int a64_crypto(a64 *s, uint32_t i) {
    unsigned rd = B(i, 4, 0), rn = B(i, 9, 5), rm = B(i, 20, 16);

    /* ───────── Cryptographic AES ───────── */
    if (B(i, 31, 24) == 0x4e && B(i, 23, 22) == 0 && B(i, 21, 17) == 0x14 && B(i, 11, 10) == 2) {
        unsigned opcode = B(i, 16, 12);
        sbox_init();
        uint8_t a[16], b[16];
        switch (opcode) {
        case 0x04: case 0x05: {                 /* AESE / AESD */
            for (int k = 0; k < 16; k++) a[k] = (uint8_t)(s->v[rd][k] ^ s->v[rn][k]);
            shift_rows(a, b, opcode == 0x05);
            /* SubBytes is byte-wise and ShiftRows is a permutation, so their order does not matter */
            for (int k = 0; k < 16; k++) s->v[rd][k] = opcode == 0x05 ? ISBOX[b[k]] : SBOX[b[k]];
            return A64_NEXT;
        }
        case 0x06: case 0x07:                   /* AESMC / AESIMC */
            memcpy(a, s->v[rn], 16);
            mix_columns(a, b, opcode == 0x07);
            memcpy(s->v[rd], b, 16);
            return A64_NEXT;
        default: return A64_UNDEF;
        }
    }

    if (B(i, 31, 24) != 0xce) return A64_UNDEF;

    /* ───────── Cryptographic four-register: EOR3, BCAX ───────── */
    if (!BIT(i, 23) && !BIT(i, 15)) {
        unsigned ra = B(i, 14, 10);
        if (B(i, 22, 21) > 1) return A64_UNDEF;             /* 10 is SM3SS1 */
        for (int k = 0; k < 16; k++) {
            uint8_t n = s->v[rn][k], m = s->v[rm][k], av = s->v[ra][k];
            /* EOR3 = n ^ m ^ a ; BCAX = n ^ (m & ~a) — the Keccak chi step */
            s->v[rd][k] = BIT(i, 21) ? (uint8_t)(n ^ (m & (uint8_t)~av)) : (uint8_t)(n ^ m ^ av);
        }
        return A64_NEXT;
    }

    /* ───────── RAX1 (three-register SHA 512 group) ───────── */
    if (B(i, 23, 21) == 3 && B(i, 15, 10) == 0x23) {
        uint64_t r0 = lrd(s->v[rn], 0, 8) ^ rol64(lrd(s->v[rm], 0, 8), 1);
        uint64_t r1 = lrd(s->v[rn], 1, 8) ^ rol64(lrd(s->v[rm], 1, 8), 1);
        memcpy(s->v[rd], &r0, 8);
        memcpy(s->v[rd] + 8, &r1, 8);
        return A64_NEXT;
    }

    /* ───────── XAR ───────── */
    if (B(i, 23, 21) == 4) {
        unsigned imm6 = B(i, 15, 10);
        uint64_t r0 = ror64_(lrd(s->v[rn], 0, 8) ^ lrd(s->v[rm], 0, 8), imm6);
        uint64_t r1 = ror64_(lrd(s->v[rn], 1, 8) ^ lrd(s->v[rm], 1, 8), imm6);
        memcpy(s->v[rd], &r0, 8);
        memcpy(s->v[rd] + 8, &r1, 8);
        return A64_NEXT;
    }

    /* ───────── SHA-512: SHA512H, SHA512H2, SHA512SU0, SHA512SU1 ─────────
       Derived from hardware behaviour, not from memory: one input lane set to
          1 at a time, reading which output lanes move and by what. Every response is accounted for, and the
          two-round chaining shows up as a double application — Vm<127:64> = 1 moves the low output by
          0x0000401010000000, which is SIGMA1(SIGMA1(1)) computed by hand. That is what identified the second
          round's operands rather than a guess at them. */
    {
        uint64_t n0 = lrd(s->v[rn], 0, 8), n1 = lrd(s->v[rn], 1, 8);
        uint64_t m0 = lrd(s->v[rm], 0, 8), m1 = lrd(s->v[rm], 1, 8);
        uint64_t d0 = lrd(s->v[rd], 0, 8), d1 = lrd(s->v[rd], 1, 8);
        uint64_t r0, r1;

        if (B(i, 23, 21) == 3 && B(i, 15, 10) == 0x20) {            /* SHA512H */
            uint64_t e1 = m1;
            uint64_t c1 = (e1 & n0) ^ (~e1 & n1);                   /* Ch(e, f, g) */
            r1 = c1 + SIG1(e1) + d1;
            uint64_t e0 = r1 + m0;                                  /* the round's d + T1 becomes the new e */
            uint64_t c0 = (e0 & m1) ^ (~e0 & n0);                   /* (e, f, g) shift by one round */
            r0 = c0 + SIG1(e0) + d0;
        } else if (B(i, 23, 21) == 3 && B(i, 15, 10) == 0x21) {     /* SHA512H2 */
            uint64_t j1 = (n0 & m1) ^ (n0 & m0) ^ (m1 & m0);        /* Maj(a, b, c) */
            r1 = j1 + SIG0(m0) + d1;
            uint64_t j0 = (r1 & m0) ^ (r1 & m1) ^ (m0 & m1);
            r0 = j0 + SIG0(r1) + d0;
        } else if (B(i, 31, 10) == 0x33b020) {                      /* SHA512SU0 Vd.2D, Vn.2D */
            r0 = d0 + sig0_512(d1);                                 /* the message schedule's first two terms */
            r1 = d1 + sig0_512(n0);
        } else if (B(i, 23, 21) == 3 && B(i, 15, 10) == 0x22) {     /* SHA512SU1 */
            r0 = d0 + m0 + sig1_512(n0);
            r1 = d1 + m1 + sig1_512(n1);
        } else return A64_UNDEF;

        memcpy(s->v[rd], &r0, 8);
        memcpy(s->v[rd] + 8, &r1, 8);
        return A64_NEXT;
    }
}

/* ───────── SHA-1 and SHA-256 (bits 31:24 = 0x5e) ───────── */
static inline uint32_t ror32(uint32_t v, unsigned n) { return n ? ((v >> n) | (v << (32 - n))) : v; }
static inline uint32_t rol32(uint32_t v, unsigned n) { return n ? ((v << n) | (v >> (32 - n))) : v; }
static inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) { return ((y ^ z) & x) ^ z; }
static inline uint32_t par(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | ((x | y) & z); }
static inline uint32_t S0(uint32_t x) { return ror32(x, 2) ^ ror32(x, 13) ^ ror32(x, 22); }
static inline uint32_t S1(uint32_t x) { return ror32(x, 6) ^ ror32(x, 11) ^ ror32(x, 25); }
static inline uint32_t s0(uint32_t x) { return ror32(x, 7) ^ ror32(x, 18) ^ (x >> 3); }
static inline uint32_t s1(uint32_t x) { return ror32(x, 17) ^ ror32(x, 19) ^ (x >> 10); }

static void rdw(const uint8_t *v, uint32_t *w) { for (int k = 0; k < 4; k++) memcpy(w + k, v + 4 * k, 4); }
static void wrw(uint8_t *v, const uint32_t *w) { for (int k = 0; k < 4; k++) memcpy(v + 4 * k, w + k, 4); }

int a64_sha(a64 *s, uint32_t i) {
    unsigned rd = B(i, 4, 0), rn = B(i, 9, 5), rm = B(i, 20, 16);
    if (B(i, 31, 24) != 0x5e) return A64_UNDEF;

    /* two-register: SHA1H, SHA1SU1, SHA256SU0 */
    if (B(i, 23, 17) == 0x14 && B(i, 11, 10) == 2) {
        unsigned opcode = B(i, 16, 12);
        uint32_t d[4], n[4];
        rdw(s->v[rd], d); rdw(s->v[rn], n);
        if (opcode == 0x00) {                       /* SHA1H Sd, Sn — one word, rotated by 30 */
            uint32_t r = rol32(n[0], 30);
            memset(s->v[rd], 0, 16);
            memcpy(s->v[rd], &r, 4);
            return A64_NEXT;
        }
        if (opcode == 0x01) {                       /* SHA1SU1 */
            uint32_t T[4], r[4];
            /* T = Vd EOR (Vn >> 32), as one 128-bit shift */
            T[0] = d[0] ^ n[1]; T[1] = d[1] ^ n[2]; T[2] = d[2] ^ n[3]; T[3] = d[3];
            r[0] = rol32(T[0], 1); r[1] = rol32(T[1], 1); r[2] = rol32(T[2], 1);
            r[3] = rol32(T[3] ^ r[0], 1);           /* the last word folds in the first result */
            wrw(s->v[rd], r);
            return A64_NEXT;
        }
        if (opcode == 0x02) {                       /* SHA256SU0 */
            uint32_t T[4], r[4];
            T[0] = d[1]; T[1] = d[2]; T[2] = d[3]; T[3] = n[0];
            for (int k = 0; k < 4; k++) r[k] = s0(T[k]) + d[k];
            wrw(s->v[rd], r);
            return A64_NEXT;
        }
        return A64_UNDEF;
    }

    /* three-register: SHA1C/P/M, SHA1SU0, SHA256H, SHA256H2, SHA256SU1 */
    if (B(i, 23, 21) == 0 && !BIT(i, 15) && B(i, 11, 10) == 0) {
        unsigned opcode = B(i, 14, 12);
        uint32_t d[4], n[4], m[4];
        rdw(s->v[rd], d); rdw(s->v[rn], n); rdw(s->v[rm], m);

        if (opcode <= 2) {                          /* SHA1C (choose), SHA1P (parity), SHA1M (majority) */
            uint32_t X[4], Y = n[0];
            memcpy(X, d, sizeof X);
            for (int e = 0; e < 4; e++) {
                uint32_t t = opcode == 0 ? ch(X[1], X[2], X[3])
                           : opcode == 1 ? par(X[1], X[2], X[3]) : maj(X[1], X[2], X[3]);
                uint32_t y2 = Y + rol32(X[0], 5) + t + m[e];
                uint32_t x1 = rol32(X[1], 30);
                /* ROL(Y:X, 32) over the 160-bit value: every word moves up one slot and Y wraps in */
                uint32_t nX0 = y2, nX1 = X[0], nX2 = x1, nX3 = X[2];
                Y = X[3];
                X[0] = nX0; X[1] = nX1; X[2] = nX2; X[3] = nX3;
            }
            wrw(s->v[rd], X);
            return A64_NEXT;
        }
        if (opcode == 3) {                          /* SHA1SU0 */
            uint32_t r[4];
            /* result = (Vn<63:0> : Vd<127:64>) EOR Vd EOR Vm */
            r[0] = d[2]; r[1] = d[3]; r[2] = n[0]; r[3] = n[1];
            for (int k = 0; k < 4; k++) r[k] ^= d[k] ^ m[k];
            wrw(s->v[rd], r);
            return A64_NEXT;
        }
        if (opcode == 4 || opcode == 5) {           /* SHA256H (part1) / SHA256H2 */
            int part1 = (opcode == 4);
            uint32_t X[4], Y[4];
            if (part1) { memcpy(X, d, sizeof X); memcpy(Y, n, sizeof Y); }
            else       { memcpy(X, n, sizeof X); memcpy(Y, d, sizeof Y); }
            for (int e = 0; e < 4; e++) {
                uint32_t chs = ch(Y[0], Y[1], Y[2]);
                uint32_t mj = maj(X[0], X[1], X[2]);
                uint32_t t = Y[3] + S1(Y[0]) + chs + m[e];
                uint32_t x3 = t + X[3];
                uint32_t y3 = t + S0(X[0]) + mj;
                /* ROL(Y:X, 32) over the 256-bit pair */
                uint32_t nX[4] = {y3, X[0], X[1], X[2]};
                uint32_t nY[4] = {x3, Y[0], Y[1], Y[2]};
                memcpy(X, nX, sizeof X);
                memcpy(Y, nY, sizeof Y);
            }
            wrw(s->v[rd], part1 ? X : Y);
            return A64_NEXT;
        }
        if (opcode == 6) {                          /* SHA256SU1 */
            /* This shape comes from hardware, not from memory. The obvious reading has T0 built out of Vd and
                  the plain addend out of Vn — the two the other way round — and gives wrong results. Setting
                  one input word at a time on arm64 hardware and reading which output words move gives the
                  dependency structure directly:
                    r0 = sigma1(Vm2) + Vd0 + Vn1      r1 = sigma1(Vm3) + Vd1 + Vn2
                    r2 = sigma1(r0)  + Vd2 + Vn3      r3 = sigma1(r1)  + Vd3 + Vm0
                  i.e. T0 = Vm<31:0> : Vn<127:32>, T1 = Vm<127:64>, and the addend is Vd. */
            uint32_t T0[4], r[4];
            T0[0] = n[1]; T0[1] = n[2]; T0[2] = n[3]; T0[3] = m[0];
            r[0] = s1(m[2]) + d[0] + T0[0];
            r[1] = s1(m[3]) + d[1] + T0[1];
            r[2] = s1(r[0]) + d[2] + T0[2];
            r[3] = s1(r[1]) + d[3] + T0[3];
            wrw(s->v[rd], r);
            return A64_NEXT;
        }
        return A64_UNDEF;
    }
    return A64_UNDEF;
}
