// a64_fp.c — the split between scalar floating point and Advanced SIMD inside Arm ARM group x111.
#include "a64_priv.h"

int a64_fp(a64 *s, uint32_t i) {
    /* Three FAMILIES share this group and the manual separates them on three bits, not on the opcode:
          bit 28 = 0            -> Advanced SIMD, vector form   (bits 28:24 = 01110 / 01111)
          bits 31:30 = 01       -> Advanced SIMD, SCALAR form   (bits 28:24 = 11110 / 11111)
          bit 30 = 0            -> scalar floating point        (bit 31 is M, or sf on the conversions)
       `add d0, d1, d2` and `fadd d0, d1, d2` differ only in bit 30 and bits 15:10. Routing on the opcode
          field instead sent the first one into the FP decoder, where it is a reserved 2-source opcode. */
    /* The CRYPTO groups share this slot, and they are named by their whole top byte — 0xce for the SHA3
          and SHA512 families, 0x4e for AES.
       NOT by bit 31 alone: that bit is `sf` on the scalar conversions, so routing on it sent every
          64-bit fcvtzs/scvtf/ucvtf/fmov to the crypto decoder and 15 working forms reported themselves
          unimplemented. The certifier's own unimplemented list caught it on the next run. */
    if (B(i, 31, 24) == 0xce
        || (B(i, 31, 24) == 0x4e && B(i, 23, 22) == 0 && B(i, 21, 17) == 0x14 && B(i, 11, 10) == 2)) {
        int r = a64_crypto(s, i);
        if (r != A64_UNDEF) return r;
    }
    if (B(i, 31, 24) == 0x5e) {
        int r = a64_sha(s, i);                    /* SHA-1 / SHA-256 share the scalar-SIMD slot */
        if (r != A64_UNDEF) return r;
    }
    if (!BIT(i, 28)) return a64_simd(s, i);
    if (BIT(i, 30) && !BIT(i, 31)) return a64_simd(s, i);
    return a64_fpsc(s, i);
}
