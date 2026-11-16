// confollow.h - the boot console follows the GPU BAR that IOPCIConfigurator moves at boot (27.2, bare metal).
//
// Why this exists: with the GPU root port 0:6.0 unmasked, IOPCIConfigurator's boot pass wipes and re-places
// every host-level range that can sit above 4 GB (same code in 26: bridgeAllocateResources sets start 0 / minAddress
// 4G). The boot framebuffer is the GOP surface inside the GPU's BAR, so it moves with the window. Apple's protocol
// is: setConsoleInfo(kPEDisableScreen) before the pass, kPEBaseAddressChange (new base | 1) only if the configurator
// "hit" the console range, kPEEnableScreen after. Three ways that goes wrong on our platform, each handled here:
//   1. an earlier diagnostic build IGNORED kPEDisableScreen, so the text console (pci_log on: hundreds of lines) kept
//      drawing into a window that was being closed and moved;
//   2. PC00 and PC02 are two host bridges, each with its own configurator, and PE_initialize_console does not nest:
//      the FIRST enable turns the screen back on, so PC02 finishing early re-enables it in the middle of PC00's move;
//   3. if the configurator did not hit the console range (no op 8) the console is re-enabled at the OLD address,
//      which no longer decodes: blind for the rest of the boot.
// Fix: nest disable/enable (only the outermost pair reaches xnu), and at the outermost enable, if no base change
// arrived, re-read the BAR that held the screen at start and deliver the base change ourselves. If that device no
// longer decodes, the screen stays off (serial console) instead of drawing into nothing.
//
// Pure C, no kernel headers: compiled into the kext AND into confollow_selftest.c on the build host.
#pragma once
#include <stdint.h>

#define CF_OP_ENABLE   6u   /* kPEEnableScreen      (pexpert.h 27 SDK line 263) */
#define CF_OP_DISABLE  7u   /* kPEDisableScreen     (line 264) */
#define CF_OP_BASE     8u   /* kPEBaseAddressChange (line 265) */

typedef uint32_t (*cf_rd_t)(void *ctx, uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);

struct cf_loc {               // where the boot screen lived when the host bridge started (before any configure)
    int      valid, viaBridge;
    uint8_t  rootBus, bdev, bfn;   // the bridge on the root bus whose window held the screen (viaBridge)
    uint8_t  dev, fn, bar;         // the function and BAR (index of the low dword) holding it
    uint32_t bid, id;              // vendor/device dwords, re-checked after the move
    uint64_t off;                  // screen offset inside the BAR (the GOP surface does not move inside VRAM)
};

// Memory BAR i of a type-0 function; *next = index after it (64-bit BARs take two). 0 = I/O, unassigned or bad.
static inline uint64_t cf_bar(cf_rd_t rd, void *c, uint8_t b, uint8_t d, uint8_t f, unsigned i, unsigned *next)
{
    uint32_t lo = rd(c, b, d, f, (uint8_t)(0x10 + 4 * i));
    *next = i + 1;
    if (lo == 0xffffffffu || (lo & 1)) return 0;
    uint64_t base = lo & ~0xfULL;
    if (((lo >> 1) & 3) == 2) {
        if (i >= 5) return 0;
        base |= (uint64_t)rd(c, b, d, f, (uint8_t)(0x14 + 4 * i)) << 32;
        *next = i + 2;
    }
    return base;
}

// A BAR is naturally aligned to its size, so a BAR at `base` is at most 2^ctz(base) bytes. Necessary, not
// sufficient; we never size a live BAR (that writes all ones into the GPU the screen is coming from).
static inline int cf_may_hold(uint64_t base, uint64_t fb)
{
    if (!base || fb < base) return 0;
    uint64_t maxsz = base & (~base + 1);
    return (fb - base) < maxsz;
}

// Bridge memory (0x20) or prefetchable (0x24/0x28/0x2c) window contains addr.
static inline int cf_win_has(cf_rd_t rd, void *c, uint8_t b, uint8_t d, uint8_t f, uint64_t addr)
{
    uint32_t v = rd(c, b, d, f, 0x20);
    uint64_t lo = (uint64_t)(v & 0xfff0) << 16, hi = ((uint64_t)(v >> 16 & 0xfff0) << 16) | 0xfffff;
    if (lo <= hi && addr >= lo && addr <= hi) return 1;
    v = rd(c, b, d, f, 0x24);
    lo = (uint64_t)(v & 0xfff0) << 16; hi = ((uint64_t)(v >> 16 & 0xfff0) << 16) | 0xfffff;
    if ((v & 0xf) == 1) { lo |= (uint64_t)rd(c, b, d, f, 0x28) << 32; hi |= (uint64_t)rd(c, b, d, f, 0x2c) << 32; }
    return lo <= hi && addr >= lo && addr <= hi;
}

// Best BAR of a type-0 function that may hold fb: the highest base at or below it.
static inline int cf_pick_bar(cf_rd_t rd, void *c, uint8_t b, uint8_t d, uint8_t f, uint64_t fb,
                              uint8_t *bar, uint64_t *base)
{
    if (((rd(c, b, d, f, 0x0c) >> 16) & 0x7f) != 0) return 0;
    uint64_t best = 0;
    for (unsigned i = 0, n; i < 6; i = n) {
        uint64_t bb = cf_bar(rd, c, b, d, f, i, &n);
        if (cf_may_hold(bb, fb) && bb > best) { best = bb; *bar = (uint8_t)i; }
    }
    if (!best) return 0;
    *base = best;
    return 1;
}

// Scan one bus for the function whose BAR holds fb; fills dev/fn/bar/id/off. Highest base wins.
static inline int cf_scan_bus(cf_rd_t rd, void *c, uint8_t b, uint64_t fb, struct cf_loc *l)
{
    uint64_t best = 0;
    for (unsigned d = 0; d < 32; d++)
        for (unsigned f = 0; f < 8; f++) {
            uint32_t id = rd(c, b, (uint8_t)d, (uint8_t)f, 0x00);
            if ((id & 0xffff) == 0xffff) { if (f == 0) break; continue; }
            uint8_t bar = 0; uint64_t base = 0;
            if (cf_pick_bar(rd, c, b, (uint8_t)d, (uint8_t)f, fb, &bar, &base) && base > best) {
                best = base; l->dev = (uint8_t)d; l->fn = (uint8_t)f; l->bar = bar; l->id = id; l->off = fb - base;
            }
            if (f == 0 && !((rd(c, b, (uint8_t)d, 0, 0x0c) >> 16) & 0x80)) break;   // single-function device
        }
    return best != 0;
}

// Find where fb lives below root bus `rb`: behind a root-bus bridge whose window holds it, else a root-bus function.
static inline int cf_locate(cf_rd_t rd, void *c, uint8_t rb, uint64_t fb, struct cf_loc *l)
{
    struct cf_loc z = {0};
    *l = z;
    if (!fb) return 0;
    for (unsigned d = 0; d < 32; d++)
        for (unsigned f = 0; f < 8; f++) {
            uint32_t id = rd(c, rb, (uint8_t)d, (uint8_t)f, 0x00);
            if ((id & 0xffff) == 0xffff) { if (f == 0) break; continue; }
            uint32_t hdr = (rd(c, rb, (uint8_t)d, (uint8_t)f, 0x0c) >> 16) & 0xff;
            if ((hdr & 0x7f) == 1 && cf_win_has(rd, c, rb, (uint8_t)d, (uint8_t)f, fb)) {
                uint8_t sec = (uint8_t)(rd(c, rb, (uint8_t)d, (uint8_t)f, 0x18) >> 8);
                if (sec > rb && cf_scan_bus(rd, c, sec, fb, l)) {
                    l->valid = 1; l->viaBridge = 1; l->rootBus = rb; l->bdev = (uint8_t)d; l->bfn = (uint8_t)f; l->bid = id;
                    return 1;
                }
            }
            if (f == 0 && !(hdr & 0x80)) break;
        }
    if (cf_scan_bus(rd, c, rb, fb, l)) { l->valid = 1; l->viaBridge = 0; l->rootBus = rb; return 1; }
    *l = z;
    return 0;
}

// Where the screen is NOW. 1 = found and decoding (*phys set), 0 = tracked but gone / not decoding, -1 = not tracked.
static inline int cf_where(cf_rd_t rd, void *c, const struct cf_loc *l, uint64_t *phys)
{
    if (!l || !l->valid) return -1;
    uint8_t bus = l->rootBus;
    if (l->viaBridge) {
        if (rd(c, l->rootBus, l->bdev, l->bfn, 0x00) != l->bid) return 0;
        if (!(rd(c, l->rootBus, l->bdev, l->bfn, 0x04) & 2)) return 0;           // bridge not forwarding memory
        bus = (uint8_t)(rd(c, l->rootBus, l->bdev, l->bfn, 0x18) >> 8);          // the boot pass may renumber it
        if (bus <= l->rootBus) return 0;
    }
    if (rd(c, bus, l->dev, l->fn, 0x00) != l->id) return 0;
    if (!(rd(c, bus, l->dev, l->fn, 0x04) & 2)) return 0;                         // memory decode off
    unsigned n;
    uint64_t base = cf_bar(rd, c, bus, l->dev, l->fn, l->bar, &n);
    if (!base) return 0;
    uint64_t p = base + l->off;
    if (l->viaBridge && !cf_win_has(rd, c, l->rootBus, l->bdev, l->bfn, p)) return 0;
    *phys = p;
    return 1;
}

// ---- the nesting policy ----
struct cf_state {
    int      depth;        // open disable requests (two configurators, NVRMFB)
    int      sawBase;      // a base change arrived inside the current outermost disable
    int      live;         // a2x_conlive=1: an earlier diagnostic (drop disables, keep drawing). Off by default.
    uint64_t cur;          // console physical base as xnu knows it (flag bits cleared)
};
enum {
    CF_FORWARD     = 1,    // pass this op to IOPlatformExpert::setConsoleInfo
    CF_BASE_FIRST  = 2,    // deliver kPEBaseAddressChange(*newBase | 1) before forwarding
    CF_KEEP_OFF    = 4,    // swallow the enable: the screen's device does not decode
    CF_CRUMB_PAUSE = 8,    // stop drawing breadcrumbs into the window being moved
    CF_CRUMB_GO    = 16,   // draw again (after any rebase)
    CF_CRUMB_MOVE  = 32,   // re-map breadcrumbs to the new base
};
typedef int (*cf_where_fn)(void *ctx, uint64_t *phys);

static inline unsigned cf_step(struct cf_state *s, unsigned op, uint64_t infoBase,
                               cf_where_fn where, void *wctx, uint64_t *newBase)
{
    switch (op) {
    case CF_OP_DISABLE:
        if (s->live) return 0;
        if (s->depth++ == 0) { s->sawBase = 0; return CF_FORWARD | CF_CRUMB_PAUSE; }
        return 0;                                       // already off: nested
    case CF_OP_BASE:
        if (infoBase) { s->sawBase = 1; s->cur = infoBase & ~3ULL; *newBase = s->cur; return CF_FORWARD | CF_CRUMB_MOVE; }
        return CF_FORWARD;
    case CF_OP_ENABLE:
        if (s->depth == 0) return CF_FORWARD | CF_CRUMB_GO;   // nobody disabled through us (or live mode)
        if (--s->depth > 0) return 0;                   // another configure pass still has the screen off
        if (!s->sawBase && where) {
            uint64_t p = 0;
            int rc = where(wctx, &p);
            if (rc > 0 && p != s->cur) { s->cur = p; *newBase = p; return CF_BASE_FIRST | CF_CRUMB_MOVE | CF_FORWARD | CF_CRUMB_GO; }
            if (rc == 0) return CF_KEEP_OFF;
        }
        return CF_FORWARD | CF_CRUMB_GO;
    default:
        return CF_FORWARD;
    }
}

#ifdef __cplusplus
extern "C" {
#endif
// Kernel side (pcibridge.cpp): where the screen is now, through the host bridge that found it at start.
int  a2x_console_where(void *unused, uint64_t *phys);
#ifdef __cplusplus
}
#endif
