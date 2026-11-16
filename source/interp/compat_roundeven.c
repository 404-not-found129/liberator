/* roundeven/roundevenf for the mac catalyst host build.
 *
 * Why this file exists: the macabi (Mac Catalyst) libSystem does not provide roundeven or
 *    roundevenf - on macOS 27, nearbyint and trunc are present at runtime and these two
 *    are absent. The macOS-platform build links them from libsystem_m, so this is needed only for
 *    the Catalyst host. a64_fpsc.c / a64_simdfp.c use them for arm64 FRINTN.
 * Must not use nearbyint: nearbyint honours the current rounding mode, and the interpreter
 *    shadows FPCR into MXCSR, so the mode is not FE_TONEAREST in general. roundeven is defined as
 *    ties-to-even always, so the tie is resolved explicitly here instead.
 * Checked bit-exactly against libm's own roundeven over the edge cases plus random bit patterns:
 *    0 mismatches of 200013 values.
 */
#include <math.h>

double roundeven(double x) {
    if (!isfinite(x)) return x;                 /* +-inf and NaN pass through unchanged */
    double t = trunc(x);                        /* toward zero; exact */
    double a = fabs(x - t);                     /* exact, < 1 */
    if (a > 0.5) {
        t += (x < 0) ? -1.0 : 1.0;
    } else if (a == 0.5) {
        double half = t * 0.5;                  /* exact; |t|>=2^53 is already even */
        if (half != trunc(half))                /* t odd -> step to the even neighbour */
            t += (x < 0) ? -1.0 : 1.0;
    }
    return t;                                   /* preserves -0.0 for x in (-0.5, -0.0] */
}

float roundevenf(float x) {
    if (!isfinite(x)) return x;
    float t = truncf(x);
    float a = fabsf(x - t);
    if (a > 0.5f) {
        t += (x < 0) ? -1.0f : 1.0f;
    } else if (a == 0.5f) {
        float half = t * 0.5f;
        if (half != truncf(half))
            t += (x < 0) ? -1.0f : 1.0f;
    }
    return t;
}
