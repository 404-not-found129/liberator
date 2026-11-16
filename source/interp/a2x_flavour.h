/* a2x_flavour.h - the first thing every interpreter source sees (the build scripts pass it with -include).
 *
 * One source tree builds two flavours. The default one keeps every diagnostic: switches read from the environment,
 * traces, stage lines, dumps. The shipped one (A2X_SHIP=1) has none of them: a switch read is the constant "unset"
 * at compile time, so its name and the code it guards are not in the binary, and a diagnostic print is nothing at
 * all. What a shipped interpreter writes is the stop record in stop_record.c, which does not go through fprintf. */
#ifndef A2X_FLAVOUR_H
#define A2X_FLAVOUR_H
#ifndef A2X_SHIP
#define A2X_SHIP 0
#endif
#include <stdio.h>
#include <stdlib.h>
/* the stop record (always on, both flavours): the reason an internal stop gives before it ends the program */
#ifdef __cplusplus
extern "C"
#endif
void a2x_stop(const char *reason, const char *detail);
#if A2X_SHIP
#define A2X_DBGENV(name) ((char *)0)
#define fprintf(...) ((void)0)
#else
#define A2X_DBGENV(name) getenv(name)
#endif
#endif
