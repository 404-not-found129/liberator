/* Repairs the one broken personality reference in the x86_64 shared cache: the CIE that covers
   _dispatch_client_callout in libdispatch encodes its personality as an indirect pc-relative pointer (0x9b)
   whose offset is the unrelocated addend 4, so the "slot" is the CIE's own next bytes and libunwind calls
   garbage (0x20019008070c10) the first time an exception unwinds through a dispatch callout. Native x86_64
   processes crash the same way; the interpreter is not involved. The fix points the reference at a cell holding
   __objc_personality_v0. Runs once, before main, in every host process. */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static uint64_t uleb(const uint8_t **p) {
    uint64_t r = 0; int s = 0;
    for (;;) { uint8_t b = *(*p)++; r |= (uint64_t)(b & 0x7f) << s; s += 7; if (!(b & 0x80)) break; }
    return r;
}

/* A cell the 32-bit pc-relative field can reach: try pages alternately above and below the field. */
static uint64_t *cell_near(const uint8_t *field, void *value) {
    for (int t = 0; t < 400; t++) {
        mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)field + (t % 2 ? 1 : -1) * (0x1000000UL * (1 + t / 2));
        a &= ~0x3fffUL;
        if (mach_vm_allocate(mach_task_self(), &a, 0x4000, VM_FLAGS_FIXED) == KERN_SUCCESS) {
            uint64_t *c = (uint64_t *)(uintptr_t)a; *c = (uint64_t)(uintptr_t)value; return c;
        }
    }
    return 0;
}

__attribute__((constructor)) static void a2x_unwfix(void) {
    void *pers = dlsym(RTLD_DEFAULT, "__objc_personality_v0");
    if (!pers) return;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const struct mach_header_64 *mh = (const void *)_dyld_get_image_header(i);
        unsigned long sz = 0; const uint8_t *eh = getsectiondata(mh, "__TEXT", "__eh_frame", &sz);
        if (!eh) continue;
        const uint8_t *p = eh, *end = eh + sz;
        while (p + 8 <= end) {
            uint32_t len = *(const uint32_t *)p; if (!len) break;
            if (*(const uint32_t *)(p + 4) == 0) {
                const uint8_t *q = p + 8; uint8_t ver = *q++; const char *aug = (const char *)q; q += strlen(aug) + 1;
                if (aug[0] == 0x7a && aug[1] == 0x50) {
                    uleb(&q); uleb(&q); if (ver == 1) q++; else uleb(&q); uleb(&q);
                    uint8_t enc = *q++; const uint8_t *field = q;
                    if (enc == 0x9b) {
                        const uint8_t *slot = field + *(const int32_t *)field;
                        if (slot >= eh && slot < end) { /* personality slot inside __eh_frame itself: never valid */
                            uint64_t *c = cell_near(field, pers);
                            int64_t d = c ? (int64_t)(intptr_t)c - (int64_t)(intptr_t)field : 0;
                            if (c && d > INT32_MIN && d < INT32_MAX) {
                                mach_vm_address_t pg = (mach_vm_address_t)(uintptr_t)field & ~0x3fffUL;
                                if (mach_vm_protect(mach_task_self(), pg, 0x8000, 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) == KERN_SUCCESS) {
                                    *(int32_t *)(uintptr_t)field = (int32_t)d;
                                    mach_vm_protect(mach_task_self(), pg, 0x8000, 0, VM_PROT_READ | VM_PROT_EXECUTE);
                                }
                            }
                        }
                    }
                }
            }
            p += len + 4;
        }
    }
}
