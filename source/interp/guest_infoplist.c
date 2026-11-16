/* guest_infoplist.c — give a non-bundle guest its own embedded Info.plist.
 *
 * reuses: CoreFoundation's own rule (a main bundle with an empty info dictionary takes the one embedded in the
 *   executable); ws_quartz.c's arm-from-main shape; slice_arm64() supplies the guest slice.
 * new-because: on an unmodified macOS 27 system, WindowServer exits 139, ud2 in +[BSServiceConnectionListener
 *   _listenerWithManager:configuration:handler:], "failed to find domain 'BKSHIDServices'" (BSServiceConnectionListener.m:194).
 *
 * The bug: CF reads __TEXT,__info_plist from the x86_64 slice of the running file, which is our host and has none.
 *    Apple's copy sits in the arm64e slice, so every daemon's main bundle was empty: no id, no BSServiceDomains.
 *    An older-release x86 WindowServer carried its own copy, so this only appears once that part is removed.
 * Checked on an x86_64 process with the real x86 BoardServices and the same call: no plist = the identical
 *    :194 failure; plist merged = "initializing automatic domain BKSHIDServices"; dict class __NSDictionaryM.
 * BSServiceDomains only. Merging for every guest with a section hung boot: all vCPUs idle, no panic,
 *    serial silent from spawn 61 (logd, xpcproxy, and trustd all carry a plist and now called CF at start). Cause within CF
 *    not proven; the trigger is limited to the key the x86 frameworks are known to need.
 * Empty dict only: a bundle with a real Info.plist wins, exactly as in CF. Never pid 1: launchd has no use for it
 *    and a CF surprise there takes the machine down. A guest with no section does not touch CF at all.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/loader.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const uint8_t *find_section(const uint8_t *m, size_t len, size_t *out) {
    if (len < sizeof(struct mach_header_64)) return 0;
    const struct mach_header_64 *mh = (const void *)m;
    if (mh->magic != MH_MAGIC_64 || (size_t)mh->sizeofcmds > len - sizeof *mh) return 0;
    const uint8_t *p = m + sizeof *mh, *end = p + mh->sizeofcmds;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if ((size_t)(end - p) < sizeof(struct load_command)) return 0;
        const struct load_command *lc = (const void *)p;
        if (lc->cmdsize < sizeof *lc || lc->cmdsize > (size_t)(end - p)) return 0;
        if (lc->cmd == LC_SEGMENT_64 && lc->cmdsize >= sizeof(struct segment_command_64)) {
            const struct segment_command_64 *sg = (const void *)p;
            if (strncmp(sg->segname, "__TEXT", 16) == 0 &&
                (lc->cmdsize - sizeof *sg) / sizeof(struct section_64) >= sg->nsects) {
                const struct section_64 *s = (const void *)(sg + 1);
                for (uint32_t j = 0; j < sg->nsects; j++)
                    if (strncmp(s[j].sectname, "__info_plist", 16) == 0) {
                        if (s[j].offset > len || s[j].size > len - s[j].offset) return 0;
                        *out = (size_t)s[j].size;
                        return m + s[j].offset;
                    }
            }
        }
        p += lc->cmdsize;
    }
    return 0;
}

static void add_absent(const void *k, const void *v, void *ctx) {
    if (!CFDictionaryContainsKey((CFDictionaryRef)ctx, k)) CFDictionarySetValue((CFMutableDictionaryRef)ctx, k, v);
}

void a2x_guest_infoplist(const uint8_t *macho, size_t len) {
    size_t n = 0;
    const uint8_t *x = find_section(macho, len, &n);
    if (!x || !n || getpid() == 1) return;
    /* the carriers known to need their plist: BSServiceDomains (WindowServer) and CMIOExtension
          (camera extensions: +[CMIOExtensionProvider startServiceWithProvider:] throws "CMIOExtension information
          dictionary missing from the Info.plist"). Still never every guest (that hung boot, see above). */
    const char *key = memmem(x, n, "BSServiceDomains", 16) ? "BSServiceDomains"
                    : memmem(x, n, "CMIOExtension", 13) ? "CMIOExtension" : 0;
    if (!key) return;
    CFBundleRef b = CFBundleGetMainBundle();
    CFDictionaryRef d = b ? CFBundleGetInfoDictionary(b) : 0;
    if (!d) { fprintf(stderr, "[host] guest plist: no main bundle dictionary\n"); return; }
    if (CFDictionaryGetCount(d) != 0) return;
    Class M = objc_getClass("NSMutableDictionary");
    if (!M || !((char (*)(id, SEL, Class))objc_msgSend)((id)d, sel_registerName("isKindOfClass:"), M)) {
        fprintf(stderr, "[host] guest plist: main bundle dictionary is immutable, %zu-byte plist NOT applied\n", n);
        return;
    }
    CFDataRef data = CFDataCreate(0, x, (CFIndex)n);
    CFErrorRef err = 0;
    CFPropertyListRef p = data ? CFPropertyListCreateWithData(0, data, kCFPropertyListImmutable, 0, &err) : 0;
    if (data) CFRelease(data);
    if (!p || CFGetTypeID(p) != CFDictionaryGetTypeID()) {
        fprintf(stderr, "[host] guest plist: %zu bytes did not parse as a dictionary\n", n);
        if (err) CFRelease(err);
        if (p) CFRelease(p);
        return;
    }
    CFDictionaryApplyFunction((CFDictionaryRef)p, add_absent, (void *)d);
    CFRelease(p);
    fprintf(stderr, "[host] guest plist: applied %zu bytes, %s present\n", n, key);
}
