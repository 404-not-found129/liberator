/* sl_display.c - makes WindowServer's display-state reply match what 27's x86_64 SkyLight decoder reads, for every client.
 *
 * reuses: the earlier sl_display.c (same add-image arming and copy-on-write protect).
 * new-because: the earlier form patched the decoder inside processes running on this host. A plain x86_64 process (Apple's own
 *   slice, not routed; 321 of them) kept reading zero displays: SecurityAgent's dialogs landed in a corner and the boot login panel
 *   was squeezed to 181 points wide. Routing SecurityAgent instead broke login outright (loginwindow exit 0 in a loop).
 *
 * The bug: 27's x86_64 SkyLight encoder never writes a 1-byte std::optional presence flag that its own decoder reads at stream offset
 *   32. Every later field reads one byte late, the blob runs out, and the client destroys the state: zero displays.
 * The fix, in WindowServer only: message 34003 (reply 34103, read from the client stub) is handled through a standard MIG table in
 *   SkyLight's __DATA_CONST. This process's private copy of that one table entry points here; after the real handler builds the reply,
 *   the blob is copied with a 0 at offset 32, meaning absent. No client changes; Apple's code in every other process stays stock.
 * Refuses unless exactly one table has this range. A2X_SLDISPLAY=0 turns it off.
 */
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SLD_START 34000u
#define SLD_END 34010u
#define SLD_ID 34003u
#define SLD_FLAG_OFFSET 32u       /* the decoder's cursor when it reads the flag */
/* The reply, from the server stub's own stores: descriptor address +28, flags +36, size +40, NDR +44, count +52, total 56. */
#define SLD_REPLY_SIZE 56u

typedef void (*sld_stub_t)(const mach_msg_header_t *, mach_msg_header_t *);
static sld_stub_t sld_real;
static volatile int sld_done, sld_logged;

/* Returns 1 when the reply was rewritten. Any reply that does not carry a blob, such as an error, passes through. */
static int sld_fix_reply(mach_msg_header_t *out) {
    if (!(out->msgh_bits & MACH_MSGH_BITS_COMPLEX) || out->msgh_size != SLD_REPLY_SIZE) return 0;
    char *o = (char *)out;
    unsigned char *old;
    uint32_t size, count;
    memcpy(&old, o + 28, 8); memcpy(&size, o + 40, 4); memcpy(&count, o + 52, 4);
    if (!old || size != count || size <= SLD_FLAG_OFFSET) return 0;
    vm_address_t na = 0;
    kern_return_t kr = vm_allocate(mach_task_self(), &na, (vm_size_t)size + 1, VM_FLAGS_ANYWHERE);
    if (kr) { fprintf(stderr, "[sldisplay] no memory for the reply (%s), sent unchanged\n", mach_error_string(kr)); return 0; }
    unsigned char *n = (unsigned char *)na;
    memcpy(n, old, SLD_FLAG_OFFSET);
    n[SLD_FLAG_OFFSET] = 0;
    memcpy(n + SLD_FLAG_OFFSET + 1, old + SLD_FLAG_OFFSET, size - SLD_FLAG_OFFSET);
    vm_deallocate(mach_task_self(), (vm_address_t)old, size);   /* the descriptor said deallocate, and now names the copy */
    uint32_t ns = size + 1;
    memcpy(o + 28, &n, 8); memcpy(o + 40, &ns, 4); memcpy(o + 52, &ns, 4);
    return 1;
}

static void sld_wrap(const mach_msg_header_t *in, mach_msg_header_t *out) {
    sld_real(in, out);
    if (sld_fix_reply(out) && !__sync_lock_test_and_set(&sld_logged, 1))
        fprintf(stderr, "[host] display state flag inserted in WindowServer's reply, pid %d\n", getpid());
}

/* The address of the handler pointer for SLD_ID, and how many tables have this exact range. */
static int sld_scan(const char *d, uint64_t size, uint64_t tlo, uint64_t thi, uint64_t **slot) {
    int hits = 0;
    for (uint64_t o = 8; o + 24 + (SLD_END - SLD_START) * 40 <= size; o += 8) {
        uint32_t start, end;
        uint64_t server, impl, stub;
        memcpy(&start, d + o, 4); memcpy(&end, d + o + 4, 4);
        if (start != SLD_START || end != SLD_END) continue;
        memcpy(&server, d + o - 8, 8);
        if (server < tlo || server >= thi) continue;
        const char *e = d + o + 24 + (SLD_ID - SLD_START) * 40;
        memcpy(&impl, e, 8); memcpy(&stub, e + 8, 8);
        if (stub < tlo || stub >= thi) continue;
        *slot = (uint64_t *)(e + 8);
        hits++;
    }
    return hits;
}

/* 1 = not SkyLight, 0 = handled (wrapped or refused once) */
static int sld_try(const struct mach_header *mh0, const char *name) {
    /* WindowServer's own path is .../SkyLight.framework/Resources/WindowServer: only the SkyLight dylib itself. */
    static const char want[] = "/SkyLight.framework/Versions/A/SkyLight";
    size_t nl = name ? strlen(name) : 0;
    if (nl < sizeof want - 1 || strcmp(name + nl - (sizeof want - 1), want)) return 1;
    if (!__sync_bool_compare_and_swap(&sld_done, 0, 1)) return 0;
    const struct mach_header_64 *mh = (const struct mach_header_64 *)mh0;
    if (mh->magic != MH_MAGIC_64) { fprintf(stderr, "[sldisplay] REFUSED: SkyLight is not a 64-bit image\n"); return 0; }
    uint64_t slide = 0, tlo = (uint64_t)mh, thi = 0;
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize))
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__TEXT")) {
            const struct segment_command_64 *sg = (const void *)lc;
            slide = tlo - sg->vmaddr; thi = tlo + sg->vmsize; break;
        }
    if (!thi) { fprintf(stderr, "[sldisplay] REFUSED: no __TEXT in %s\n", name); return 0; }
    uint64_t *slot = 0;
    int hits = 0;
    lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize))
        if (lc->cmd == LC_SEGMENT_64 && !strncmp(((const struct segment_command_64 *)lc)->segname, "__DATA", 6)) {
            const struct segment_command_64 *sg = (const void *)lc;
            hits += sld_scan((const char *)(sg->vmaddr + slide), sg->vmsize, tlo, thi, &slot);
        }
    if (hits != 1) { fprintf(stderr, "[sldisplay] REFUSED: %d tables for message %u (expected 1), nothing changed\n", hits, SLD_ID); return 0; }
    sld_real = (sld_stub_t)*slot;
    mach_vm_address_t pg = (mach_vm_address_t)slot & ~(mach_vm_address_t)(vm_page_size - 1);
    kern_return_t kr = mach_vm_protect(mach_task_self(), pg, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr) { fprintf(stderr, "[sldisplay] REFUSED: mach_vm_protect rw: %s, nothing changed\n", mach_error_string(kr)); return 0; }
    *slot = (uint64_t)sld_wrap;
    kr = mach_vm_protect(mach_task_self(), pg, vm_page_size, FALSE, VM_PROT_READ);
    fprintf(stderr, "[sldisplay] WindowServer display reply handler wrapped at %p pid %d%s%s\n", (void *)slot, getpid(), kr ? ", restoring r-- FAILED: " : "", kr ? mach_error_string(kr) : "");
    return 0;
}

static void sld_added(const struct mach_header *mh, intptr_t slide) {
    (void)slide;
    if (sld_done) return;
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++)
        if (_dyld_get_image_header(i) == mh) { sld_try(mh, _dyld_get_image_name(i)); return; }
}

/* Called once by the host for every guest. Only WindowServer encodes this message. */
void a2x_sldisplay_arm(const char *gpath) {
    const char *b = gpath ? strrchr(gpath, '/') : 0;
    if (!b || strcmp(b + 1, "WindowServer")) return;
    const char *off = A2X_DBGENV("A2X_SLDISPLAY");
    if (off && off[0] == '0') return;
    _dyld_register_func_for_add_image(sld_added);   /* also replays every image already loaded */
}

#ifdef SLD_SELFTEST
/* Run natively: the reply rewrite on real-shaped replies, the guard in both directions, and the table scan on fake segments. */
static int bad;
static void arm(const char *name, int pass) { printf("%s %s\n", pass ? "ok  " : "FAIL", name); if (!pass) bad++; }
static void make_reply(char *r, uint32_t bits, uint32_t msize, void *addr, uint32_t size, uint32_t count) {
    mach_msg_header_t *h = (mach_msg_header_t *)r;
    memset(r, 0, 64);
    h->msgh_bits = bits; h->msgh_size = msize;
    memcpy(r + 28, &addr, 8); memcpy(r + 40, &size, 4); memcpy(r + 52, &count, 4);
}
static void fake_table(char *d, uint64_t o, uint64_t server, uint64_t stub) {
    uint32_t s = SLD_START, e = SLD_END;
    memcpy(d + o - 8, &server, 8); memcpy(d + o, &s, 4); memcpy(d + o + 4, &e, 4);
    memcpy(d + o + 24 + (SLD_ID - SLD_START) * 40 + 8, &stub, 8);
}
int main(void) {
    char r[64];
    vm_address_t a = 0;
    uint32_t sz = 17208;
    vm_allocate(mach_task_self(), &a, sz, VM_FLAGS_ANYWHERE);
    unsigned char *b = (unsigned char *)a;
    for (uint32_t i = 0; i < sz; i++) b[i] = (unsigned char)(i * 7 + 1);
    unsigned char before31 = b[31], before32 = b[32], last = b[sz - 1];
    make_reply(r, MACH_MSGH_BITS_COMPLEX, SLD_REPLY_SIZE, b, sz, sz);
    arm("a display reply is rewritten", sld_fix_reply((mach_msg_header_t *)r) == 1);
    unsigned char *n; uint32_t ns, nc;
    memcpy(&n, r + 28, 8); memcpy(&ns, r + 40, 4); memcpy(&nc, r + 52, 4);
    arm("size and count both grow by exactly one", ns == sz + 1 && nc == sz + 1);
    arm("bytes before the flag are unchanged", n[31] == before31);
    arm("the flag reads absent at offset 32", n[32] == 0);
    arm("the rest moved by one byte, through the last", n[33] == before32 && n[sz] == last);
    unsigned char x[64] = { 1 };
    make_reply(r, 0, 36, x, 64, 64);
    arm("an error reply, not complex, is not touched", sld_fix_reply((mach_msg_header_t *)r) == 0);
    make_reply(r, MACH_MSGH_BITS_COMPLEX, SLD_REPLY_SIZE, x, 64, 63);
    arm("a reply whose size and count disagree is not touched", sld_fix_reply((mach_msg_header_t *)r) == 0);
    make_reply(r, MACH_MSGH_BITS_COMPLEX, SLD_REPLY_SIZE, x, 32, 32);
    arm("a reply too short to hold the flag is not touched", sld_fix_reply((mach_msg_header_t *)r) == 0);
    make_reply(r, MACH_MSGH_BITS_COMPLEX, SLD_REPLY_SIZE, 0, 64, 64);
    arm("a reply with no blob is not touched", sld_fix_reply((mach_msg_header_t *)r) == 0);
    static char d[8192];
    uint64_t tlo = 0x1000, thi = 0x2000, *slot = 0;
    arm("no table counts 0, which refuses", sld_scan(d, sizeof d, tlo, thi, &slot) == 0);
    fake_table(d, 512, 0x1100, 0x1200);
    arm("one table counts 1", sld_scan(d, sizeof d, tlo, thi, &slot) == 1);
    uint64_t got = 0; if (slot) memcpy(&got, slot, 8);
    arm("the slot found holds the handler", got == 0x1200);
    fake_table(d, 2048, 0x1100, 0x1300);
    arm("two tables count 2, which refuses", sld_scan(d, sizeof d, tlo, thi, &slot) == 2);
    memset(d, 0, sizeof d);
    fake_table(d, 512, 0x9000, 0x1200);
    arm("a range whose server is outside SkyLight is not a table", sld_scan(d, sizeof d, tlo, thi, &slot) == 0);
    printf("%d failed\n", bad);
    return bad != 0;
}
#endif
