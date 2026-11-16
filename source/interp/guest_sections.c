/* guest_sections.c — make the guest's identity segments visible to main-executable section lookups.
 *
 * reuses: guest_infoplist.c's arm point in host.c and its bounded load-command walk; slice_arm64() supplies the slice.
 * new-because: on an unmodified macOS 27 system, WindowServer hit SIGABRT at SkyBridge.m:41 "this functionality can only be
 *   used from the WindowServer process". FURunningInWindowServer is getsectbyname("__SLSERVER", "__slserver") != NULL.
 *
 * The bug: getsectbyname walks the running image's x86_64 header, which is our host. Apple's __SLSERVER and
 *    __CGSERVER sit in the arm64e slice, so a stock WindowServer did not look like WindowServer to the x86 frameworks.
 *    Same class as __TEXT,__info_plist: identity carried in Mach-O sections that only the guest slice holds.
 * __interpose in the host does not work: dyld ignores it in a main executable (armed = 0).
 * So the host appends those segments to its own in-memory header. Only read-only guest segments the host lacks are
 *    copied, never __LINKEDIT. A guest with none of them never touches the page. dyld read this header before main,
 *    so its image bookkeeping does not change. Section addresses point at the bytes in the guest file map.
 * The reason rides back: every refusal prints one [host] line and leaves the header exactly as it was.
 */
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const struct mach_header_64 *_NSGetMachExecuteHeader(void);

#define A2X_MAX_ADD 4096 /* chosen: far above two 1-section segments, and bounds the stack copy */

/* First byte in the host image that is not header: load commands may grow only up to here. */
static uint64_t first_content_off(const struct mach_header_64 *h) {
    const uint8_t *p = (const uint8_t *)(h + 1);
    uint64_t low = UINT64_MAX;
    for (uint32_t i = 0; i < h->ncmds; i++, p += ((const struct load_command *)p)->cmdsize) {
        const struct segment_command_64 *s = (const void *)p;
        if (s->cmd != LC_SEGMENT_64 || strncmp(s->segname, "__TEXT", 16)) continue;
        const struct section_64 *c = (const void *)(s + 1);
        for (uint32_t k = 0; k < s->nsects; k++) if (c[k].offset && c[k].offset < low) low = c[k].offset;
    }
    return low;
}

static int host_has_seg(const struct mach_header_64 *h, const char *name) {
    const uint8_t *p = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++, p += ((const struct load_command *)p)->cmdsize) {
        const struct segment_command_64 *s = (const void *)p;
        if (s->cmd == LC_SEGMENT_64 && !strncmp(s->segname, name, 16)) return 1;
    }
    return 0;
}

void a2x_guest_sections(const uint8_t *macho, size_t len) {
    if (!macho || len < sizeof(struct mach_header_64)) return;
    const struct mach_header_64 *g = (const void *)macho;
    const struct mach_header_64 *h = _NSGetMachExecuteHeader();
    if (g->magic != MH_MAGIC_64 || !h || h->magic != MH_MAGIC_64) return;

    uint8_t add[A2X_MAX_ADD];
    size_t used = 0, off = sizeof(*g);
    uint32_t nseg = 0;
    for (uint32_t i = 0; i < g->ncmds; i++) {
        if (off + sizeof(struct load_command) > len) return;
        const struct load_command *lc = (const void *)(macho + off);
        if (lc->cmdsize < sizeof(*lc) || off + lc->cmdsize > len) return;
        const struct segment_command_64 *s = (const void *)lc;
        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(*s) && s->initprot == VM_PROT_READ &&
            strncmp(s->segname, "__LINKEDIT", 16) && !host_has_seg(h, s->segname) &&
            (size_t)s->nsects == (lc->cmdsize - sizeof(*s)) / sizeof(struct section_64) &&
            (uint64_t)s->fileoff + s->filesize <= len && used + lc->cmdsize <= sizeof add) {
            struct segment_command_64 *ns = (void *)(add + used);
            memcpy(ns, s, lc->cmdsize);
            ns->vmaddr = (uint64_t)(uintptr_t)(macho + s->fileoff);
            struct section_64 *c = (void *)(ns + 1);
            for (uint32_t k = 0; k < ns->nsects; k++) {
                if ((uint64_t)c[k].offset + c[k].size > len) return;
                c[k].addr = (uint64_t)(uintptr_t)(macho + c[k].offset);
            }
            used += lc->cmdsize;
            nseg++;
        }
        off += lc->cmdsize;
    }
    if (!nseg) return;

    size_t end = sizeof(*h) + h->sizeofcmds;
    uint64_t room = first_content_off(h);
    if (room == UINT64_MAX || end + used > room) {
        fprintf(stderr, "[host] guest sections: no header room (end %zu + %zu > %llu), %u segment(s) not added\n",
                end, used, (unsigned long long)room, nseg);
        return;
    }
    vm_address_t page = trunc_page((vm_address_t)h);
    vm_size_t size = round_page((vm_address_t)h + end + used) - page;
    kern_return_t kr = vm_protect(mach_task_self(), page, size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "[host] guest sections: vm_protect %d, %u segment(s) not added\n", kr, nseg);
        return;
    }
    struct mach_header_64 *w = (struct mach_header_64 *)h;
    memcpy((uint8_t *)w + end, add, used);
    w->sizeofcmds += (uint32_t)used;
    w->ncmds += nseg; /* last: a concurrent walker sees the old count until the commands exist */
    /* The host links with -headerpad so no code shares these pages: read-only then, and a dirtied page is never executed. */
    kr = vm_protect(mach_task_self(), page, size, FALSE, room < size ? (VM_PROT_READ | VM_PROT_EXECUTE) : VM_PROT_READ);
    fprintf(stderr, "[host] guest sections: added %u segment(s), %zu bytes, reprotect %d\n", nseg, used, kr);
}

/* The swift @main of an extension is found by section, in __TEXT. ExtensionFoundation (EXExtensionMain) reads
      the main image's __TEXT,__swift5_entry: under us that is this host, which has none, so every ExtensionKit/WidgetKit
      extension logged "SwiftUI: Swift entry point data not found" and then trapped (ud2 at -[_EXConnectionHandlerExtension
      willFinishLaunching]+0x43a, "Failed to instantiate connection handler class: (null) principal class: (null)") —
      ~25 widget/control extensions per login, and no extension ever ran.
   The host already has a __TEXT, so the segment pass above skips it. This adds a second, read-only segment named __TEXT
      holding only the guest's entry section(s), pointing at the guest's loaded copy (the executable's slide is 0, so the
      file's section addr is where it is mapped): the int32 in __swift5_entry is relative to its own address, and resolving it
      against the file map would land in the file, not in code. getsectbyname/getsectiondata walk every segment of that
      name; the first __TEXT (the host's, fileoff 0) still decides the slide, which is 0 for this -no_pie host. */
static const char *const kEntrySects[] = { "__swift5_entry", 0 };
void a2x_guest_text_entry(const uint8_t *macho, size_t len, uint64_t slide) {
    if (!macho || len < sizeof(struct mach_header_64)) return;
    const struct mach_header_64 *g = (const void *)macho;
    const struct mach_header_64 *h = _NSGetMachExecuteHeader();
    if (g->magic != MH_MAGIC_64 || !h || h->magic != MH_MAGIC_64) return;
    struct { struct segment_command_64 s; struct section_64 c[4]; } seg;
    memset(&seg, 0, sizeof seg);
    uint32_t n = 0;
    size_t off = sizeof(*g);
    for (uint32_t i = 0; i < g->ncmds && n < 4; i++) {
        if (off + sizeof(struct load_command) > len) return;
        const struct load_command *lc = (const void *)(macho + off);
        if (lc->cmdsize < sizeof(*lc) || off + lc->cmdsize > len) return;
        const struct segment_command_64 *s = (const void *)lc;
        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(*s) && !strncmp(s->segname, "__TEXT", 16) &&
            (size_t)s->nsects == (lc->cmdsize - sizeof(*s)) / sizeof(struct section_64)) {
            const struct section_64 *c = (const void *)(s + 1);
            for (uint32_t k = 0; k < s->nsects && n < 4; k++)
                for (int q = 0; kEntrySects[q]; q++)
                    if (!strncmp(c[k].sectname, kEntrySects[q], 16) && c[k].size && c[k].size < 4096) {
                        seg.c[n] = c[k];
                        seg.c[n].addr = c[k].addr + slide;
                        n++;
                    }
        }
        off += lc->cmdsize;
    }
    if (!n) return;
    /* the host must not already answer for these (a rebuilt host with its own entry would win anyway, being first) */
    const uint8_t *p = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++, p += ((const struct load_command *)p)->cmdsize) {
        const struct segment_command_64 *s = (const void *)p;
        if (s->cmd != LC_SEGMENT_64) continue;
        const struct section_64 *c = (const void *)(s + 1);
        for (uint32_t k = 0; k < s->nsects; k++)
            if (!strncmp(c[k].sectname, kEntrySects[0], 16) && !strncmp(c[k].segname, "__TEXT", 16)) {
                fprintf(stderr, "[host] guest entry: the header already has __TEXT,%s; left as is\n", kEntrySects[0]);
                return;
            }
    }
    uint64_t lo = seg.c[0].addr, hi = seg.c[0].addr + seg.c[0].size;
    for (uint32_t k = 1; k < n; k++) {
        if (seg.c[k].addr < lo) lo = seg.c[k].addr;
        if (seg.c[k].addr + seg.c[k].size > hi) hi = seg.c[k].addr + seg.c[k].size;
    }
    seg.s.cmd = LC_SEGMENT_64;
    seg.s.cmdsize = (uint32_t)(sizeof seg.s + n * sizeof(struct section_64));
    strncpy(seg.s.segname, "__TEXT", 16);
    /* libmacho getsectiondata sets slide = mh - vmaddr at every segment named __TEXT, the last one
          passed wins, so vmaddr = the guest section returned the host's base (0x400000000). This segment carries the host
          __TEXT's own vmaddr and no size: the slide stays the host's, and the sections keep their absolute addresses. */
    { const struct segment_command_64 *ht = getsegbyname("__TEXT"); seg.s.vmaddr = ht ? ht->vmaddr : (uint64_t)(uintptr_t)h; }
    (void)lo; (void)hi;
    seg.s.vmsize = 0; seg.s.fileoff = seg.c[0].offset; seg.s.filesize = 0;
    seg.s.maxprot = seg.s.initprot = VM_PROT_READ;
    seg.s.nsects = n;
    size_t used = seg.s.cmdsize;
    size_t end = sizeof(*h) + h->sizeofcmds;
    uint64_t room = first_content_off(h);
    if (room == UINT64_MAX || end + used > room) {
        fprintf(stderr, "[host] guest entry: no header room (end %zu + %zu > %llu)\n", end, used, (unsigned long long)room);
        return;
    }
    vm_address_t page = trunc_page((vm_address_t)h);
    vm_size_t size = round_page((vm_address_t)h + end + used) - page;
    kern_return_t kr = vm_protect(mach_task_self(), page, size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS) { fprintf(stderr, "[host] guest entry: vm_protect %d, not added\n", kr); return; }
    struct mach_header_64 *w = (struct mach_header_64 *)h;
    memcpy((uint8_t *)w + end, &seg, used);
    w->sizeofcmds += (uint32_t)used;
    w->ncmds += 1;
    kr = vm_protect(mach_task_self(), page, size, FALSE, room < size ? (VM_PROT_READ | VM_PROT_EXECUTE) : VM_PROT_READ);
    if (kr != KERN_SUCCESS || A2X_DBGENV("ARM2X86_TRACE_SECTS"))   /* every Swift-@main guest takes this: quiet when it works */
        fprintf(stderr, "[host] guest entry: added __TEXT,%s at %#llx, reprotect %d\n", kEntrySects[0], (unsigned long long)lo, kr);
}
