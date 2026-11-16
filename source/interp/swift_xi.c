/* swift_xi.c - lets native Optional witnesses read a nested-none value written by arm64 guest code.
 *
 * reuses: sl_display.c (same copy-on-write protect, patch, restore in this process only; Apple's files stay stock).
 * new-because: no existing host piece corrects a Swift enum tag; the difference sits in compiled Swift ABI, not in any import.
 *
 * The difference, seen by compiling one Swift source for both architectures: a heap reference's spare values are index << 1
 * on x86_64 (bit 0 is reserved for tagged pointers) and plain index on arm64. T?.none is index 0 and matches. T??.none is word 1
 * on arm64 and word 2 on x86_64. Native code reads word 1 as a real pointer, so Control Center died on address 0x11 comparing a
 * Text whose modifiers array was the word 1.
 * The fix: every Optional in the process shares one get witness and one enum tag witness (checked on Int, String, Bool, Text,
 * nested Text and ModifiedContent). Each starts with a 16-byte thunk, which now comes here first. A word that is below 2^31 and
 * odd in a heap reference's spare-value slot cannot be a valid reference, so it is an arm64 index, and the native witness reads a
 * private copy holding index << 1. The value itself is never written: guest code still reads what it wrote.
 * Not covered: an even small word is ambiguous (arm64 index 2 reads as x86_64 index 1, which is T??.none read for T???.none and
 * stays a none); native concrete code that inlines its own tag test; a value native code writes and guest code then reads.
 */
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef unsigned (*sxi_get_t)(const void *, unsigned, const void *);
typedef unsigned (*sxi_tag_t)(const void *, const void *);
typedef void (*sxi_store_t)(void *, unsigned, unsigned, const void *);
typedef struct { const void *v; unsigned long s; } sxi_mr_t;

#define SXI_OPTIONAL_KIND 0x202ul
#define SXI_MAX_PAYLOAD 2048ul   /* chosen: a stack buffer per first sight of a type; a larger payload keeps native behaviour */
#define SXI_TABLE 8192u          /* chosen: a full table only costs a recount, never a wrong answer */
#define SXI_PROBES 8u

static sxi_get_t sxi_get_real;
static sxi_tag_t sxi_tag_real;
static _Atomic uint64_t sxi_table[SXI_TABLE];
static _Atomic int sxi_logged;

/* The offset of the word holding a heap reference's spare values, or -1. Asks the payload's own store witness: two consecutive
   spare values must land in one aligned word, two apart, with every other byte untouched. Raw pointers step by 1 and Bool lives
   in a byte; both fail this and keep native behaviour. */
static int sxi_slot_measure(const void *self) {
    const void *payload = ((const void *const *)self)[2];
    const unsigned long *pw = *(const unsigned long *const *)((const char *)payload - 8);
    unsigned long size = pw[8];
    unsigned xi = (unsigned)(pw[10] >> 32);
    if (xi < 3 || size < 8 || size > SXI_MAX_PAYLOAD) return -1;
    _Alignas(16) unsigned char a[SXI_MAX_PAYLOAD], b[SXI_MAX_PAYLOAD];
    memset(a, 0, size); memset(b, 0, size);
    sxi_store_t st = (sxi_store_t)pw[7];
    st(a, 2, 3, payload); st(b, 3, 3, payload);
    int slot = -1;
    for (unsigned long o = 0; o + 8 <= size; o += 8) {
        uint64_t x, y;
        memcpy(&x, a + o, 8); memcpy(&y, b + o, 8);
        if (x == y) { if (x) return -1; continue; }
        if (slot >= 0 || (x & 1) || y != x + 2) return -1;
        slot = (int)o;
    }
    for (unsigned long o = size & ~7ul; o < size; o++) if (a[o] != b[o] || a[o]) return -1;
    return slot;
}

static int sxi_slot(const void *self) {
    uint64_t key = (uint64_t)self;
    unsigned h = (unsigned)((key >> 4) ^ (key >> 17)) % SXI_TABLE;
    for (unsigned i = 0; i < SXI_PROBES; i++) {
        uint64_t e = atomic_load_explicit(&sxi_table[(h + i) % SXI_TABLE], memory_order_relaxed);
        if (!e) break;
        if ((e >> 16) == key) return (int)(e & 0xffff) - 1;
    }
    int slot = sxi_slot_measure(self);
    uint64_t ne = (key << 16) | (uint64_t)(slot + 1);
    for (unsigned i = 0; i < SXI_PROBES; i++) {
        uint64_t zero = 0;
        if (atomic_compare_exchange_strong(&sxi_table[(h + i) % SXI_TABLE], &zero, ne) || zero == ne) break;
    }
    return slot;
}

static void sxi_log(const void *self, uint64_t w, int slot) {
    if (atomic_fetch_add_explicit(&sxi_logged, 1, memory_order_relaxed) >= 4) return;
    typedef struct { const char *p; size_t n; } tn_t;
    tn_t (*nm)(const void *, int) = (tn_t (*)(const void *, int))dlsym(RTLD_DEFAULT, "swift_getTypeName");
    tn_t t = { "?", 1 };
    if (nm) t = nm(self, 1);
    fprintf(stderr, "[host] nested none from arm64 word %llu slot %d pid %d type %.*s\n", (unsigned long long)w, slot, getpid(), (int)(t.n > 200 ? 200 : t.n), t.p);
}

/* The value to hand the native witness: the original, or buf holding the x86_64 form of an arm64 spare value. */
static const void *sxi_view(const void *value, const void *self, unsigned char *buf) {
    if (*(const unsigned long *)self != SXI_OPTIONAL_KIND) return value;
    int slot = sxi_slot(self);
    if (slot < 0) return value;
    uint64_t w;
    memcpy(&w, (const char *)value + slot, 8);
    if (!(w & 1) || w >= 0x80000000ull) return value;
    memcpy(buf, value, (size_t)slot);
    uint64_t x = w << 1;
    memcpy(buf + slot, &x, 8);
    sxi_log(self, w, slot);
    return buf;
}

static unsigned sxi_get(const void *value, unsigned n, const void *self) {
    _Alignas(16) unsigned char buf[SXI_MAX_PAYLOAD + 8];
    return sxi_get_real(sxi_view(value, self, buf), n, self);
}

static unsigned sxi_tag(const void *value, const void *self) {
    _Alignas(16) unsigned char buf[SXI_MAX_PAYLOAD + 8];
    return sxi_tag_real(sxi_view(value, self, buf), self);
}

/* The real function behind a thunk: push rbp; mov rbp,rsp; pop rbp; jmp rel32; 6-byte nop. 0 when the bytes differ. */
static void *sxi_thunk_target(const unsigned char *t) {
    static const unsigned char head[6] = { 0x55, 0x48, 0x89, 0xe5, 0x5d, 0xe9 };
    static const unsigned char tail[6] = { 0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00 };
    if (memcmp(t, head, 6) || memcmp(t + 10, tail, 6)) return 0;
    int32_t rel;
    memcpy(&rel, t + 6, 4);
    return (void *)(t + 10 + rel);
}

static int sxi_patch(unsigned char *t, void *to) {
    unsigned char code[14] = { 0xff, 0x25, 0, 0, 0, 0 };   /* jmp *0(%rip); the address follows */
    memcpy(code + 6, &to, 8);
    mach_vm_address_t pg = (mach_vm_address_t)t & ~(mach_vm_address_t)(vm_page_size - 1);
    mach_vm_size_t len = ((mach_vm_address_t)t + sizeof code - pg + vm_page_size - 1) & ~(mach_vm_size_t)(vm_page_size - 1);
    kern_return_t kr = mach_vm_protect(mach_task_self(), pg, len, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr) { fprintf(stderr, "[swiftxi] REFUSED: mach_vm_protect rw: %s\n", mach_error_string(kr)); return 0; }
    memcpy(t, code, sizeof code);
    kr = mach_vm_protect(mach_task_self(), pg, len, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr) fprintf(stderr, "[swiftxi] restoring r-x FAILED: %s\n", mach_error_string(kr));
    return 1;
}

/* Called once for every guest before guest code runs. Foundation already brought the runtime in, so nothing new is loaded. */
void a2x_swiftxi_arm(void) {
    if (getpid() == 1) return;
    const char *off = A2X_DBGENV("A2X_SWIFTXI");
    if (off && off[0] == '0') return;
    void *core = dlopen("/usr/lib/swift/libswiftCore.dylib", RTLD_NOW | RTLD_NOLOAD);
    if (!core) return;
    sxi_mr_t (*opt)(unsigned long, const void *) = (sxi_mr_t (*)(unsigned long, const void *))dlsym(core, "$sSqMa");
    const void *si = dlsym(core, "$sSiN"), *ss = dlsym(core, "$sSSN");
    if (!opt || !si || !ss) { fprintf(stderr, "[swiftxi] REFUSED: runtime symbols missing\n"); return; }
    const void *oi = opt(0, si).v, *os = opt(0, ss).v;
    const unsigned long *a = *(const unsigned long *const *)((const char *)oi - 8), *b = *(const unsigned long *const *)((const char *)os - 8);
    if (a[6] != b[6] || a[11] != b[11]) { fprintf(stderr, "[swiftxi] REFUSED: Optional witnesses are not shared\n"); return; }
    unsigned char *tg = (unsigned char *)a[6], *tt = (unsigned char *)a[11];
    void *rg = sxi_thunk_target(tg), *rt = sxi_thunk_target(tt);
    if (!rg || !rt) { fprintf(stderr, "[swiftxi] REFUSED: witness entry bytes differ, nothing changed\n"); return; }
    sxi_get_real = (sxi_get_t)rg;
    sxi_tag_real = (sxi_tag_t)rt;
    if (sxi_patch(tg, (void *)sxi_get)) sxi_patch(tt, (void *)sxi_tag);
}

#ifdef SWIFTXI_SELFTEST
/* Run natively on the guest: the same types and words before and after arming, with the guard checked in both directions. */
static int fails;
static void arm(const char *name, unsigned got, unsigned want) {
    printf("%s  %s: got %u want %u\n", got == want ? "ok  " : "FAIL", name, got, want);
    if (got != want) fails++;
}
int main(void) {
    void *core = dlopen("/usr/lib/swift/libswiftCore.dylib", RTLD_NOW);
    sxi_mr_t (*opt)(unsigned long, const void *) = (sxi_mr_t (*)(unsigned long, const void *))dlsym(core, "$sSqMa");
    sxi_mr_t (*ary)(unsigned long, const void *) = (sxi_mr_t (*)(unsigned long, const void *))dlsym(core, "$sSaMa");
    const void *si = dlsym(core, "$sSiN"), *sb = dlsym(core, "$sSbN");
    const void *as = ary(0, si).v, *o1 = opt(0, as).v, *o2 = opt(0, o1).v, *o3 = opt(0, o2).v, *ob = opt(0, sb).v;
    unsigned long *v2 = *(unsigned long **)((char *)o2 - 8);
    sxi_tag_t tag = (sxi_tag_t)v2[11];
    unsigned long w;
    w = 1; arm("before the fix, native reads arm64 Array??.none as a real value (control)", tag(&w, o2), 0);
    a2x_swiftxi_arm();
    w = 1; arm("arm64 Array??.none reads none", tag(&w, o2), 1);
    w = 2; arm("x86_64 Array??.none still reads none", tag(&w, o2), 1);
    w = 0; arm("Array??.some(.none) is still the payload case", tag(&w, o2), 0);
    w = 0; arm("Array?.none is unchanged", tag(&w, o1), 1);
    w = 1; arm("arm64 word 1 in Array? is treated as a none, never a pointer", tag(&w, o1), 2);
    w = 0x100000001ul; arm("an odd word above 2^31 is left alone (payload)", tag(&w, o2), 0);
    w = 0x7ff800000000ul; arm("a real pointer is the payload case", tag(&w, o2), 0);
    w = 3; arm("arm64 Array???.none, word 3, reads a none layer", tag(&w, o3) != 0, 1);
    unsigned char bv = 3; unsigned long *vb = *(unsigned long **)((char *)ob - 8);
    arm("Bool? byte values are not touched", ((sxi_tag_t)vb[11])(&bv, ob), 2);
    arm("Bool? has no heap slot", (unsigned)(sxi_slot(ob) + 1), 0);
    arm("Array? slot is word 0", (unsigned)sxi_slot(o1), 0);
    printf("%d failed\n", fails);
    return fails != 0;
}
#endif
