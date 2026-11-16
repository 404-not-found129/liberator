// crumb.cpp - lock-free boot breadcrumbs drawn straight into the boot framebuffer (diagnostic build).
//
// Why: 27.2 on a test machine hangs with CPU 0 unreachable even by an HPET NMI, so neither the console (locks, a ring
// that drains in chunks) nor a panic can say where it stopped. Every platform step writes two hex lines into a
// box at the top-right of the screen with plain uncached stores: line 1 = the current code (site << 24 | detail),
// line 2 = a running count. Each site writes ENTER before the access and EXIT after it, so a frozen box shows the
// access that never completed. Sites: 10/11 config read enter/exit, 12/13 config write, 20/21 ACPI port read,
// 22/23 ACPI port write, 24/25 ACPI memory read, 26/27 ACPI memory write, 30/31 interrupt, 40/41 platform function.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "crumb.h"

static volatile uint32_t *gFB;
static uint32_t gStride, gW;           // stride in pixels
static volatile uint32_t gSeq;
static IOMemoryMap *gMap;
static uint32_t gRowBytes;
static uint32_t gShown[2];             // value on screen per line (valid once gShownOK)
static bool     gShownOK;
static volatile bool gPaused;        // the console is off: the window may be closed or moving under gFB
extern "C" boolean_t ml_set_interrupts_enabled(boolean_t enable);

// 3x5 hex font, one row per 3 bits (MSB = left)
static const uint8_t kFont[16][5] = {
    {7,5,5,5,7},{2,6,2,2,7},{7,1,7,4,7},{7,1,7,1,7},{5,5,7,1,1},{7,4,7,1,7},{7,4,7,5,7},{7,1,1,1,1},
    {7,5,7,5,7},{7,5,7,1,7},{7,5,7,5,5},{6,5,6,5,6},{7,4,4,4,7},{6,5,5,5,6},{7,4,7,4,7},{7,4,7,4,4}};
enum { kScale = 3, kGlyphW = 3 * kScale, kGlyphH = 5 * kScale, kGap = kScale, kDigits = 8,
       kBoxW = kDigits * (kGlyphW + kGap) + kGap, kBoxH = 2 * (kGlyphH + kGap) + kGap,
       kRingN = 8, kRingY = 4 + kBoxH + 8, kRingRowH = kGlyphH + kGap,
       kRingW = 2 * kBoxW + kGlyphW + kGap, kMapH = kRingY + kRingN * kRingRowH + 8 };
static uint32_t gRing[kRingN][2], gRingShown[kRingN][2];
static bool     gRingShownOK[kRingN];

static void drawLine(uint32_t value, uint32_t prev, bool prevOK, uint32_t y0, uint32_t x0, uint32_t fg)
{
    for (int d = 0; d < kDigits; d++) {
        uint32_t nib = (value >> ((kDigits - 1 - d) * 4)) & 0xf;
        if (prevOK && nib == ((prev >> ((kDigits - 1 - d) * 4)) & 0xf)) continue;   // unchanged digit
        uint32_t gx = x0 + kGap + d * (kGlyphW + kGap);
        for (int r = 0; r < 5; r++) {
            uint8_t bits = kFont[nib][r];
            for (int sy = 0; sy < kScale; sy++) {
                volatile uint32_t *row = gFB + (size_t)(y0 + r * kScale + sy) * gStride + gx;
                for (int c = 0; c < 3; c++) {
                    uint32_t px = (bits >> (2 - c)) & 1 ? fg : 0x00000000u;
                    for (int sx = 0; sx < kScale; sx++) row[c * kScale + sx] = px;
                }
            }
        }
    }
}

extern "C" void a2x_crumb_init(uint64_t fbPhys, uint32_t rowBytes, uint32_t width, uint32_t height, uint32_t depth)
{
    if (!fbPhys || depth != 32 || width < kRingW + kGlyphW + 16 || height < kMapH) {
        IOLog("ACPI: crumbs off (fb 0x%llx %ux%u depth %u)\n", fbPhys, width, height, depth); return;
    }
    // kIOMemoryMapperNone: this runs inside the platform expert's own start, before the system DMA mapper (VT-d)
    // exists; a mapper-backed descriptor waits for it (IOMapper::waitForSystemMapper) and panicked a test machine.
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)fbPhys,
                                 (mach_vm_size_t)rowBytes * kMapH, kIODirectionInOut | kIOMemoryMapperNone, TASK_NULL);
    if (md) { gMap = md->map(kIOMapInhibitCache); md->release(); }
    if (!gMap) { IOLog("ACPI: crumbs off (framebuffer not mappable)\n"); return; }
    gStride = rowBytes / 4; gW = width; gRowBytes = rowBytes;
    gFB = (volatile uint32_t *)gMap->getVirtualAddress();
    IOLog("ACPI: crumbs on: framebuffer 0x%llx %ux%u, box at top right (code / count)\n", fbPhys, width, height);
    a2x_crumb(0xC0DE0000u);
}

extern "C" void a2x_crumb(uint32_t code)
{
    volatile uint32_t *fb = gFB;
    if (!fb) return;
    if (gPaused) { gShown[0] = code; gShownOK = false; gSeq++; return; }   // drawn again on resume
    boolean_t ie = ml_set_interrupts_enabled(FALSE);           // one whole crumb at a time (see header)
    uint32_t seq = ++gSeq;
    uint32_t x0 = gW - kBoxW - 4;
    drawLine(code, gShown[0], gShownOK, 4 + kGap, x0, 0x00ffffffu);                 // white: the code
    drawLine(seq, gShown[1], gShownOK, 4 + kGap + kGlyphH + kGap, x0, 0x0000ff00u); // green: the count
    gShown[0] = code; gShown[1] = seq; gShownOK = true;
    __asm__ volatile("sfence" ::: "memory");
    (void)ml_set_interrupts_enabled(ie);
}

// ---- ring of the last kRingN config writes (newest at the bottom) ----
static void ringMarker(uint32_t color)
{
    uint32_t x0 = gW - kRingW - 4 - kGlyphW - kGap, y0 = kRingY + (kRingN - 1) * kRingRowH;
    for (int y = 0; y < kGlyphH; y++)
        for (int x = 0; x < kGlyphW; x++) gFB[(size_t)(y0 + y) * gStride + x0 + x] = color;
}

extern "C" void a2x_crumb_ring(uint32_t a, uint32_t b)
{
    if (!gFB) return;
    if (gPaused) {                                             // keep the history, draw nothing
        for (int i = 0; i < kRingN - 1; i++) { gRing[i][0] = gRing[i + 1][0]; gRing[i][1] = gRing[i + 1][1]; }
        gRing[kRingN - 1][0] = a; gRing[kRingN - 1][1] = b;
        return;
    }
    boolean_t ie = ml_set_interrupts_enabled(FALSE);
    for (int i = 0; i < kRingN - 1; i++) { gRing[i][0] = gRing[i + 1][0]; gRing[i][1] = gRing[i + 1][1]; }
    gRing[kRingN - 1][0] = a; gRing[kRingN - 1][1] = b;
    uint32_t xa = gW - kRingW - 4, xb = xa + kBoxW + kGlyphW;
    for (int i = 0; i < kRingN; i++) {
        uint32_t y = kRingY + i * kRingRowH;
        drawLine(gRing[i][0], gRingShown[i][0], gRingShownOK[i], y, xa, 0x00ffffffu);
        drawLine(gRing[i][1], gRingShown[i][1], gRingShownOK[i], y, xb, 0x0000ffffu);
        gRingShown[i][0] = gRing[i][0]; gRingShown[i][1] = gRing[i][1]; gRingShownOK[i] = true;
    }
    ringMarker(0x00ffff00u);                                   // yellow: issued
    __asm__ volatile("sfence" ::: "memory");
    (void)ml_set_interrupts_enabled(ie);
}

extern "C" void a2x_crumb_ring_done(void)
{
    if (!gFB || gPaused) return;
    boolean_t ie = ml_set_interrupts_enabled(FALSE);
    ringMarker(0x0000ff00u);                                   // green: returned
    __asm__ volatile("sfence" ::: "memory");
    (void)ml_set_interrupts_enabled(ie);
}

// ---- console relocation: map the new base and force a full redraw ----
extern "C" void a2x_crumb_rebase(uint64_t fbPhys)
{
    if (!gFB || !fbPhys || !gRowBytes) return;
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)fbPhys,
                                 (mach_vm_size_t)gRowBytes * kMapH, kIODirectionInOut | kIOMemoryMapperNone, TASK_NULL);
    IOMemoryMap *m = md ? md->map(kIOMapInhibitCache) : NULL;
    if (md) md->release();
    if (!m) { IOLog("ACPI: crumbs could not follow the console to 0x%llx\n", fbPhys); return; }
    boolean_t ie = ml_set_interrupts_enabled(FALSE);
    gFB = (volatile uint32_t *)m->getVirtualAddress();       // the old map is kept: a late draw may still hold it
    gMap = m;
    gShownOK = false;
    for (int i = 0; i < kRingN; i++) gRingShownOK[i] = false;
    (void)ml_set_interrupts_enabled(ie);
    IOLog("ACPI: crumbs follow the console to 0x%llx\n", fbPhys);
    a2x_crumb(0xC0DE0001u);
}

// Screen off -> on. On resume every digit is redrawn (the window may have moved: see a2x_crumb_rebase).
extern "C" void a2x_crumb_pause(int on)
{
    if (on) { gPaused = true; return; }
    if (!gPaused) return;
    gShownOK = false;
    for (int i = 0; i < kRingN; i++) gRingShownOK[i] = false;
    gPaused = false;
    uint32_t last = gShown[0];
    a2x_crumb(last ? last : 0xC0DE0002u);
}

extern "C" uint32_t a2x_crumb_seq(uint32_t *lastCode)
{
    if (lastCode) *lastCode = gShown[0];
    return gSeq;
}
