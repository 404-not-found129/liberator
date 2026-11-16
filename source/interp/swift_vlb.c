/* __VaListBuilder is an architecture-variant class. libswiftCore for arm64 and for x86_64 both export a class of that
   name with the same entry points (CMa, va_list) but different stored properties, so an arm64 program that inlines
   withVaList / String(format:) allocates and fills the arm64 layout and then calls into the x86 class.
     arm64  (0x30 bytes): +0x10 requiredAlignment, +0x18 count, +0x20 allocated (words), +0x28 storage
     x86_64 (0x40 bytes): +0x10 header{gp_offset, fp_offset, overflow_arg_area, reg_save_area}, +0x38 ContiguousArray
   In the two libswiftCores: the x86 va_list() writes +0x28 and +0x30 and reads +0x38 (past the 0x30 the program
   allocated), the x86 deinit swift_releases the word at +0x38, and deallocStorage has no x86 definition (it binds to 0).
   These handlers are the arm64 bodies, read from the arm64 libswiftCore disassembly. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <pthread.h>

extern __thread unsigned char __arm2x86_rf[208];
extern __thread unsigned long __arm2x86_rfall[31];
#define VRX(i) (((uint64_t *)__arm2x86_rf)[i])

static void (*p_slowDealloc)(void *, size_t, size_t);
static void (*p_deallocClassInstance)(void *, size_t, size_t);
static void vlb_syms(void) {
    if (!p_slowDealloc) p_slowDealloc = dlsym(RTLD_DEFAULT, "swift_slowDealloc");
    if (!p_deallocClassInstance) p_deallocClassInstance = dlsym(RTLD_DEFAULT, "swift_deallocClassInstance");
}

/* deallocStorage(wordCount: x0, storage: x1) is swift_slowDealloc(storage, wordCount * 8, 7) */
void a2x_vlb_deallocStorage(void) {
    vlb_syms();
    if (VRX(1)) p_slowDealloc((void *)VRX(1), (size_t)(VRX(0) << 3), 7);
}

/* va_list() on self (x20): the storage pointer, or a zeroed 16-aligned block when nothing was appended */
static _Alignas(16) uint64_t vlb_empty[16];
void a2x_vlb_va_list(void) {
    uint64_t self = __arm2x86_rfall[20];
    uint64_t st = *(uint64_t *)(uintptr_t)(self + 0x28);
    VRX(0) = st ? st : (uint64_t)(uintptr_t)vlb_empty;
}

/* the deallocating deinit; swift_release calls it natively with self in r13 (swiftself) */
void a2x_vlb_destroy_c(void *self) {
    vlb_syms();
    uint64_t st = *(uint64_t *)((char *)self + 0x28), words = *(uint64_t *)((char *)self + 0x20);
    if (st) p_slowDealloc((void *)st, (size_t)(words << 3), 7);
    p_deallocClassInstance(self, 0x30, 7);
}
__asm__(".text\n.globl _a2x_vlb_destroy_thunk\n_a2x_vlb_destroy_thunk:\n\tmovq %r13, %rdi\n\tjmp _a2x_vlb_destroy_c\n");
extern void a2x_vlb_destroy_thunk(void);

/* __VaListBuilderCMa answers a copy of the x86 class metadata whose destroy slot (metadata[-16]) is the arm64 deinit.
   Copied from (meta - ClassAddressPoint) for ClassSize bytes (+0x3c and +0x38; 0x18 and 0x90 in the cache's
   libswiftCore). The original is untouched for native x86 users of the class (Foundation's own String(format:)). */
static void *vlb_meta;
static pthread_once_t vlb_once = PTHREAD_ONCE_INIT;
static void vlb_build(void) {
    void *(*ma)(uintptr_t) = dlsym(RTLD_DEFAULT, "$ss15__VaListBuilderCMa");
    if (!ma) { fprintf(stderr, "[host] vlb: __VaListBuilderCMa not found; guest withVaList stays on the x86 class\n"); return; }
    char *m = ma(0);
    if (!m) return;
    uint32_t csz = *(uint32_t *)(m + 0x38), ap = *(uint32_t *)(m + 0x3c);
    if (csz < 0x50 || csz > 0x400 || ap < 16 || ap > 0x40) {
        fprintf(stderr, "[host] vlb: unexpected class metadata (size 0x%x, address point 0x%x); left alone\n", csz, ap);
        return;
    }
    char *raw = malloc(csz + 16);
    if (!raw) return;
    char *copy = (char *)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
    memcpy(copy, m - ap, csz);
    char *nm = copy + ap;
    *(void **)(nm - 16) = (void *)a2x_vlb_destroy_thunk;
    *(uint32_t *)(nm + 0x30) = 0x30;                    /* InstanceSize: the arm64 object */
    vlb_meta = nm;
}
void a2x_vlb_Ma(void) {
    pthread_once(&vlb_once, vlb_build);
    if (!vlb_meta) {                                    /* fall back to the x86 metadata, as before this change */
        void *(*ma)(uintptr_t) = dlsym(RTLD_DEFAULT, "$ss15__VaListBuilderCMa");
        VRX(0) = (uint64_t)(uintptr_t)(ma ? ma(VRX(0)) : 0);
    } else VRX(0) = (uint64_t)(uintptr_t)vlb_meta;
    VRX(1) = 0;                                         /* MetadataState: complete */
}

/* symbol -> handler, consulted by apply() ahead of the rf table */
void (*a2x_arm_only_handler(const char *sym))(void) {
    if (!sym) return 0;
    if (!strcmp(sym, "_$ss15__VaListBuilderC14deallocStorage9wordCount7storageySi_SpySiGtF")) return a2x_vlb_deallocStorage;
    if (!strcmp(sym, "_$ss15__VaListBuilderC7va_lists03CVaB7PointerVyF")) return a2x_vlb_va_list;
    if (!strcmp(sym, "_$ss15__VaListBuilderCMa")) return a2x_vlb_Ma;
    return 0;
}
