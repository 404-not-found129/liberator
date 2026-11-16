#ifndef A2X_SAFE_BLOCK_PROBE_H
#define A2X_SAFE_BLOCK_PROBE_H
#include <stdint.h>
extern unsigned int mach_task_self_;
extern int mach_vm_read_overwrite(unsigned int, uint64_t, uint64_t, uint64_t, uint64_t *);
extern const void *(*a2x_block_probe_view)(unsigned long);
/* An opaque collection key may be an aligned integer rather than readable memory. */
static inline int a2x_read_block_isa(uint64_t address, uint64_t *isa) {
    if (address < 4096 || (address & 7)) return 0;
    if (a2x_block_probe_view)
        address = (uint64_t)(uintptr_t)a2x_block_probe_view((unsigned long)address);
    uint64_t value = 0, copied = 0;
    int kr = mach_vm_read_overwrite(mach_task_self_, address, sizeof value,
                                    (uint64_t)(uintptr_t)&value, &copied);
    if (kr || copied != sizeof value) return 0;
    *isa = value;
    return 1;
}
#endif
