// vararg.c — variadic calls across the two ABIs.
//
// The one ABI difference that cannot be papered over. Apple's arm64 ABI passes every variadic argument
//    on the stack, eight bytes each, doubles included. x86-64 SysV passes them in registers: integer-class in
//    rsi, rdx, rcx, r8, r9 then the stack, floating-class in xmm0..xmm7. So interpreted code calling printf
//    has carefully placed its arguments somewhere the native printf will never look, and the call "works"
//    while printing garbage — argc comes out as -1074791576 and a double as nan, while the function's
//    return value is correct, because only the variadic arguments are affected.
// The fix is to know the shape. For the formatter family the format string itself says what the arguments
//    are, so each stack slot is routed to the integer or the floating sequence the callee will read it from.
// A function we do not recognise is not re-marshalled and says so under the trace, rather than guessing a
//    layout: a wrong guess here is silent corruption, and the whole point of this engine is that it is not.
#include <dlfcn.h>
#include "a64i.h"
#include <stdio.h>
#include <string.h>
#define A2X_VTAIL_IMPL
#include "vtail.h"            /* the native-variadic detector objc_bridge.c uses */
#include <stdlib.h>

/* the formatter family, with the zero-based index of the format argument */
static const struct { const char *name; int fmt_index; } VFMT[] = {
    {"printf", 0}, {"vprintf", -1},
    {"fprintf", 1}, {"dprintf", 1}, {"sprintf", 1},
    {"snprintf", 2},
    /* asprintf(char **strp, const char *fmt, ...) — the format is argument 1, not 2. Sharing a
          line with snprintf gives it the wrong index; the fixup then reads x2 as a char* and faults inside
          itself (seen in identityservicesd: faulting address 0x400 == the value in x2). */
    {"asprintf", 1},
    {"syslog", 1}, {"warn", 0}, {"warnx", 0}, {"err", 1}, {"errx", 1},
    /* NSLog takes an NSString OBJECT, not a C string. It is listed here so the index is known,
          and fmt_is_object() below is what makes the format readable. */
    {"NSLog", 0},
    {"scanf", 0}, {"fscanf", 1}, {"sscanf", 1},     /* every argument is a pointer: all integer-class */
    /* The CF formatters need their own rows, or their tail is whatever x3.. holds: diskarbitrationd's
          CFStringCreateWithFormat(NULL, NULL, CFSTR("...%@..."), obj) faulted in objc_opt_respondsToSelector under
          __CFCopyFormattingDescription on a pointer that was never an object. Their format is a CFString (OBJFMT). */
    {"CFStringCreateWithFormat", 2}, {"CFStringAppendFormat", 2}, {"CFLog", 1},
    /* configd died in SCDynamicStoreKeyCreate(alloc, CFSTR("%@/%@"), ...) -> no network, no ssh */
    {"SCDynamicStoreKeyCreate", 1}, {"SCPreferencesPathKeyCreate", 1}, {"SCPrint", 2}, {"SCLog", 2},
    /* the _FORTIFY_SOURCE entries a release build calls instead of snprintf/sprintf, and the _l / c variants */
    {"__snprintf_chk", 4}, {"__sprintf_chk", 3},
    {"snprintf_l", 3}, {"sprintf_l", 2}, {"asprintf_l", 2}, {"printf_l", 1}, {"fprintf_l", 2},
    {"errc", 2}, {"warnc", 1}, {"asl_log", 3}, {"xpc_string_create_with_format", 0},
    /* dirhelper (27 crashes without this row): __os_log_simple(mh, uuid, uuid, subsys, type, fmt, ...) — the format
       is x6, so the tail starts at xs[7] then the overflow buffer. */
    {"_os_log_simple", 6},
    /* logind: DBLoggingLogWithFormat(ctx, CFString fmt, ...) — the format is an object */
    {"DBLoggingLogWithFormat", 1},
    /* CoreUtils' print family, private so the SDK scan never saw it. Without these rows sharingd crash-loops every
       ~11 s in NSAppendPrintF because its tail sits on the arm stack. Format positions read from each function's own x86
       prologue, which saves the registers after the fixed arguments. */
    {"IO8Log", 0}, {"IO8Log2", 1},
    /* 11-05 r14k: CoreUtils' print family, private so the SDK scan never saw it. sharingd crash-looped every ~11 s in
       NSAppendPrintF because its tail sat on the arm stack. Format positions read from each function's own x86 prologue,
       which saves the registers after the fixed arguments. */
    {"NSAppendPrintF", 1}, {"NSAppendPrintF_safe", 1}, {"NSPrintF", 0}, {"NSPrintF_safe", 0}, {"CFAppendPrintF", 1},
    {"AppendPrintF", 1}, {"ASPrintF", 1}, {"SNPrintF", 2}, {"SNPrintF_Add", 2}, {"MemPrintF", 2}, {"CPrintF", 2},
    {"FPrintF", 1}, {"FPrintF_safe", 1}, {"LogPrintF", 3}, {"LogPrintF_safe", 3},
    {0, 0}
};

/* The rest of the family comes from the SDK, not from A list someone remembered. SCDynamicStoreKeyCreate
      (a CF formatter) was missing from the hand list above; AirPort Utility died in __CFAllocatorGetAllocator under
      __CFStringAppendFormatCore because its %@ arguments were read from registers that held garbage. vfmt_gen.h is
      every function the macOS SDK headers declare with a format attribute (printf / scanf / NSString / CFString),
      with the format index and whether the format is an object; interp/genfmt.py makes it by parsing clang's AST,
      never a regex. The hand table wins on a name in both, so no existing entry changes behaviour. A va_list form
      (attribute argument 0) is not in it: those are VALIST's. */
static const struct { const char *name; int fmt_index; int obj; int scan; } VFMTG[] = {
#include "vfmt_gen.h"
    {0, 0, 0, 0}
};
/* the zero-based format index for a name, -1 for a v-form, -2 when the name is not in either table */
static int vfmt_find(const char *n) {
    for (int k = 0; VFMT[k].name; k++) if (!strcmp(VFMT[k].name, n)) return VFMT[k].fmt_index;
    for (int k = 0; VFMTG[k].name; k++) if (!strcmp(VFMTG[k].name, n)) return VFMTG[k].fmt_index;
    return -2;
}

/* classify the conversions in a printf format, in order: 0 = integer-class, 1 = floating-class.
      Only the class matters — the callee itself decides how many bytes to read out of the register. */
static int classify(const char *f, unsigned char *cls, int max) {
    int n = 0;
    for (; *f && n < max; f++) {
        if (*f != '%') continue;
        f++;
        if (*f == '%') continue;
        int is_double = 0, star = 0, extra = 0;
        for (; *f; f++) {
            if (*f == '*') { star++; continue; }                 /* {name}: an os_log annotation when a length or conversion letter follows (%{public}s), else a
                   CoreUtils extension that ends the spec (%#ll{flags}). CoreUtils %{flags} takes the value and a
                   descriptor string: rapportd "Daemon info changed: %#ll{flags}" passes both; counting only one
                   leaves the descriptor out and native PrintFWriteFlags reads 0x80. Every other extension in the
                   298 routes (mask dur error ptr pid xpc DateCF errnod uuid_t) takes one argument. */
            if (strchr("-+ #'0123456789.hlLqjzZt", *f)) continue;
            if (*f == '{') {
                /* unterminated: one argument */
                const char *e = strchr(f, '}');
                if (!e) break;    /* unterminated: one argument, as before r14r */
                if (e[1] && strchr("hlLqjztdiouxXcCsSpfFeEgGaA@", e[1])) { f = e; continue; }
                if (e - f - 1 == 5 && !memcmp(f + 1, "flags", 5)) extra = 1;
                f = e;
                break;
            }
            if (strchr("eEfFgGaA", *f)) is_double = 1;
            break;
        }
        if (!*f) break;
        while (star-- > 0 && n < max) cls[n++] = 0;
        if (n < max) cls[n++] = (unsigned char)is_double;
        while (extra-- > 0 && n < max) cls[n++] = 0;
    }
    return n;
}

/* Returns 1 when the call was re-marshalled. xs/vs are the register arrays the native door will pass. */
/* Which of the family take an object as the format. Kept as a list rather than a flag column so
      the table above stays readable as "name -> which argument is the format". */
static int fmt_is_object(const char *name) {
    static const char *const OBJFMT[] = { "NSLog", "CFStringCreateWithFormat", "CFStringAppendFormat", "CFLog",
                                          "SCDynamicStoreKeyCreate", "SCPreferencesPathKeyCreate", "SCPrint", "SCLog",
                                          "DBLoggingLogWithFormat", 0 };
    for (int k = 0; OBJFMT[k]; k++) if (!strcmp(OBJFMT[k], name)) return 1;
    for (int k = 0; VFMTG[k].name; k++) if (!strcmp(VFMTG[k].name, name)) return VFMTG[k].obj;   /* the SDK's NSString/CFString archetypes */
    return 0;
}

/* Copy an NSString/CFString's characters into buf as UTF-8. Returns 0 when it cannot.
   dlsym, not a link: vararg.c is host-independent C and is compiled for the arm64 truth side too,
      where a CoreFoundation link would not be wanted. A missing symbol is a refusal, never a guess. */
static int cfstring_to_c(const void *obj, char *buf, unsigned long len) {
    typedef unsigned char (*getcstr_t)(const void *, char *, long, unsigned int);
    static getcstr_t f;
    static int tried;
    if (!tried) { tried = 1; f = (getcstr_t)dlsym(RTLD_DEFAULT, "CFStringGetCString"); }
    if (!f) return 0;
    buf[0] = 0;
    return f(obj, buf, (long)len, 0x08000100u /* A va_list handed to native code must not be the raw arm64 pointer. Apple arm64 va_list is a `char *` into the
      8-byte-slot argument area; x86-64 SysV va_list is a pointer to {gp_offset, fp_offset, overflow_arg_area,
      reg_save_area}. Native vsnprintf / CFStringCreateWithFormatAndArguments read the first arm64 argument as two
      offsets and the second as the overflow pointer — Music and TV died inside CFStringCreateWithFormatAndArguments.
   The fix is the one the per-app bind (arm2x86.py va_conv) and objc_bridge.c arm_va2 already use: gp_offset 48 and
      fp_offset 176 say "no register arguments left", so every va_arg reads the overflow area, which is the arm64 slot
      area (8 bytes per argument on both sides). Table: the 53 VALIST rows of a coverage sweep; in every one the
      va_list is the last fixed argument. Returns 1 when it swapped a register. */) && buf[0] != 0;
}

/* A va_list handed to native code was the raw arm64 POINTER. Apple arm64 va_list is a `char *` into the
      8-byte-slot argument area; x86-64 SysV va_list is a pointer to {gp_offset, fp_offset, overflow_arg_area,
      reg_save_area}. Native vsnprintf / CFStringCreateWithFormatAndArguments read the first arm64 argument as two
      offsets and the second as the overflow pointer — Music and TV died inside CFStringCreateWithFormatAndArguments.
   The fix is the one the per-app bind (arm2x86.py va_conv) and objc_bridge.c arm_va2 already use: gp_offset 48 and
      fp_offset 176 say "no register arguments left", so every va_arg reads the overflow area, which is the arm64 slot
      area (8 bytes per argument on both sides). Table: the 53 VALIST rows of the 10-27 coverage sweep; in every one the
      va_list is the last fixed argument. Returns 1 when it swapped a register. */
extern FILE *a2x_errf(void);
struct a64_x86va { unsigned gp_offset, fp_offset; void *overflow_arg_area, *reg_save_area; };
static const struct { const char *name; int idx; } VALIST[] = {
    { "CFAppendPrintV", 2 },
    { "CFStringAppendFormatAndArguments", 3 },
    { "CFStringCreateStringWithValidatedFormatAndArguments", 5 },
    { "CFStringCreateWithFormatAndArguments", 3 },
    { "CGColorConversionInfoCreateFromListWithArguments", 4 },
    { "LogPrintV", 4 },
    { "NSAppendPrintV", 2 },
    { "NSErrorV", 3 },
    { "NSLogv", 1 },
    { "NSPrintV", 1 },
    { "Tcl_AppendResultVA", 1 },   /* r15b census */
    { "Tcl_AppendStringsToObjVA", 1 },   /* r15b census */
    { "Tcl_PanicVA", 1 },   /* r15b census */
    { "Tcl_SetErrorCodeVA", 1 },   /* r15b census */
    { "Tcl_VarEvalVA", 1 },   /* r15b census */
    { "VASPrintF", 2 },
    { "VCPrintF", 3 },
    { "VFPrintF", 2 },
    { "VSNPrintF", 3 },
    { "__svfscanf", 2 },
    { "__vsnprintf_chk", 5 },
    { "__vsprintf_chk", 4 },
    /* stock 27 launchd (PID 1) formats its log lines with _simple_vsnprintf: unconverted, the native
       side read the arm64 va_list as an x86 one and faulted in __simple_bprintf (initproc exit 139). */
    { "_simple_vdprintf", 2 },
    { "_simple_vesprintf", 3 },
    { "_simple_vsnprintf", 3 },
    { "_simple_vsprintf", 2 },
    { "asl_vlog", 4 },
    { "clSetKernelArgsVaListAPPLE", 2 },   /* r15b census */
    { "com_err_va", 3 },   /* r15b census */
    { "curl_mvaprintf", 1 },   /* r15b census */
    { "curl_mvfprintf", 2 },   /* r15b census */
    { "curl_mvprintf", 1 },   /* r15b census */
    { "curl_mvsnprintf", 3 },   /* r15b census */
    { "curl_mvsprintf", 2 },   /* r15b census */
    { "dispatch_debugv", 2 },
    { "dsBuildListFromStringsAllocV", 3 },   /* r15b census */
    { "ippAddStringfv", 6 },
    { "ippSetStringfv", 4 },
    { "krb5_build_principal_alloc_va", 4 },   /* r15b census */
    { "krb5_build_principal_va", 4 },   /* r15b census */
    { "krb5_vset_error_message", 3 },   /* r15b census */
    { "pam_verror", 2 },   /* r15b census */
    { "pam_vinfo", 2 },   /* r15b census */
    { "pam_vprompt", 4 },   /* r15b census */
    { "snmp_vlog", 2 },   /* r15b census */
    { "sqlite3_vmprintf", 1 },   /* r15b census */
    { "vAEBuildAppleEvent", 10 },
    { "vAEBuildDesc", 3 },
    { "vAEBuildParameters", 3 },
    { "vasprintf", 2 },
    { "vasprintf_l", 3 },
    { "vasxprintf", 4 },   /* r15b census */
    { "vasxprintf_exec", 2 },   /* r15b census */
    { "vdprintf", 2 },
    { "vdprintf_l", 3 },
    { "vdxprintf", 4 },   /* r15b census */
    { "vdxprintf_exec", 2 },   /* r15b census */
    { "verr", 2 },
    { "verrc", 3 },
    { "verrx", 2 },
    { "vfprintf", 2 },
    { "vfprintf_l", 3 },
    { "vfscanf", 2 },
    { "vfscanf_l", 3 },
    { "vfwprintf", 2 },
    { "vfwprintf_l", 3 },
    { "vfwscanf", 2 },
    { "vfwscanf_l", 3 },
    { "vfxprintf", 4 },   /* r15b census */
    { "vfxprintf_exec", 2 },   /* r15b census */
    { "vprintf", 1 },
    { "vprintf_l", 2 },
    { "vscanf", 1 },
    { "vscanf_l", 2 },
    { "vsnprintf", 3 },
    { "vsnprintf_l", 4 },
    { "vsprintf", 2 },
    { "vsprintf_l", 3 },
    { "vsscanf", 2 },
    { "vsscanf_l", 3 },
    { "vswprintf", 3 },
    { "vswprintf_l", 4 },
    { "vswscanf", 2 },
    { "vswscanf_l", 3 },
    { "vsxprintf", 5 },   /* r15b census */
    { "vsxprintf_exec", 3 },   /* r15b census */
    { "vsyslog", 2 },
    { "vwarn", 1 },
    { "vwarnc", 2 },
    { "vwarnx", 1 },
    { "vwprintf", 1 },
    { "vwprintf_l", 2 },
    { "vwscanf", 1 },
    { "vwscanf_l", 2 },
    { "vxprintf", 3 },   /* r15b census */
    { "vxprintf_exec", 1 },   /* r15b census */
    { "xmlStrVPrintf", 3 },   /* r15b census */
    { "xmlTextWriterWriteVFormatAttribute", 3 },   /* r15b census */
    { "xmlTextWriterWriteVFormatAttributeNS", 5 },   /* r15b census */
    { "xmlTextWriterWriteVFormatCDATA", 2 },   /* r15b census */
    { "xmlTextWriterWriteVFormatComment", 2 },   /* r15b census */
    { "xmlTextWriterWriteVFormatDTD", 5 },   /* r15b census */
    { "xmlTextWriterWriteVFormatDTDAttlist", 3 },   /* r15b census */
    { "xmlTextWriterWriteVFormatDTDElement", 3 },   /* r15b census */
    { "xmlTextWriterWriteVFormatDTDInternalEntity", 4 },   /* r15b census */
    { "xmlTextWriterWriteVFormatElement", 3 },   /* r15b census */
    { "xmlTextWriterWriteVFormatElementNS", 5 },   /* r15b census */
    { "xmlTextWriterWriteVFormatPI", 3 },   /* r15b census */
    { "xmlTextWriterWriteVFormatRaw", 2 },   /* r15b census */
    { "xmlTextWriterWriteVFormatString", 2 },   /* r15b census */
    { "xpc_string_create_with_format_and_arguments", 1 },
};
int a64_valist_fixup(const char *name, unsigned long *xs, struct a64_x86va *va) {
    const char *n = name[0] == '_' ? name + 1 : name;
    unsigned lo = 0, hi = sizeof VALIST / sizeof VALIST[0];
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(VALIST[mid].name, n);
        if (c == 0) {
            int i = VALIST[mid].idx;
            if (i >= 8) {
                /* the va_list itself was passed on the arm64 stack; not rebuilt, and said so */
                fprintf(a2x_errf(), "[valist] %s: va_list is argument %d (on the stack), passed through unconverted\n", n, i);
                return 0;
            }
            if (!xs[i]) return 0;                      /* CFGregorianDate crosses by value, and the two ABIs split it differently. The struct is
      {SInt32 year; SInt8 month, day, hour, minute; double second} = 16 bytes. arm64 carries it in two integer
      registers (x0 = year..minute, x1 = the bits of `second`); x86-64 SysV classifies the second eightbyte SSE, so
      it is rdi + xmm0, and the next argument moves up to rsi. Without this, Music and TV hand the raw x1 (5.0 =
      0x4014000000000000) to CFGregorianDateGetAbsoluteTime as the time zone and die in objc_msgSend on it.
   Both directions are here because both are imported (Music, TV): GetAbsoluteTime takes it, GetGregorianDate
      returns it (arm64 x0:x1, x86 rax:xmm0). Returns 1 when it made the call; xs/vs then hold the arm64 result. */
            va->gp_offset = 48; va->fp_offset = 176;
            va->overflow_arg_area = (void *)(uintptr_t)xs[i]; va->reg_save_area = 0;
            xs[i] = (unsigned long)(uintptr_t)va;
            return 1;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}

/* CFGregorianDate crosses by value, and the two ABIs split it differently. The struct is
      {SInt32 year; SInt8 month, day, hour, minute; double second} = 16 bytes. arm64 carries it in TWO INTEGER
      registers (x0 = year..minute, x1 = the bits of `second`); x86-64 SysV classifies the second eightbyte SSE, so
      it is rdi + xmm0, and the next argument moves up to rsi. Measured: Music and TV handed the raw x1 (5.0 =
      0x4014000000000000) to CFGregorianDateGetAbsoluteTime as the time zone and died in objc_msgSend on it.
   Both directions are here because both are imported (Music, TV): GetAbsoluteTime takes it, GetGregorianDate
      returns it (arm64 x0:x1, x86 rax:xmm0). Returns 1 when it made the call; xs/vs then hold the arm64 result. */
struct a64_cfgdate { int32_t year; int8_t month, day, hour, minute; double second; };
int a64_cfgreg_door(const char *name, unsigned long tgt, unsigned long *xs, unsigned char *vs) {
    const char *n = name[0] == '_' ? name + 1 : name;
    if (!strcmp(n, "CFGregorianDateGetAbsoluteTime")) {
        struct a64_cfgdate g;
        memcpy(&g, &xs[0], 8); memcpy(&g.second, &xs[1], 8);
        double r = ((double (*)(struct a64_cfgdate, const void *))(uintptr_t)tgt)(g, (const void *)(uintptr_t)xs[2]);
        memset(vs, 0, 16); memcpy(vs, &r, 8);
        return 1;
    }
    if (!strcmp(n, "CFAbsoluteTimeGetGregorianDate")) {
        double at; memcpy(&at, vs, 8);
        struct a64_cfgdate g = ((struct a64_cfgdate (*)(double, const void *))(uintptr_t)tgt)(at, (const void *)(uintptr_t)xs[0]);
        memcpy(&xs[0], &g, 8); memcpy(&xs[1], &g.second, 8);
        return 1;
    }
    return 0;
}

/* An indirect result is x8 on arm64 and rdi on x86, which shifts every integer argument. airportd:
      NSEnumerateMapTable(table) got rdi = x0 = the table as its result buffer and rsi = x1 = leftover junk as the
      table; x1 held an x86 function, so objc_msgSend read `55 48 89 e5 53 50` as an isa (fault 0x5053e5894868).
      sret_gen.h is generated from the binaries, not remembered: every function imported by an on-disk arm64e binary
      whose real 27.0 arm64e body reads x8 before writing it. Control: 0 of 2,209 typed scalar-return functions read x8. */
/* An indirect result is x8 ON ARM64 AND rdi ON x86, which shifts every INTEGER argument. airportd:
      NSEnumerateMapTable(table) got rdi = x0 = the table as its result buffer and rsi = x1 = leftover junk as the
      table; x1 held an x86 function, so objc_msgSend read `55 48 89 e5 53 50` as an isa (fault 0x5053e5894868).
      sret_gen.h is measured, not remembered: every function imported by an on-disk arm64e binary whose real 27.0
      arm64e body reads x8 before writing it. Ruler: 0 of 2,209 typed scalar-return functions read x8. */
static const char *const A64_SRET[] = {
#include "sret_gen.h"
};
int a64_native_is_sret(const char *name) {
    if (!name || A2X_DBGENV("ARM2X86_NO_SRETDOOR")) return 0;
    for (int t = 0; t < 2; t++) {
        const char *n = name + t;
        if (t && name[0] != '_') break;
        unsigned lo = 0, hi = sizeof A64_SRET / sizeof A64_SRET[0];
        while (lo < hi) {
            unsigned mid = (lo + hi) / 2;
            int c = strcmp(A64_SRET[mid], n);
            if (!c) return 1;
            if (c < 0) lo = mid + 1; else hi = mid;
        }
    }
    return 0;
}

#define A64_VA_OVF_MAX 64
__thread unsigned long a64_va_ovf_buf[A64_VA_OVF_MAX];
__thread const unsigned long *a64_va_ovf;
/* fixed-arity variadics whose tail sits on the arm stack; file scope so a64_door_flags reads the same rows */
static const struct { const char *name; int nfixed, nvar; } VFIX[] = {
    {"open", 2, 1}, {"openat", 3, 1}, {"fcntl", 2, 1}, {"ioctl", 2, 1},
    {"shm_open", 2, 1}, {"sem_open", 2, 2}, {"semctl", 3, 1},
    {"open_dprotected_np", 4, 1}, {"openat_dprotected_np", 5, 1},
    /* class scan: callees whose prologue saves no register before the al test, so the prologue reader cannot count
       their fixed arguments. sandbox_check: logd, logd_helper, launchctl, log, apsd, wifip2pd read the path filter from
       x3, which held junk (arm64 -1, native 0). execlp/execle: sshd-session, ssh, ssh-agent, ssh-keygen, edquota —
       nvar -1 = a NULL-terminated list, the 0 moves too. */
    {"sandbox_check_by_audit_token", 3, 3},
    /* Which of the fixups above can possibly apply to this callee, decided from the name alone. Asking every one
      of them on every native call, each strcmp-ing the name against a table, made a plain strlen cost
      ~100 string compares (34% of the interpreter's samples). The name is fixed for an address,
      so the verdict is computed once per target and cached (a64_rt.c, door_lookup).
      bit0 va_list argument · bit1 CFGregorianDate by value · bit2 variadic (format, fixed-arity or object list).
   Each test mirrors the one inside the function it guards; a row added there must be added here.
      bit3 8-bit return: zero-extend al on the untyped door (narrow8.h, below). */
    {"sandbox_check", 3, 3}, {"reboot3", 1, 1}, {"execlp", 2, -1}, {"execle", 2, -1},
    {0, 0, 0}
};

/* Which of the FIXUPS above can possibly apply to this callee, decided from the name alone. The interpreter
      asked every one of them on every native call, and each answers by strcmp-ing the name against a table,
      so a plain strlen paid for ~100 string compares (34% of its samples). The name is fixed for an address,
      so the verdict is computed once per target and cached (a64_rt.c, door_lookup).
      bit0 va_list argument · bit1 CFGregorianDate by value · bit2 variadic (format, fixed-arity or object list).
   Each test mirrors the one inside the function it guards; a row added there must be added here.
      bit3 8-bit return: zero-extend al on the untyped door (narrow8.h, below). */
#include "narrow8.h"
unsigned a64_door_flags(const char *name) {
    if (!name) return 0;
    const char *n = name[0] == '_' ? name + 1 : name;
    unsigned f = 0;
    unsigned lo = 0, hi = sizeof VALIST / sizeof VALIST[0];
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(VALIST[mid].name, n);
        if (c == 0) { f |= 1; break; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    if (!strcmp(n, "CFGregorianDateGetAbsoluteTime") || !strcmp(n, "CFAbsoluteTimeGetGregorianDate")) f |= 2;
    /* a64_variadic_fixup strips one underscore and then compares against the names below */
    const char *v = name[0] == '_' ? name + 1 : name;
    if (!strcmp(v, "syslog$DARWIN_EXTSN")) v = "syslog";
    if (!strcmp(v, "_NSDictionaryOfVariableBindings") || !strcmp(v, "NSDictionaryOfVariableBindings")) f |= 4;
    size_t bl = strcspn(v, "$");
    for (int k = 0; VFIX[k].name; k++)
        if (strlen(VFIX[k].name) == bl && !strncmp(VFIX[k].name, v, bl)) f |= 4;
    if (!strcmp(v, "ICKeyPathFromSelectors")) f |= 4;
    if (vfmt_find(v) >= 0) f |= 4;
    /* A private BOOL comes back in al with garbage above it. Native _ISProcessIsRoot ends
          mov eax,[mem]; or al,cl; and al,1 — as uid 501 rax=0x2000, al=0. iconservicesagent's
          listener:shouldAcceptNewConnection: does `bl __ISProcessIsRoot; cbz w0` on all of w0, takes "root",
          then rejects every client above uid 500: no icon is ever rendered fresh and Safari's is blank.
          Same class as PRIVATE_BOOL_SPI (mksigtable.py) but for functions sigdb cannot type at all; the rows
          come from their own x86 code (retwidth.py), ruled against 2,050 typed functions with 0 int misreads. */
    lo = 0; hi = sizeof NARROW8 / sizeof NARROW8[0];
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(NARROW8[mid], n);
        if (c == 0) { f |= 8; break; }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return f;
}

int a64_variadic_fixup(const char *name, const a64 *s, unsigned long *xs, unsigned char *vs) {
    if (!name) return 0;
    if (name[0] == '_') name++;
    /* Keep the actual native target; only its known variadic signature aliases. */
    if (!strcmp(name, "syslog$DARWIN_EXTSN")) name = "syslog";
    /* A NIL-terminated object list has no format, but it has A TERMINATOR. macOS 27 Calendar died at
          start-up: "NSDictionaryOfVariableBindings failed because either one of the values is nil" for
          `firstDay, lastDay`. The macro is _NSDictionaryOfVariableBindings(@"a, b", a, b, nil): the first value
          is a fixed argument (x1) and the rest, nil included, sat on the arm stack where x86 never looks.
          Every one of them is an object, so they move slot for slot into the integer sequence after x1.
       Bounded by the six integer slots the door carries after the two fixed arguments. A longer list is
          refused out loud, never truncated. */
    if (!strcmp(name, "_NSDictionaryOfVariableBindings") || !strcmp(name, "NSDictionaryOfVariableBindings")) {
        const unsigned long *ol = (const unsigned long *)(uintptr_t)s->sp;
        int k = 0;
        while (k < 6 && ol[k]) k++;
        if (k == 6) {
            fprintf(stderr, "[xlat2] interp: %s object list longer than 6 values: NOT re-marshalled\n", name);
            return 0;
        }
        for (int j = 0; j <= k; j++) xs[2 + j] = ol[j];   /* A nil-terminated selector list. ICKeyPathFromSelectors(SEL first, ..., nil) (NotesSupport) joins the
          selector names with dots. The tail sits on the arm stack and x86 reads rsi.. instead, so the list never ends at
          the nil: the Notes app built "noteEditorController.setupAccessibility.<junk>" and KVO threw NSUnknownKeyException.
          Same shape as the object list above, one fixed argument (x0), five integer slots after it (rsi..r9).
       A longer list is refused out loud, never truncated. */
        return 1;
    }
    /* A NIL-terminated selector list. ICKeyPathFromSelectors(SEL first, ..., nil) (NotesSupport) joins the
          selector names with dots. The tail sat on the arm stack, x86 read rsi.. instead, so the list never ended at
          the nil: Notes built "noteEditorController.setupAccessibility.<junk>" and KVO threw NSUnknownKeyException.
          Same shape as the object list above, one fixed argument (x0), five integer slots after it (rsi..r9).
       A longer list is refused out loud, never truncated. */
    if (!strcmp(name, "ICKeyPathFromSelectors")) {
        const unsigned long *ol = (const unsigned long *)(uintptr_t)s->sp;
        int k = 0;
        while (k < 5 && ol[k]) k++;
        if (k == 5) {
            fprintf(stderr, "[xlat2] interp: %s selector list longer than 5: NOT re-marshalled\n", name);
            return 0;
        }
        for (int j = 0; j <= k; j++) xs[1 + j] = ol[j];   /* A variadic with no format still has its tail on the arm stack. ioctl/fcntl/open need moving too:
          with only format strings handled, their third argument stays at sp[0] while x86 reads
          whatever is in x2. diskarbitrationd ioctl(fd, DKIOCGETBLOCKSIZE, &bs) wrote through that junk
          pointer -> nanov2 guard corruption in strdup on an NSXPC reply; under strict-size Guard Malloc it faulted
          in memmove on a 16-byte buffer the code had sized from bs. Every fcntl(F_SETFL), open(O_CREAT, mode) and
          ioctl in every guest took the same junk.
       Apple arm64 puts every variadic argument in its own 8-byte stack slot, even with registers free, so the
          slots move into the integer registers after the fixed ones. Reading a slot the caller did not pass is
          harmless (it is the caller's stack) and the callee ignores it. */
        return 1;
    }
    /* A variadic with no format still has its tail on the ARM stack. ioctl/fcntl/open were "left to
          vararg.c" and vararg.c only knew format strings, so their third argument stayed at sp[0] while x86 read
          whatever was in x2. Measured: diskarbitrationd ioctl(fd, DKIOCGETBLOCKSIZE, &bs) wrote through that junk
          pointer -> nanov2 guard corruption in strdup on an NSXPC reply; under strict-size Guard Malloc it faulted
          in memmove on a 16-byte buffer the code had sized from bs. Every fcntl(F_SETFL), open(O_CREAT, mode) and
          ioctl in every guest took the same junk.
       Apple arm64 puts every variadic argument in its own 8-byte stack slot, even with registers free, so the
          slots move into the integer registers after the fixed ones. Reading a slot the caller did not pass is
          harmless (it is the caller's stack) and the callee ignores it. */
    {
        size_t bl = strcspn(name, "$");           /* open$NOCANCEL, fcntl$NOCANCEL take the same arguments */
        for (int k = 0; VFIX[k].name; k++) {
            if (strlen(VFIX[k].name) != bl || strncmp(VFIX[k].name, name, bl)) continue;
            const unsigned long *va = (const unsigned long *)(uintptr_t)s->sp;
            int nv = VFIX[k].nvar;
            if (nv == -1) { nv = 0; while (nv < A64_VA_OVF_MAX + 8 && va[nv]) nv++; nv++; }   /* ... up to and with the 0 */
            /* x6, x7 are the callee's first two stack words (native_door.ll: 8 integers, then 16 stack words) — the
               format path fills them the same way; the old cap of six registers dropped them silently */
            if (VFIX[k].nfixed + nv > 8 + A64_VA_OVF_MAX) {
                fprintf(stderr, "[xlat2] interp: %s list of %d slot(s) is longer than the overflow area: NOT re-marshalled\n",
                        name, nv);
                return 0;
            }
            int novf = 0;
            for (int j = 0; j < nv; j++) {
                if (VFIX[k].nfixed + j < 8) xs[VFIX[k].nfixed + j] = va[j];
                else a64_va_ovf_buf[novf++] = va[j];
            }
            if (novf) a64_va_ovf = a64_va_ovf_buf;
            static int tf = -1;
            if (tf < 0) tf = A2X_DBGENV("ARM2X86_TRACE_INTERP") ? 1 : 0;
            if (tf) fprintf(stderr, "[xlat2] interp: %s fixed-arity variadic, %d slot(s) off the arm stack\n",
                            name, VFIX[k].nvar);
            return 1;
        }
    }
    int fi = vfmt_find(name);
    if (fi < 0) return 0;                        /* unknown, or a v-form that takes a va_list instead */

    /* Some of this family take the format as an NSString/CFString object. Read its characters out
          before classifying; the pointer itself is an object and scanning it finds nothing real. */
    char objfmt[1024];
    const char *fmt;
    if (fmt_is_object(name)) {
        const void *o = (const void *)(uintptr_t)s->x[fi];
        if (!o || !cfstring_to_c(o, objfmt, sizeof objfmt)) return 0;   /* never guess a layout */
        fmt = objfmt;
    } else {
        fmt = (const char *)(uintptr_t)s->x[fi];
        if (!fmt) return 0;
    }

    unsigned char cls[32];
    int n = classify(fmt, cls, 32);
    if (!n) return 1;                            /* a format with no conversions needs no arguments */

    /* The fixed arguments stay where they are and the variadic ones come off the arm stack in order.
          Apple's arm64 ABI starts the variadic area at sp, with each argument in its own 8-byte slot. */
    const unsigned long *va = (const unsigned long *)(uintptr_t)s->sp;
    int ngp = fi + 1;                            /* the next integer-class register slot to fill */
    int nsse = 0;
    int novf = 0;
    for (int k = 0; k < n; k++) {
        unsigned long slot = va[k];
        if (cls[k]) {
            if (nsse < 8) { memset(vs + 16 * nsse, 0, 16); memcpy(vs + 16 * nsse, &slot, 8); nsse++; }
            else if (novf < A64_VA_OVF_MAX) a64_va_ovf_buf[novf++] = slot;
        } else {
            if (ngp < 8) xs[ngp++] = slot;
            else if (novf < A64_VA_OVF_MAX) a64_va_ovf_buf[novf++] = slot;
        }
    }
    if (novf) a64_va_ovf = a64_va_ovf_buf;
    static int tr = -1;
    if (tr < 0) tr = A2X_DBGENV("ARM2X86_TRACE_INTERP") ? 1 : 0;
    if (tr) fprintf(stderr, "[xlat2] interp: %s variadic, %d args (%d integer, %d floating) off the arm stack\n",
                    name, n, ngp - fi - 1, nsse);
    return 1;
}

/* Private byte-return SPI lacks SDK declarations. Preserve its actual return width
   when the untyped C door receives unspecified high bits of the Intel register. */
int a64_native_return_is_u8(const char *name) {
    if (!name) return 0;
    const char *n = name[0] == '_' ? name + 1 : name;
    unsigned lo = 0, hi = sizeof NARROW8 / sizeof NARROW8[0];
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        int c = strcmp(NARROW8[mid], n);
        if (!c) return 1;
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return 0;
}
