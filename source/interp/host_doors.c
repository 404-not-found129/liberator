// host_doors.c — the doors the ObjC bridge expects, implemented for the interpreter.
//
// objc_bridge.c is reused unchanged. It compiles clean for x86_64 and needs only four things of ours, and
//    each has a natural interpreter answer: a register-file call is simply "interpret at this address", and a
//    thread without an arm64 stack gets one. That is why the bridge is ported rather than rewritten — it
//    carries many ObjC/Swift ABI corrections that would be costly to redo.
// One door is not done and says so: __arm2x86_cb hands native code a callable stub for a guest function,
//    which needs the trampoline pool (131,072 generated slots in the per-app runtime). Until that is ported
//    it aborts by name rather than returning the arm64 address — native code calling an arm64 address
//    executes nonsense, and a loud stop names the symbol that wanted it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <dlfcn.h>

#include "a64i.h"

extern __thread unsigned long __arm_sp;
extern int __arm2x86_interp_run(unsigned long target, unsigned long *xs, unsigned char *vs);

/* The register file the bridge marshals through: x0..x8 at 0, v0..v7 at 80 + 16j. objc_bridge.c owns it —
      it reads and writes the layout directly — so this only borrows it. (Defining it here too is a duplicate
      symbol, which is how I learned the bridge already carries it.) */
extern __thread unsigned char __arm2x86_rf[208];

/* give this thread an arm64 stack if it has none — a pthread or GCD worker is a thread the guest has never
   run on, and the interpreter needs somewhere to build frames */
void __arm2x86_ensure_stack(void) {
    if (__arm_sp) return;
    size_t sz = 8u << 20;
    void *p = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "[host] cannot give this thread an arm64 stack\n");
        a2x_stop("no memory for a thread stack", ""); abort();
    }
    __arm_sp = ((unsigned long)(uintptr_t)p + sz - 4096) & ~15UL;
}

/* the bridge's door into guest code: arguments are already in the register file, and x0/v0 come back there */
void __arm2x86_call_rf(unsigned long imp) {
    __arm2x86_ensure_stack();
    unsigned long xs[32];
    memset(xs, 0, sizeof xs);
    _Alignas(16) unsigned char vs[32 * 16];
    memset(vs, 0, sizeof vs);

    const unsigned long *rf = (const unsigned long *)__arm2x86_rf;
    for (int k = 0; k <= 8; k++) xs[k] = rf[k];                  /* x0..x8 */
    for (int j = 0; j < 8; j++) memcpy(vs + 16 * j, __arm2x86_rf + 80 + 16 * j, 16);
    xs[31] = __arm_sp;

    unsigned long a2x_sp0 = __arm_sp;   /* restored below: see cb_host.c */
    int rc = __arm2x86_interp_run(imp, xs, vs);
    __arm_sp = a2x_sp0;
    if (rc) {
        fprintf(stderr, "[host] a guest method at 0x%lx did not complete (reason %d above)\n", imp, rc);
        a2x_stop("a method of the program did not complete", ""); abort();
    }
    ((unsigned long *)__arm2x86_rf)[0] = xs[0];
    ((unsigned long *)__arm2x86_rf)[1] = xs[1];
    /* Composite results use x0:x1 or up to four floating-point registers. */
    for (int j = 0; j < 4; j++)
        memcpy(__arm2x86_rf + 80 + 16 * j, vs + 16 * j, 16);
}

/* __arm2x86_cb and the trampoline pool now live in cb_host.c with the generated stubs (cb_slots.s). */

/* The guest's swift METADATA is invisible to libswiftCore unless we hand it over. The Swift runtime learns
      a program's conformances, type records and protocols from dyld's add-image callback over every loaded
      Mach-O — and a guest image is mapped by this host, not by dyld. Without this, a Swift type simply "does
      not conform" to a protocol it plainly conforms to, and the next metadata read is nil.
   The records are 32-bit relative pointers, so they are valid wherever the image sits; these three entry
      points are the ones dyld's own callback feeds. Arguments are section addresses and BYTE sizes.
   A Swift guest with no Swift runtime loaded is said out loud, never skipped: the program would run and
      then fail somewhere unrelated. */
void __arm2x86_swift_register(unsigned long proto, unsigned long nproto, unsigned long types,
                              unsigned long ntypes, unsigned long protos, unsigned long nprotos) {
    typedef void (*reg_t)(const void *, const void *);
    reg_t rc = (reg_t)dlsym(RTLD_DEFAULT, "swift_registerProtocolConformances");
    reg_t rt = (reg_t)dlsym(RTLD_DEFAULT, "swift_registerTypeMetadataRecords");
    reg_t rp = (reg_t)dlsym(RTLD_DEFAULT, "swift_registerProtocols");
    if (!rc || !rt || !rp) {
        fprintf(stderr, "[host] Swift runtime not loaded: %lu conformance / %lu type / %lu protocol record(s) "
                        "of the guest image NOT registered\n", nproto / 4, ntypes / 4, nprotos / 4);
        return;
    }
    if (nproto) rc((const void *)(uintptr_t)proto, (const void *)(uintptr_t)(proto + nproto));
    if (ntypes) rt((const void *)(uintptr_t)types, (const void *)(uintptr_t)(types + ntypes));
    if (nprotos) rp((const void *)(uintptr_t)protos, (const void *)(uintptr_t)(protos + nprotos));
}

/* Signature-derived stack layout, with the existing fallback for unsupported calls. */
#include "swift_stack_runtime.h"
