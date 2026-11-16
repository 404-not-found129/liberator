/* The two facts interp/a64_rt.c asks the host for, answered inside a translated image.
 *
 * The link was broken for every translated program, and the gate said so first. The generic
 *    host (interp/host.c) binds every import itself, so it knows which address got which symbol and
 *    which ones it routed through the register-file bridge; it answers both questions from its own
 *    tables. a64_rt.c calls them unconditionally, and xlat2 links a64_rt.c into every program — but
 *    not host.c, which cannot be linked (it owns `main` and redefines __arm2x86_stack_args / _cbarg /
 *    _svc). The result: `Undefined symbols ... _a64_is_rf_callee, _a64_native_name,
 *    _a64_variadic_fixup`, so nothing translated linked, including every certifier ruler. Same shape as
 *    the sysreg_rt.c omission of 10-11: the definitions existed on disk and were in no link line.
 *
 * Both definitions are weak, so the generic host's richer ones win whenever host.c is what is being
 *    linked. Nothing here is a fallback that guesses: each answer comes from a fact this link owns.
 */
#include <dlfcn.h>
#include <stdint.h>

/* Which native addresses are bridge wrappers, per image. A wrapper takes no arguments and returns
 *    nothing: it reads x0..x8/v0..v7 out of __arm2x86_rf and writes its result back there, so the
 *    interpreter must enter it through the register file instead of the native adapter.
 *    interp/rf_table.h cannot answer this in a translated image. It names all 214 wrappers, and
 *    xlat2 emits only the ones this image binds — the TLV probe binds 4 natives — so including it would
 *    leave the other 210 undefined at link time. The set is per image, so xlat2 emits it per image
 *    (@__arm2x86_rfcallees, from the binds that carry rf=1).
 */
extern void *const __arm2x86_rfcallees[];
extern const unsigned long __arm2x86_rfcallees_n;

__attribute__((weak)) int a64_is_rf_callee(unsigned long addr) {
    for (unsigned long k = 0; k < __arm2x86_rfcallees_n; k++)
        if ((unsigned long)(uintptr_t)__arm2x86_rfcallees[k] == addr) return 1;
    return 0;
}

/* Who are we calling. Only the symbol says whether the variadic ABI applies, and the two
 *    architectures disagree about it (interp/vararg.c). dyld bound this image's imports, so dyld is who
 *    knows the name — dladdr reads it back from the image that owns the address, which is correct for a
 *    native function, for one of our own wrappers, and for a dlopened dependency alike.
 *    No name is a real answer, not a failure: nothing is re-marshalled, which is what the generic host
 *    also does for an address it never bound. dladdr returns the C name (no leading underscore) and
 *    a64_variadic_fixup strips one anyway, so both spellings match the same row.
 */
__attribute__((weak)) const char *a64_native_name(unsigned long addr) {
    Dl_info in;
    /* the string belongs to dyld's symbol table for a loaded image, so it outlives the call */
    if (!dladdr((const void *)(uintptr_t)addr, &in) || !in.dli_sname) return 0;
    return in.dli_sname;
}

/* AOT images already have typed import bindings. */
/* a64_rt.c fills this 31-register side channel before every register-file call. The generic host defines
   it in cxx_bridge.cpp for the register-specialised retain/release wrappers; a translated image lifts those
   helpers itself, so here the array only has to exist. Weak, so a bridge that defines it wins. */
__attribute__((weak)) __thread unsigned long __arm2x86_rfall[31];

__attribute__((weak)) int __arm2x86_sig_door(const char *name, unsigned long target,
        unsigned long *xs, unsigned char *vs, unsigned long sp) {
    (void)name; (void)target; (void)xs; (void)vs; (void)sp;
    return 0;
}

__attribute__((weak)) int a2x_native_loader_call(unsigned long target,unsigned long *xs) {
    (void)target;(void)xs;return 0;
}
/* The interpreter's per-target door cache (a64_rt.c) asks the host three things. A translated image answers
   "cannot cache" (generation 0) and "every fixup may apply", which is exactly the behaviour before the cache. */
__attribute__((weak)) unsigned a64_names_gen(void) { return 0; }
__attribute__((weak)) unsigned a64_door_flags(const char *name) { (void)name; return 7; }
__attribute__((weak)) int __arm2x86_sig_applies(const char *name) { (void)name; return 1; }
