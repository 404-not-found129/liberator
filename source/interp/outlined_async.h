/* Recognize only the compiler's outlined async-frame prologue.
   The reader must reject words outside executable guest sections/function bounds. */
#include <stdint.h>
typedef int (*a2x_async_word_reader)(unsigned long, unsigned int *);
static int a2x_outlined_async(unsigned long pc, a2x_async_word_reader readword, unsigned depth) {
    unsigned int a, b, c;
    if (depth >= 2 || !readword(pc, &a)) return 0;
    if (a == 0xb24403bdu) return 1;
    if (a != 0xd503237fu || pc > ~0UL - 8 || !readword(pc + 4, &b)) return 0;
    if (b == 0xb24403bdu) return 1;
    if (b != 0xaa1e03e0u || !readword(pc + 8, &c) || (c & 0xfc000000u) != 0x94000000u) return 0;
    long disp = (long)(int32_t)(c << 6) >> 4;
    unsigned long branch = pc + 8;
    if ((disp > 0 && branch > ~0UL - (unsigned long)disp) ||
        (disp < 0 && branch < (unsigned long)(-disp))) return 0;
    return a2x_outlined_async(branch + disp, readword, depth + 1);
}
