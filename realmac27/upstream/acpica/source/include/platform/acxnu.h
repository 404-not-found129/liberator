/*
 * acxnu.h - ACPICA platform header for the XNU (macOS) x86_64 kernel.
 * Freestanding: no userspace headers; libkern supplies the C string routines.
 */
#ifndef __ACXNU_H__
#define __ACXNU_H__

#include <stdint.h>
#include <stdarg.h>
#include <string.h>

/*
 * The kernel string.h turns strcpy/strcat/strncpy/strncat into compile errors (deprecated, unbounded).
 * ACPICA only copies into buffers it sized itself; these keep its semantics without the unbounded libkern calls.
 */
#undef strcpy
#undef strcat
#undef strncpy
#undef strncat
static inline char *AcpiXnuStrcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }
static inline char *AcpiXnuStrcat(char *d, const char *s) { AcpiXnuStrcpy(d + strlen(d), s); return d; }
static inline char *AcpiXnuStrncpy(char *d, const char *s, size_t n)
{ size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; for (; i < n; i++) d[i] = 0; return d; }
static inline char *AcpiXnuStrncat(char *d, const char *s, size_t n)
{ char *e = d + strlen(d); size_t i = 0; for (; i < n && s[i]; i++) e[i] = s[i]; e[i] = 0; return d; }
#define strcpy(d, s)        AcpiXnuStrcpy((d), (s))
#define strcat(d, s)        AcpiXnuStrcat((d), (s))
#define strncpy(d, s, n)    AcpiXnuStrncpy((d), (s), (n))
#define strncat(d, s, n)    AcpiXnuStrncat((d), (s), (n))

/* No ctype.h in the kernel: ASCII-only classification, which is all ACPI names and AML strings use. */
static inline int AcpiXnuIsdigit(int c)  { return c >= '0' && c <= '9'; }
static inline int AcpiXnuIsxdigit(int c) { return AcpiXnuIsdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int AcpiXnuIsspace(int c)  { return c == ' ' || (c >= '\t' && c <= '\r'); }
static inline int AcpiXnuIsprint(int c)  { return c >= 0x20 && c < 0x7f; }
static inline int AcpiXnuIsupper(int c)  { return c >= 'A' && c <= 'Z'; }
static inline int AcpiXnuIslower(int c)  { return c >= 'a' && c <= 'z'; }
static inline int AcpiXnuIsalpha(int c)  { return AcpiXnuIsupper(c) || AcpiXnuIslower(c); }
static inline int AcpiXnuToupper(int c)  { return AcpiXnuIslower(c) ? c - 32 : c; }
static inline int AcpiXnuTolower(int c)  { return AcpiXnuIsupper(c) ? c + 32 : c; }
#define isdigit  AcpiXnuIsdigit
#define isxdigit AcpiXnuIsxdigit
#define isspace  AcpiXnuIsspace
#define isprint  AcpiXnuIsprint
#define isupper  AcpiXnuIsupper
#define islower  AcpiXnuIslower
#define isalpha  AcpiXnuIsalpha
#define toupper  AcpiXnuToupper
#define tolower  AcpiXnuTolower

#define ACPI_MACHINE_WIDTH          64
#define COMPILER_DEPENDENT_INT64    long long
#define COMPILER_DEPENDENT_UINT64   unsigned long long
#define ACPI_USE_DO_WHILE_0
#define ACPI_USE_NATIVE_DIVIDE
#define ACPI_USE_NATIVE_MATH64
#define ACPI_USE_SYSTEM_CLIBRARY
#define ACPI_USE_LOCAL_CACHE
#define ACPI_INLINE                 inline
#define ACPI_FLUSH_CPU_CACHE()      __asm__ __volatile__("wbinvd" ::: "memory")

/* The OS layer (our osl) implements locks and semaphores on IOLock / IOSimpleLock / semaphore_t. */
#define ACPI_MUTEX_TYPE             ACPI_OSL_MUTEX

#include "acgcc.h"

#endif /* __ACXNU_H__ */
