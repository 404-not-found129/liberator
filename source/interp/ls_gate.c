/* ls_gate.c — the host lets LaunchServices accept arm64/arm64e apps on this x86_64 machine, inside every process.
 * reuses: the earlier standalone LaunchServices fix — the same three sites and fixes, byte for byte, and its exactly-once rule;
 *   ws_quartz.c's arming pattern (_dyld_register_func_for_add_image, which replays images already loaded).
 * new-because: Apple's lsd / CoreServicesUIAgent plists stay unmodified, so no dylib can be inserted with DYLD_INSERT.
 *   The host is already the x86_64 slice of every one of those processes, so the patch lives here. Without it, every
 *   Dock icon carries the prohibitory badge —
 *   LaunchServices' _LSBundleDataGetUnsupportedFormatFlag asks for an x86_64 slice and an arm64e-only app has none.
 * Patch 1 gate: the movabs $0xffffffff01000007 (x86_64, any subtype) request becomes -1/-1 = "any slice".
 * Patch 2/3 mask (lsd): _LSSliceMaskForSliceArray's arm64 (0x80) and arm64e (0x100) arms also set x86_64's 0x8, so the
 *   database itself says this host can run them and every client agrees.
 * Each site must match exactly once in LaunchServices __TEXT or nothing changes for it and it says so. A2X_LSGATE=0 = off.
 */
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct lsg_patch { const char *what; const unsigned char *site; size_t n; size_t off; const unsigned char *fix; size_t fn; int alt; };
static const unsigned char LSG_GATE[] = {0x84,0xc0, 0x74,0x1e, 0x48,0xbe,0x07,0x00,0x00,0x01,0xff,0xff,0xff,0xff, 0x4c,0x89,0xf7, 0xe8};
static const unsigned char LSG_GATE_FIX[] = {0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff};
static const unsigned char LSG_M64[] = {0x85,0xc0, 0x0f,0x85,0x27,0x01,0x00,0x00, 0x41,0x81,0xcf,0x80,0x00,0x00,0x00, 0xe9,0x1b,0x01,0x00,0x00};
static const unsigned char LSG_M64_FIX[] = {0x88};
static const unsigned char LSG_M64E[] = {0x41,0x83,0xcf,0x08, 0xeb,0x43, 0x41,0x81,0xcf,0x00,0x01,0x00,0x00, 0xeb,0x3a, 0x41,0x81,0xcf,0x00,0x80,0x00,0x00};
static const unsigned char LSG_M64E_FIX[] = {0x08};
/* 27.2 moved both arms. Read from the 27.2 image: subtype 2 (arm64e) falls into or r15d,0x100 after its jne; subtype 0
 * (arm64) je lands on or r15d,0x80 inside the arm list (0x8 / 0x8000 / 0x80). Same fix, new anchors, 27.0 anchors kept. */
static const unsigned char LSG_M64_272[] = {0x41,0x83,0xcf,0x08, 0xeb,0x4c, 0x41,0x81,0xcf,0x00,0x80,0x00,0x00, 0xeb,0x43, 0x41,0x81,0xcf,0x80,0x00,0x00,0x00, 0xeb,0x3a};
static const unsigned char LSG_M64E_272[] = {0x83,0xf8,0x02, 0x0f,0x85,0x46,0x01,0x00,0x00, 0x41,0x81,0xcf,0x00,0x01,0x00,0x00, 0xe9,0x3a,0x01,0x00,0x00};
static const struct lsg_patch LSG[] = {
    {"slice gate",        LSG_GATE, sizeof LSG_GATE, 6,  LSG_GATE_FIX, sizeof LSG_GATE_FIX},
    {"arm64 slice mask",  LSG_M64,  sizeof LSG_M64,  11, LSG_M64_FIX,  sizeof LSG_M64_FIX},
    {"arm64 slice mask",  LSG_M64_272,  sizeof LSG_M64_272,  18, LSG_M64_FIX,  sizeof LSG_M64_FIX, 1},
    {"arm64e slice mask", LSG_M64E, sizeof LSG_M64E, 9,  LSG_M64E_FIX, sizeof LSG_M64E_FIX},
    {"arm64e slice mask", LSG_M64E_272, sizeof LSG_M64E_272, 12, LSG_M64E_FIX, sizeof LSG_M64E_FIX, 1},
};
static volatile int lsg_done, lsg_loud;
static const char *lsg_self = "?";

static int lsg_hits(const unsigned char *text, uint64_t tsize, const struct lsg_patch *pt) {
    int nhit = 0;
    for (const unsigned char *p = text; p + pt->n <= text + tsize; p++)
        if (*p == pt->site[0] && !memcmp(p, pt->site, pt->n)) nhit++;
    return nhit;
}
/* returns 1 patched, 0 refused (site count != 1), -1 protect failed */
static int lsg_apply(unsigned char *text, uint64_t tsize, const struct lsg_patch *pt, int writeable_test) {
    unsigned char *hit = 0; int nhit = 0;
    for (unsigned char *p = text; p + pt->n <= text + tsize; p++)
        if (*p == pt->site[0] && !memcmp(p, pt->site, pt->n)) { hit = p; nhit++; }
    if (nhit != 1) {
        fprintf(stderr, "[lsgate] REFUSED %s in %s pid %d: the site matched %d times (expected 1) — nothing changed\n",
                pt->what, lsg_self, getpid(), nhit);
        return 0;
    }
    unsigned char *at = hit + pt->off;
    if (writeable_test) { memcpy(at, pt->fix, pt->fn); return 1; }
    mach_vm_address_t pg = (mach_vm_address_t)at & ~(mach_vm_address_t)(vm_page_size - 1);
    mach_vm_size_t len = ((mach_vm_address_t)at + pt->fn - pg + vm_page_size - 1) & ~(mach_vm_size_t)(vm_page_size - 1);
    kern_return_t kr = mach_vm_protect(mach_task_self(), pg, len, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr) { fprintf(stderr, "[lsgate] REFUSED %s in %s: mach_vm_protect rw: %s\n", pt->what, lsg_self, mach_error_string(kr)); return -1; }
    memcpy(at, pt->fix, pt->fn);
    kr = mach_vm_protect(mach_task_self(), pg, len, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (lsg_loud || kr)
        fprintf(stderr, "[lsgate] patched the LaunchServices %s at %p in %s pid %d%s\n", pt->what, (void *)at, lsg_self, getpid(),
                kr ? " — r-x restore FAILED" : "");
    return 1;
}
static void lsg_added(const struct mach_header *mh, intptr_t slide) {
    (void)slide;
    if (lsg_done) return;
    for (uint32_t i = 0, n = _dyld_image_count(); i < n; i++) {
        if (_dyld_get_image_header(i) != mh) continue;
        const char *nm = _dyld_get_image_name(i);
        if (!nm || !strstr(nm, "/LaunchServices.framework/Versions/A/LaunchServices")) return;
        if (!__sync_bool_compare_and_swap(&lsg_done, 0, 1)) return;
        const struct mach_header_64 *ls = (const struct mach_header_64 *)mh;
        const struct load_command *lc = (const void *)(ls + 1);
        uint64_t tsize = 0;
        for (uint32_t k = 0; k < ls->ncmds; k++, lc = (const void *)((const char *)lc + lc->cmdsize))
            if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__TEXT")) {
                tsize = ((const struct segment_command_64 *)lc)->vmsize; break; }
        if (!tsize) { fprintf(stderr, "[lsgate] REFUSED: no __TEXT in LaunchServices (%s)\n", lsg_self); return; }
        for (size_t k = 0; k < sizeof LSG / sizeof LSG[0]; k++) {
            /* a site with a newer-build alternate is tried only if present, so one refused line means no build matched */
            if (k + 1 < sizeof LSG / sizeof LSG[0] && LSG[k + 1].alt && !LSG[k].alt && !lsg_hits((const unsigned char *)ls, tsize, &LSG[k])) continue;
            if (LSG[k].alt && lsg_hits((const unsigned char *)ls, tsize, &LSG[k - 1]) == 1) continue;
            lsg_apply((unsigned char *)ls, tsize, &LSG[k], 0);
        }
        return;
    }
}
/* Called once by the host with the guest's path, for every guest process. */
void a2x_lsgate_arm(const char *gpath) {
    const char *off = A2X_DBGENV("A2X_LSGATE");
    if (off && off[0] == '0') return;
    const char *b = gpath ? strrchr(gpath, '/') : 0;
    lsg_self = b ? b + 1 : (gpath ? gpath : "?");
    lsg_loud = !strcmp(lsg_self, "lsd") || !strcmp(lsg_self, "CoreServicesUIAgent");
    /* Arming only CoreServicesUIAgent gives 1 patch there, and whole-__TEXT scans refused in every other process (xpcproxy 2,300+ in 30 min) */
    /* Dock and other LaunchServices clients evaluate the gate too; arming only two processes brings the crossed icons back. xpcproxy never needs it. */
    if (!strcmp(lsg_self, "xpcproxy")) return;
    _dyld_register_func_for_add_image(lsg_added);
}

#ifdef LSG_SELFTEST
/* named arms, both directions: a site present once is patched to the exact fix; twice or never is refused unchanged. */
static int arm(const char *name, int copies, int want) {
    static unsigned char buf[1 << 16];
    memset(buf, 0x90, sizeof buf);
    for (int c = 0; c < copies; c++) memcpy(buf + 4096 + c * 8192, LSG_GATE, sizeof LSG_GATE);
    unsigned char before[sizeof buf]; memcpy(before, buf, sizeof buf);
    int r = lsg_apply(buf, sizeof buf, &LSG[0], 1);
    int changed = memcmp(before, buf, sizeof buf) != 0;
    int fixed = copies == 1 && !memcmp(buf + 4096 + 6, LSG_GATE_FIX, sizeof LSG_GATE_FIX)
                && !memcmp(buf + 4096, LSG_GATE, 6) && !memcmp(buf + 4096 + 14, LSG_GATE + 14, 4);
    int ok = (r == want) && (want ? fixed : !changed);
    printf("%s %s (r=%d changed=%d)\n", ok ? "ok  " : "FAIL", name, r, changed);
    return ok;
}
int main(void) {
    int f = 0;
    f += !arm("a gate site present ONCE is patched to any-slice, surrounding bytes untouched", 1, 1);
    f += !arm("a gate site present TWICE is refused and nothing changes", 2, 0);
    f += !arm("a LaunchServices with NO gate site is refused and nothing changes", 0, 0);
    {   /* both 27.2 arms land on the immediate byte: 0x80 becomes 0x88, 0x100 becomes 0x108, neighbours untouched */
        static unsigned char b[4096];
        for (int w = 0; w < 2; w++) {
            const struct lsg_patch *pt = w ? &LSG[4] : &LSG[2];
            memset(b, 0x90, sizeof b); memcpy(b + 64, pt->site, pt->n);
            int r = lsg_apply(b, sizeof b, pt, 1);
            int ok = r == 1 && (w ? (b[64 + 12] == 0x08 && b[64 + 13] == 0x01 && b[64 + 11] == 0xcf) : (b[64 + 18] == 0x88 && b[64 + 19] == 0 && b[64 + 17] == 0xcf));
            printf("%s %s\n", ok ? "ok  " : "FAIL", w ? "27.2 arm64e arm becomes or r15d,0x108" : "27.2 arm64 arm becomes or r15d,0x88");
            f += !ok;
        }
        f += !(LSG[2].alt == 1 && LSG[4].alt == 1 && !LSG[1].alt && !LSG[3].alt);
    }
    printf("%d failed\n", f);
    return f != 0;
}
#endif
