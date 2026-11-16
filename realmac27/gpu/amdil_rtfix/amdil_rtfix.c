// amdil_rtfix.dylib - loaded by libAMDIL902.dylib (the D300's AIR compiler, macOS 12.5 era, LLVM 11 inside).
//
// macOS 27's precompiled shaders (RenderBox, IconRendering, ...) describe render targets in two forms this compiler
// predates, and its ILEntryFunc::initShaderArgType() rejects them with
//   "Unsupported 'air.render_target' metadata configuration! Expected 2 ConstantInt operands ..."
// so every pipeline built from them fails and MTLCompilerService exits 1. Measured on the Mac Pro 2013, 27.2:
//   (a) !{"air.render_target", i32 addrspace(2)* @__metal_implicit_attr_int_expr_N, i32 0, ...}
//       the attachment index is a constant global, not an integer
//   (b) !{i32 K, "air.function_constant", !x, "air.render_target", i32 1, "air.arg_type_name", ...}
//       there is no blending index
// Before initShaderArgType runs, every render_target in the entry function's metadata is rewritten to the classic
// shape: (a) the global's i32 initializer, (b) blending index 0 inserted. Nothing else is touched.
//
// The compiler's LLVM is linked in privately; the functions used here are its own, found by their offsets from the
// image's __TEXT base (they are local symbols). Offsets belong to libAMDIL902.dylib 18,187,216 bytes, __TEXT at
// 0x4ffa0d370000; the hook refuses to install if the prologue bytes it expects are not there.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <dlfcn.h>

#define TEXT_BASE           0x4ffa0d370000ULL
#define OFF(a)              ((uint64_t)(a) - TEXT_BASE)
#define F_INITSHADERARGTYPE OFF(0x4ffa0d3db188ULL)
#define F_GETFUNCMDNODE     OFF(0x4ffa0d3d8552ULL)
#define F_MDSTRING_GETSTR   OFF(0x4ffa0de4f410ULL)
#define F_MDNODE_REPLACEOP  OFF(0x4ffa0de564c0ULL)
#define F_VAM_GET           OFF(0x4ffa0de4ed20ULL)
#define F_CINT_GET          OFF(0x4ffa0ddc2a50ULL)
#define F_GETINT32TY        OFF(0x4ffa0de78d90ULL)
#define F_MDTUPLE_GETIMPL   OFF(0x4ffa0de562c0ULL)

// LLVM 11 layout, read from this compiler's own code (validator in initShaderArgType)
#define MD_KIND(m)          (*(const uint8_t *)(m))          // 0 MDString, 1 ConstantAsMetadata, 4.. MDNode kinds
#define MDK_STRING          0
#define MDK_CONSTANT        1
#define MDK_TUPLE           4
#define MDN_NUMOPS(n)       (*(const uint32_t *)((const char *)(n) + 8))
#define MDN_OP(n, i)        (((void *const *)(n))[(long)(i) - (long)MDN_NUMOPS(n)])
#define VAM_VALUE(m)        (*(void *const *)((const char *)(m) + 0x80))
#define VAL_ID(v)           (*(const uint8_t *)((const char *)(v) + 0x10))
#define VAL_TYPE(v)         (*(void *const *)(v))
#define TYPE_CTX(t)         (*(void *const *)(t))
#define VID_GLOBALVAR       8
#define VID_CONSTANTINT     0x12
#define GV_INITIALIZER(g)   (*(void *const *)((const char *)(g) - 0x20))   // Op<0>, one 32-byte Use before the object

typedef struct { const char *p; size_t n; } strref;
static void *(*p_getFuncMDNode)(void *M);
static strref (*p_getString)(void *mds);
static void (*p_replaceOp)(void *node, unsigned i, void *md);
static void *(*p_vamGet)(void *value);
static void *(*p_cintGet)(void *type, uint64_t v, bool isSigned);
static void *(*p_int32Ty)(void *ctx);
static void *(*p_tupleGet)(void *ctx, void **ops, size_t n, unsigned storage, bool create);
static void (*p_orig)(void *self, void *M);         // trampoline to the original initShaderArgType
static unsigned g_fixed_a, g_fixed_b;

static bool is_rt_string(void *md) {
    if (!md || MD_KIND(md) != MDK_STRING) return false;
    strref s = p_getString(md);
    return s.n == 17 && !memcmp(s.p, "air.render_target", 17);
}
static bool is_cint_md(void *md) {
    return md && MD_KIND(md) == MDK_CONSTANT && VAM_VALUE(md) && VAL_ID(VAM_VALUE(md)) == VID_CONSTANTINT;
}

// Returns a replacement node for e (form b), or NULL; fixes form (a) in place.
static void *fix_node(void *e) {
    unsigned n = MDN_NUMOPS(e);
    for (unsigned k = 0; k < n; k++) {
        if (!is_rt_string(MDN_OP(e, k))) continue;
        if (k + 1 >= n) return NULL;
        void *idx = MDN_OP(e, k + 1);
        // (a) attachment index given as a constant global: use its integer initializer
        if (idx && MD_KIND(idx) == MDK_CONSTANT && !is_cint_md(idx)) {
            void *gv = VAM_VALUE(idx);
            if (!gv || VAL_ID(gv) != VID_GLOBALVAR) return NULL;
            void *init = GV_INITIALIZER(gv);
            if (!init || VAL_ID(init) != VID_CONSTANTINT) return NULL;
            p_replaceOp(e, k + 1, p_vamGet(init));
            g_fixed_a++;
            idx = MDN_OP(e, k + 1);
        }
        if (!is_cint_md(idx)) return NULL;
        // (b) no blending index: insert i32 0 after the attachment index
        if (k + 2 < n && is_cint_md(MDN_OP(e, k + 2))) return NULL;
        if (n + 1 > 64) return NULL;
        void *ops[64];
        for (unsigned i = 0, j = 0; i < n; i++) {
            ops[j++] = MDN_OP(e, i);
            if (i == k + 1) {
                void *ctx = TYPE_CTX(VAL_TYPE(VAM_VALUE(idx)));
                ops[j++] = p_vamGet(p_cintGet(p_int32Ty(ctx), 0, false));
            }
        }
        void *ctx = TYPE_CTX(VAL_TYPE(VAM_VALUE(idx)));
        g_fixed_b++;
        return p_tupleGet(ctx, ops, n + 1, 0 /* Uniqued */, true);
    }
    return NULL;
}

static void hook_initShaderArgType(void *self, void *M) {
    void *fn = M ? p_getFuncMDNode(M) : NULL;
    if (fn && MD_KIND(fn) >= MDK_TUPLE) {
        unsigned nf = MDN_NUMOPS(fn);
        for (unsigned i = 0; i < nf; i++) {
            void *list = MDN_OP(fn, i);
            if (!list || MD_KIND(list) < MDK_TUPLE) continue;
            unsigned nl = MDN_NUMOPS(list);
            for (unsigned j = 0; j < nl; j++) {
                void *e = MDN_OP(list, j);
                if (!e || MD_KIND(e) < MDK_TUPLE) continue;
                void *ne = fix_node(e);
                if (ne) p_replaceOp(list, j, ne);
            }
        }
    }
    p_orig(self, M);
}

static const uint8_t k_prologue[17] = {0x55, 0x48,0x89,0xe5, 0x41,0x57, 0x41,0x56, 0x41,0x55, 0x41,0x54, 0x53,
                                       0x48,0x83,0xec,0x68};

static void install(const struct mach_header_64 *mh, intptr_t slide) {
    uint8_t *base = (uint8_t *)mh;                  // __TEXT starts at the header
    uint8_t *target = base + F_INITSHADERARGTYPE;
    if (memcmp(target, k_prologue, sizeof k_prologue)) {
        fprintf(stderr, "amdil_rtfix: initShaderArgType prologue differs - not installed\n");
        return;
    }
    p_getFuncMDNode = (void *)(base + F_GETFUNCMDNODE);
    p_getString     = (void *)(base + F_MDSTRING_GETSTR);
    p_replaceOp     = (void *)(base + F_MDNODE_REPLACEOP);
    p_vamGet        = (void *)(base + F_VAM_GET);
    p_cintGet       = (void *)(base + F_CINT_GET);
    p_int32Ty       = (void *)(base + F_GETINT32TY);
    p_tupleGet      = (void *)(base + F_MDTUPLE_GETIMPL);
    // trampoline: the 17 relocated prologue bytes, then jmp *[rip] back to target+17
    uint8_t *tr = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (tr == MAP_FAILED) return;
    memcpy(tr, k_prologue, sizeof k_prologue);
    uint8_t jmp[14] = {0xff, 0x25, 0, 0, 0, 0};
    uint64_t back = (uint64_t)(target + sizeof k_prologue);
    memcpy(jmp + 6, &back, 8);
    memcpy(tr + sizeof k_prologue, jmp, sizeof jmp);
    if (mprotect(tr, 4096, PROT_READ | PROT_EXEC)) { fprintf(stderr, "amdil_rtfix: trampoline not executable\n"); return; }
    p_orig = (void *)tr;
    // patch: jmp *[rip] to the hook (14 bytes), rest of the 17 left as is (never reached)
    uint64_t to = (uint64_t)hook_initShaderArgType;
    memcpy(jmp + 6, &to, 8);
    vm_address_t page = (vm_address_t)target & ~(vm_address_t)4095;
    vm_size_t len = ((vm_address_t)target + 32 - page + 4095) & ~(vm_size_t)4095;
    if (vm_protect(mach_task_self(), page, len, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
        fprintf(stderr, "amdil_rtfix: cannot make the compiler text writable - not installed\n");
        return;
    }
    memcpy(target, jmp, sizeof jmp);
    vm_protect(mach_task_self(), page, len, false, VM_PROT_READ | VM_PROT_EXECUTE);
    sys_icache_invalidate(target, sizeof jmp);
    (void)slide;
}

static int g_installed;
static void on_add_image(const struct mach_header *mh, intptr_t slide) {
    if (g_installed) return;
    Dl_info di;
    if (!dladdr(mh, &di) || !di.dli_fname) return;
    size_t l = strlen(di.dli_fname);
    if (l >= 17 && !strcmp(di.dli_fname + l - 17, "libAMDIL902.dylib")) {
        g_installed = 1;
        install((const struct mach_header_64 *)mh, slide);
    }
}
__attribute__((constructor)) static void amdil_rtfix_init(void) {
    // fires for every image already loaded (libAMDIL902 is mapped before its dependencies' initializers run) and later
    _dyld_register_func_for_add_image(on_add_image);
}

// for the offline test harness
unsigned amdil_rtfix_counts(unsigned *b) { if (b) *b = g_fixed_b; return g_fixed_a; }
