// vtdmap.cpp - the VT-d DMA mapper for network controllers on macOS 27.2 Intel.
//
// new-because: the Mac Pro's Broadcom Ethernet (AppleBCM5701Ethernet) allocates a 384 KB receive ring from ordinary,
// scattered kernel memory and asks IODMACommand for ONE device segment covering it, through the mapper it gets from
// IOMapper::copyMapperForDevice(its PCI nub). With no mapper the ring is several physical runs, the driver logs
// "MyAllocIOBuffer - failed to get physical address" and start() fails on both ports (Mac Pro, macOS 27.2).
// macOS 26 answers this with AppleVTD as the system mapper; the 27.2 Intel boot collection carries no VT-d driver.
//
// Design, chosen so a mistake here can cost a network port and never the boot:
//  - every device keeps untranslated DMA: its context entry is pass-through (ECAP.PT), exactly what it had with
//    translation off. Without PT support every device shares the translated domain below, whose low 512 GB is an
//    identity map, so physical addresses still work for every driver.
//  - network functions (PCI class 02) are moved into the translated domain when their nub is first published - before
//    any driver probes them - and get "iommu-parent" = this mapper. Their drivers' IODMACommands then map through
//    iovmMapMemory into a 1 GB window at IOVA 512 GB; anything they hand the chip by physical address (mbufs,
//    IOMbufMemoryCursor) still lands through the identity map.
//  - this is NOT the system mapper: IOMapper::gSystem stays empty, so no other DMA client changes behaviour.
#include <IOKit/IOService.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
extern "C" {
#include "acpi.h"
}

// IOTLB invalidation command, chosen by measurement on the running machine (autoTune at the first heartbeat):
// 0 global + drain (per CAP), 1 global, 2 domain + drain, 3 domain. Until then: mode 1 with a 10 ms bound.
static int gVtdMode = 1, gVtdBoundUs = 10000;

extern "C" ppnum_t upl_phys_page(upl_page_info_t *upl, int index);

#define VT_GCMD   0x18
#define VT_GSTS   0x1C
#define VT_RTADDR 0x20
#define VT_CCMD   0x28
#define VT_FSTS   0x34
#define GS_TE     (1u << 31)
#define GS_RTP    (1u << 30)
#define GS_WBF    (1u << 27)
#define GS_QIE    (1u << 26)
#define GS_IRE    (1u << 25)
#define GS_CFI    (1u << 23)
#define GS_PERSIST (GS_TE | GS_QIE | GS_IRE | GS_CFI)   // command bits that hold state; the others are one-shot

#define SL_R  1ULL
#define SL_W  2ULL
#define SL_PS (1ULL << 7)
#define SL_SNP (1ULL << 11)   // leaf: force snooped access (ECAP.SC). link up but vector
                              // 0x74 fired ~21k/s with no packet processed and no VT-d fault - the chip's No-Snoop status
                              // writes bypassed the CPU cache, so the driver kept reading a stale status block.

static const uint64_t kWinBase  = 512ULL << 30;        // IOVA 512 GB: PML4 slot 1, clear of the identity map below
static const uint64_t kWinPages = (1ULL << 30) >> 12;  // 1 GB window, 4 KB pages
static const uint16_t kDidXlate = 1, kDidPass = 2;
// The mapper's setup result, repeated in every heartbeat: the kernel message buffer (128 KB) wraps within ~20 s on
// a Mac Pro start, so a one-time line at setup is lost before any probe can save it.
static char gA2XVTdWhy[256] = "not attempted";
#define VTWHY(...) do { snprintf(gA2XVTdWhy, sizeof gA2XVTdWhy, __VA_ARGS__); IOLog("ACPI: VT-d mapper: %s\n", gA2XVTdWhy); } while (0)

struct VTUnit {
    IOMemoryMap      *map;
    volatile uint8_t *r;
    uint64_t          base, cap, ecap;
    uint32_t          iotlb;                           // IOTLB register offset (ECAP.IRO * 16 + 8)
};

class A2XVTdMapper : public IOMapper {
    OSDeclareDefaultStructors(A2XVTdMapper);
public:
    bool setupFromDMAR(const uint8_t *dmar, uint32_t len);
    bool translate(IOPCIDevice *dev);
    void report(void);

    virtual bool initHardware(IOService *provider) APPLE_KEXT_OVERRIDE { return true; }
    virtual uint64_t getPageSize(void) const APPLE_KEXT_OVERRIDE { return 4096; }
    virtual IOReturn iovmMapMemory(IOMemoryDescriptor *memory, uint64_t descriptorOffset, uint64_t length,
                                   uint32_t mapOptions, const IODMAMapSpecification *mapSpecification,
                                   IODMACommand *dmaCommand, const IODMAMapPageList *pageList,
                                   uint64_t *mapAddress, uint64_t *mapLength) APPLE_KEXT_OVERRIDE;
    virtual IOReturn iovmUnmapMemory(IOMemoryDescriptor *memory, IODMACommand *dmaCommand,
                                     uint64_t mapAddress, uint64_t mapLength) APPLE_KEXT_OVERRIDE;
    virtual IOReturn iovmInsert(uint32_t options, uint64_t mapAddress, uint64_t offset,
                                uint64_t physicalAddress, uint64_t length) APPLE_KEXT_OVERRIDE;
    virtual uint64_t mapToPhysicalAddress(uint64_t mappedAddress) APPLE_KEXT_OVERRIDE;

private:
    VTUnit    fUnit[8];
    uint32_t  fUnits;
    bool      fCoherent, fCaching, fPass;
    IOSimpleLock *fLock;
    OSArray  *fMem;                                    // keeps every table allocation alive
    uint64_t *fRoot, fRootPhys;
    uint64_t *fCtx[256], fCtxPhys[256];
    uint64_t  fPML4Phys;
    uint64_t *fPTE;                                    // window page-table entries, kWinPages contiguous
    uint64_t *fBitmap, fHint;
    uint32_t  fMaps, fUnmaps, fFails, fXlated, fMismatch;
    // invalidation health (heartbeat): count, timeouts, slowest wait, IOVA pages retired after a timeout
    uint32_t  fInvN, fInvTimeouts, fLeakPages;
    uint64_t  fInvMaxNs;
    uint64_t  fSnp;                                    // SL_SNP when every unit has snoop control, else 0

    void *alloc(uint32_t pages, uint64_t *phys);
    void  flush(const void *p, size_t n);
    bool  waitStatus(VTUnit &u, uint32_t bit, bool set);
    bool  command(VTUnit &u, uint32_t oneShot, uint32_t waitBit, bool waitSet, uint32_t persist);
    bool  invalidateAll(bool contexts);
public:
    void  selfTest(uint32_t n, char *out, size_t outsz);
    void  autoTune(void);
    bool  fTuned;
private:
    bool  inval64(VTUnit &u, uint32_t off, uint64_t v);
    int64_t allocIOVA(uint64_t pages, uint64_t alignPages);
    void  freeIOVA(uint64_t first, uint64_t pages);
};

OSDefineMetaClassAndStructors(A2XVTdMapper, IOMapper);

void *A2XVTdMapper::alloc(uint32_t pages, uint64_t *phys)
{
    IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, (vm_size_t)pages * 4096, 0x000000FFFFFFF000ULL);
    if (!b) return NULL;
    void *v = b->getBytesNoCopy();
    *phys = b->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
    if (!v || !*phys) { b->release(); return NULL; }
    bzero(v, (size_t)pages * 4096);
    fMem->setObject(b); b->release();
    return v;
}

void A2XVTdMapper::flush(const void *p, size_t n)
{
    if (!fCoherent) {      // ECAP.C = 0: the remapper reads tables from memory, not the CPU cache
        for (uintptr_t a = (uintptr_t)p & ~63UL; a < (uintptr_t)p + n; a += 64) __builtin_ia32_clflush((const void *)a);
    }
    __builtin_ia32_mfence();
}

bool A2XVTdMapper::waitStatus(VTUnit &u, uint32_t bit, bool set)
{
    for (uint32_t t = 0; t < 100000; t++) {            // 1 s bound: a remapper that never answers is reported, not waited on
        bool now = (*(volatile uint32_t *)(u.r + VT_GSTS) & bit) != 0;
        if (now == set) return true;
        IODelay(10);
    }
    return false;
}

bool A2XVTdMapper::command(VTUnit &u, uint32_t oneShot, uint32_t waitBit, bool waitSet, uint32_t persist)
{
    *(volatile uint32_t *)(u.r + VT_GCMD) = persist | oneShot;
    return waitStatus(u, waitBit, waitSet);
}

// Bounded by wall time (mach_absolute_time), not by a count of IODelay calls: a poll count says nothing about how
// long interrupts stay off. The old 100000 x IODelay(1) bound measured ~127 ms per timed-out invalidation.
bool A2XVTdMapper::inval64(VTUnit &u, uint32_t off, uint64_t v)
{
    volatile uint64_t *reg = (volatile uint64_t *)(u.r + off);
    uint64_t t0 = mach_absolute_time(), lim = 0, el = 0;
    nanoseconds_to_absolutetime((uint64_t)(gVtdBoundUs > 0 ? gVtdBoundUs : 10000) * 1000, &lim);
    // a command still in flight from an earlier timeout must finish before the register is written again
    for (uint32_t t = 0; t < 4000000 && (*reg & (1ULL << 63)); t++) {
        if ((t & 63) == 63 && mach_absolute_time() - t0 > lim) break;
        __builtin_ia32_pause();
    }
    *reg = v;
    bool done = false;
    for (uint32_t t = 0; t < 4000000; t++) {
        if (!(*reg & (1ULL << 63))) { done = true; break; }
        if ((t & 63) == 63 && mach_absolute_time() - t0 > lim) break;
        __builtin_ia32_pause();
    }
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &el);
    fInvN++;
    if (el > fInvMaxNs) fInvMaxNs = el;
    if (!done) fInvTimeouts++;
    return done;
}

void A2XVTdMapper::selfTest(uint32_t n, char *out, size_t outsz)
{
    uint32_t ok = 0, to0 = fInvTimeouts;
    uint64_t mx0 = fInvMaxNs, sum = 0, mn = ~0ULL, mx = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t t0 = mach_absolute_time(), el = 0;
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
        fInvMaxNs = 0;
        bool r = invalidateAll(false);
        IOSimpleLockUnlockEnableInterrupt(fLock, is);
        absolutetime_to_nanoseconds(mach_absolute_time() - t0, &el);
        if (r) ok++;
        sum += el; if (el < mn) mn = el; if (el > mx) mx = el;
    }
    if (mx0 > fInvMaxNs) fInvMaxNs = mx0;
    snprintf(out, outsz, "mode %d bound %d us: n=%u ok=%u timeouts=%u min=%llu avg=%llu max=%llu us", gVtdMode, gVtdBoundUs,
             n, ok, fInvTimeouts - to0, mn / 1000, (sum / (n ? n : 1)) / 1000, mx / 1000);
    IOLog("ACPI: vtd test %s\n", out);
}

// Once, from the heartbeat (thread context, ~30 s after start): time 50 invalidations in each mode, keep the fastest
// mode with no timeout. Each test holds the lock with interrupts off for at most one bound (10 ms) per invalidation.
void A2XVTdMapper::autoTune(void)
{
    if (fTuned) return;
    fTuned = true;
    char line[160];
    int best = -1; uint64_t bestAvg = ~0ULL;
    for (int m = 0; m < 4; m++) {
        gVtdMode = m;
        uint32_t to0 = fInvTimeouts;
        uint64_t t0 = mach_absolute_time(), el = 0;
        selfTest(50, line, sizeof line);
        absolutetime_to_nanoseconds(mach_absolute_time() - t0, &el);
        if (fInvTimeouts == to0 && el / 50 < bestAvg) { best = m; bestAvg = el / 50; }
    }
    gVtdMode = best >= 0 ? best : 1;
    IOLog("ACPI: vtd autotune: mode %d chosen (%s)\n", gVtdMode, best >= 0 ? "no timeouts" : "every mode timed out, kept 1");
}

bool A2XVTdMapper::invalidateAll(bool contexts)
{
    bool ok = true;
    for (uint32_t i = 0; i < fUnits; i++) {
        VTUnit &u = fUnit[i];
        uint32_t persist = *(volatile uint32_t *)(u.r + VT_GSTS) & GS_PERSIST;
        if (u.cap & (1ULL << 4)) command(u, GS_WBF, GS_WBF, false, persist);                 // CAP.RWBF
        if (contexts && !inval64(u, VT_CCMD, (1ULL << 63) | (1ULL << 61))) {                  // global context cache
            ok = false;
            if (fInvTimeouts < 16) IOLog("ACPI: VT-d unit 0x%llx: context-cache invalidation timed out\n", u.base);
        }
        // global IOTLB; read/write draining only where CAP.DRD (55) / CAP.DWD (54) say the unit has it (as Linux does)
        int mode = gVtdMode;
        uint64_t cmd = (1ULL << 63) | ((mode & 2) ? ((2ULL << 60) | ((uint64_t)kDidXlate << 32)) : (1ULL << 60));
        if (!(mode & 1) && (u.cap & (1ULL << 55))) cmd |= 1ULL << 49;
        if (!(mode & 1) && (u.cap & (1ULL << 54))) cmd |= 1ULL << 48;
        if (!inval64(u, u.iotlb, cmd)) {
            ok = false;
            if (fInvTimeouts < 16) IOLog("ACPI: VT-d unit 0x%llx: IOTLB invalidation timed out (cmd 0x%llx)\n", u.base, cmd);
        }
    }
    return ok;
}

bool A2XVTdMapper::setupFromDMAR(const uint8_t *b, uint32_t len)
{
    fLock = IOSimpleLockAlloc();
    fMem = OSArray::withCapacity(8);
    if (!fLock || !fMem) { VTWHY("no lock / array"); return false; }

    // 1. units: map every DRHD's registers and check it can take our tables
    for (uint32_t off = 48; off + 4 <= len;) {
        uint16_t type = (uint16_t)(b[off] | (b[off + 1] << 8)), slen = (uint16_t)(b[off + 2] | (b[off + 3] << 8));
        if (slen < 4 || off + slen > len) break;
        if (type == 0 && slen >= 16 && fUnits < 8) {
            VTUnit &u = fUnit[fUnits];
            u.base = 0;
            for (int i = 7; i >= 0; i--) u.base = (u.base << 8) | b[off + 8 + i];
            IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(u.base, 0x1000, kIODirectionInOut);
            u.map = md ? md->map(kIOMapInhibitCache) : NULL;
            if (md) md->release();
            if (!u.map) { VTWHY("unit 0x%llx registers not mappable", u.base); return false; }
            u.r = (volatile uint8_t *)u.map->getVirtualAddress();
            u.cap = *(volatile uint64_t *)(u.r + 0x08);
            u.ecap = *(volatile uint64_t *)(u.r + 0x10);
            u.iotlb = (uint32_t)(((u.ecap >> 8) & 0x3FF) * 16 + 8);
            uint32_t sts = *(volatile uint32_t *)(u.r + VT_GSTS);
            IOLog("ACPI: VT-d unit 0x%llx: cap 0x%llx ecap 0x%llx status 0x%08x\n", u.base, u.cap, u.ecap, sts);
            if (!((u.cap >> 8) & 4)) { VTWHY("unit 0x%llx has no 4-level tables (cap 0x%llx)", u.base, u.cap); return false; }
            // The Mac Pro firmware hands over with interrupt remapping on (status 0xc3000000); every
            // macOS 27 start so far has run with it off (status 0x41000000 after translation-off) and the kernel programs
            // compatibility-format interrupts, so it is switched off below instead of refusing the mapper.
            fUnits++;
        }
        off += slen;
    }
    if (!fUnits) { VTWHY("DMAR lists no remapping unit"); return false; }
    fCoherent = true; fPass = true; fCaching = false;
    bool gb = true, mb2 = true;
    for (uint32_t i = 0; i < fUnits; i++) {
        fCoherent &= (fUnit[i].ecap & 1) != 0;
        fPass     &= (fUnit[i].ecap & (1ULL << 6)) != 0;
        fCaching  |= (fUnit[i].cap & (1ULL << 7)) != 0;
        gb  &= (fUnit[i].cap & (1ULL << 35)) != 0;
        mb2 &= (fUnit[i].cap & (1ULL << 34)) != 0;
    }
    fSnp = SL_SNP;
    for (uint32_t i = 0; i < fUnits; i++) if (!(fUnit[i].ecap & (1ULL << 7))) fSnp = 0;   // bit 11 is reserved without SC
    if (!gb && !mb2) { VTWHY("no 2 MB / 1 GB pages (cap 0x%llx)", fUnit[0].cap); return false; }

    // 2. tables: root, 256 context tables, the translated domain (identity low 512 GB + the 1 GB window)
    uint64_t miscPhys, ctxPhys, ptPhys, idPhys = 0;
    uint64_t *misc = (uint64_t *)alloc(5, &miscPhys);           // root, PML4, PDPT low, PDPT window, PD window
    uint64_t *ctx  = (uint64_t *)alloc(256, &ctxPhys);
    fPTE           = (uint64_t *)alloc((uint32_t)(kWinPages / 512), &ptPhys);
    uint64_t *idpd = gb ? NULL : (uint64_t *)alloc(512, &idPhys);
    fBitmap = (uint64_t *)IOMallocZero(kWinPages / 8);
    if (!misc || !ctx || !fPTE || (!gb && !idpd) || !fBitmap) { VTWHY("table memory: misc %p ctx %p pt %p idpd %p bitmap %p", misc, ctx, fPTE, idpd, fBitmap); return false; }
    fRoot = misc; fRootPhys = miscPhys;
    uint64_t *pml4 = misc + 512, *pdptLo = misc + 1024, *pdptWin = misc + 1536, *pdWin = misc + 2048;
    fPML4Phys = miscPhys + 4096;
    pml4[0] = (miscPhys + 2 * 4096) | SL_R | SL_W;
    pml4[1] = (miscPhys + 3 * 4096) | SL_R | SL_W;
    for (uint64_t i = 0; i < 512; i++) {
        if (gb) pdptLo[i] = (i << 30) | SL_PS | SL_R | SL_W | fSnp;
        else {
            pdptLo[i] = (idPhys + i * 4096) | SL_R | SL_W;
            for (uint64_t j = 0; j < 512; j++) idpd[i * 512 + j] = (i << 30) | (j << 21) | SL_PS | SL_R | SL_W | fSnp;
        }
    }
    pdptWin[0] = (miscPhys + 4 * 4096) | SL_R | SL_W;
    for (uint64_t i = 0; i < kWinPages / 512; i++) pdWin[i] = (ptPhys + i * 4096) | SL_R | SL_W;

    // every function: pass-through when the hardware has it, else the identity domain
    for (uint32_t bus = 0; bus < 256; bus++) {
        fCtx[bus] = ctx + bus * 512; fCtxPhys[bus] = ctxPhys + bus * 4096;
        for (uint32_t df = 0; df < 256; df++) {
            uint64_t *e = fCtx[bus] + df * 2;
            e[1] = 2ULL | ((uint64_t)(fPass ? kDidPass : kDidXlate) << 8);           // AW 2 = 48-bit, DID
            e[0] = fPass ? ((2ULL << 2) | 1) : (fPML4Phys | 1);                       // TT 10 pass-through, or TT 00
        }
        fRoot[bus * 2] = fCtxPhys[bus] | 1;
    }
    flush(misc, 5 * 4096); flush(ctx, 256 * 4096); flush(fPTE, kWinPages * 8);
    if (idpd) flush(idpd, 512 * 4096);

    // 3. hardware: translation off, queued invalidation off, new root, invalidate, translation on
    for (uint32_t i = 0; i < fUnits; i++) {
        VTUnit &u = fUnit[i];
        uint32_t persist = *(volatile uint32_t *)(u.r + VT_GSTS) & GS_PERSIST;
        if ((persist & GS_TE) && !command(u, 0, GS_TE, false, persist & ~GS_TE)) { VTWHY("unit 0x%llx translation will not turn off (0x%08x)", u.base, *(volatile uint32_t *)(u.r + VT_GSTS)); return false; }
        persist &= ~GS_TE;
        if ((persist & GS_IRE) && !command(u, 0, GS_IRE, false, persist & ~GS_IRE)) { VTWHY("unit 0x%llx interrupt remapping will not turn off (0x%08x)", u.base, *(volatile uint32_t *)(u.r + VT_GSTS)); return false; }
        persist &= ~GS_IRE;
        if ((persist & GS_QIE) && !command(u, 0, GS_QIE, false, persist & ~GS_QIE)) { VTWHY("unit 0x%llx QI will not turn off (0x%08x)", u.base, *(volatile uint32_t *)(u.r + VT_GSTS)); return false; }
        persist &= ~GS_QIE;
        *(volatile uint64_t *)(u.r + VT_RTADDR) = fRootPhys;                         // RTT 0: legacy root table
        // RTPS is already set from the firmware's table, so a set bit alone does not prove the new pointer was latched:
        // give the unit time to clear and re-set it, then check the pointer reads back.
        *(volatile uint32_t *)(u.r + VT_GCMD) = persist | GS_RTP;
        IODelay(1000);
        if (*(volatile uint64_t *)(u.r + VT_RTADDR) != fRootPhys) { VTWHY("unit 0x%llx root pointer reads back 0x%llx", u.base, *(volatile uint64_t *)(u.r + VT_RTADDR)); return false; }
        if (!waitStatus(u, GS_RTP, true)) { VTWHY("unit 0x%llx root table not taken (0x%08x)", u.base, *(volatile uint32_t *)(u.r + VT_GSTS)); return false; }
    }
    invalidateAll(true);
    for (uint32_t i = 0; i < fUnits; i++) {
        VTUnit &u = fUnit[i];
        uint32_t persist = *(volatile uint32_t *)(u.r + VT_GSTS) & GS_PERSIST;
        if (!command(u, 0, GS_TE, true, persist | GS_TE)) { VTWHY("unit 0x%llx translation will not turn on (0x%08x)", u.base, *(volatile uint32_t *)(u.r + VT_GSTS)); return false; }
        IOLog("ACPI: VT-d unit 0x%llx: translation on, status 0x%08x\n", u.base, *(volatile uint32_t *)(u.r + VT_GSTS));
    }
    VTWHY("ready: %u unit(s) cap 0x%llx ecap 0x%llx, devices %s, %s identity pages, window 0x%llx + 1 GB, %scoherent%s, snoop %s",
          fUnits, fUnit[0].cap, fUnit[0].ecap, fPass ? "pass-through" : "identity-mapped", gb ? "1 GB" : "2 MB", kWinBase,
          fCoherent ? "" : "not ", fCaching ? ", caching mode" : "", fSnp ? "forced" : "not available");
    return true;
}

bool A2XVTdMapper::translate(IOPCIDevice *dev)
{
    uint32_t bus = dev->getBusNumber(), df = (dev->getDeviceNumber() << 3) | dev->getFunctionNumber();
    if (bus > 255 || df > 255) return false;
    uint64_t *e = fCtx[bus] + df * 2;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
    if (fPass) {                       // present pass-through -> not present -> translated; the device has not started
        e[0] = 0; flush(e, 16);
        invalidateAll(true);
        e[1] = 2ULL | ((uint64_t)kDidXlate << 8);
        flush(e, 16);
        e[0] = fPML4Phys | 1;          // TT 00: second-level translation for untranslated requests
        flush(e, 16);
        invalidateAll(true);
    }
    fXlated++;
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    dev->setProperty("iommu-parent", this);
    return true;
}

int64_t A2XVTdMapper::allocIOVA(uint64_t pages, uint64_t align)
{
    if (!pages || pages > kWinPages) return -1;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t start = pass ? 0 : fHint;
        start = (start + align - 1) / align * align;
        while (start + pages <= kWinPages) {
            uint64_t i = 0;
            for (; i < pages; i++) {
                uint64_t p = start + i;
                if (fBitmap[p >> 6] & (1ULL << (p & 63))) break;
            }
            if (i == pages) {
                for (i = 0; i < pages; i++) { uint64_t p = start + i; fBitmap[p >> 6] |= 1ULL << (p & 63); }
                fHint = start + pages;
                return (int64_t)start;
            }
            start = (start + i + 1 + align - 1) / align * align;
        }
    }
    return -1;
}

void A2XVTdMapper::freeIOVA(uint64_t first, uint64_t pages)
{
    for (uint64_t i = 0; i < pages && first + i < kWinPages; i++) {
        uint64_t p = first + i;
        fBitmap[p >> 6] &= ~(1ULL << (p & 63));
    }
}

IOReturn A2XVTdMapper::iovmMapMemory(IOMemoryDescriptor *md, uint64_t off, uint64_t len, uint32_t opts,
                                     const IODMAMapSpecification *spec, IODMACommand *, const IODMAMapPageList *pl,
                                     uint64_t *mapAddress, uint64_t *mapLength)
{
    if (!len || !mapAddress) return kIOReturnBadArgument;
    if (spec && spec->numAddressBits && spec->numAddressBits < 64
        && ((kWinBase + (kWinPages << 12) - 1) >> spec->numAddressBits)) {
        if (OSIncrementAtomic(&fFails) < 8) IOLog("ACPI: VT-d map refused: device takes %u address bits, window is 40\n", spec->numAddressBits);
        return kIOReturnNotPermitted;
    }
    uint64_t pgoff = 0;
    if (pl) pgoff = (pl->pageOffset + off) & 0xFFF;
    else if (md) {
        IOByteCount sl = 0;
        addr64_t p = md->getPhysicalSegment(off, &sl, kIOMemoryMapperNone);
        if (!p) { if (OSIncrementAtomic(&fFails) < 8) IOLog("ACPI: VT-d map: no physical page at offset 0x%llx\n", off); return kIOReturnVMError; }
        pgoff = p & 0xFFF;
    }
    uint64_t pages = (pgoff + len + 0xFFF) >> 12;
    uint64_t align = (spec && spec->alignment > 4096) ? (spec->alignment >> 12) : 1;
    uint64_t perm = (opts & (kIODMAMapReadAccess | kIODMAMapWriteAccess)) ? 0 : (SL_R | SL_W);
    if (opts & kIODMAMapReadAccess)  perm |= SL_R;     // device reads memory
    if (opts & kIODMAMapWriteAccess) perm |= SL_W;     // device writes memory

    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
    int64_t first = allocIOVA(pages, align);
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    if (first < 0) { if (OSIncrementAtomic(&fFails) < 8) IOLog("ACPI: VT-d map: window full (%llu pages wanted)\n", pages); return kIOReturnNoResources; }

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = 0;
        if (pl) {
            if (pl->pageListCount && i >= pl->pageListCount) break;
            phys = ptoa_64(upl_phys_page((upl_page_info_t *)pl->pageList, (int)i));
        } else if (md) {
            IOByteCount sl = 0;
            uint64_t o = (i == 0) ? off : off - pgoff + (i << 12);
            phys = md->getPhysicalSegment(o, &sl, kIOMemoryMapperNone) & ~0xFFFULL;
        }
        if (phys && pl && md) {                       // self-check: the page list must agree with the descriptor
            IOByteCount sl = 0;
            uint64_t o = (i == 0) ? off : off - pgoff + (i << 12);
            uint64_t want = md->getPhysicalSegment(o, &sl, kIOMemoryMapperNone) & ~0xFFFULL;
            if (want != phys && OSIncrementAtomic(&fMismatch) < 4)
                IOLog("ACPI: VT-d map: page %llu list 0x%llx descriptor 0x%llx\n", i, phys, want);
        }
        fPTE[first + i] = phys ? (phys | perm | fSnp) : 0;   // no memory (IOMapperIOVMAlloc): filled later by iovmInsert
    }
    flush(&fPTE[first], pages * 8);
    if (fCaching) { is = IOSimpleLockLockDisableInterrupt(fLock); invalidateAll(false); IOSimpleLockUnlockEnableInterrupt(fLock, is); }
    OSIncrementAtomic(&fMaps);
    *mapAddress = kWinBase + ((uint64_t)first << 12) + pgoff;
    if (mapLength) *mapLength = len;                   // == the length asked: IODMACommand's single-run case
    return kIOReturnSuccess;
}

IOReturn A2XVTdMapper::iovmUnmapMemory(IOMemoryDescriptor *, IODMACommand *, uint64_t addr, uint64_t len)
{
    if (addr < kWinBase || addr >= kWinBase + (kWinPages << 12) || !len) return kIOReturnSuccess;
    uint64_t first = (addr - kWinBase) >> 12, pages = ((addr & 0xFFF) + len + 0xFFF) >> 12;
    if (first + pages > kWinPages) pages = kWinPages - first;
    for (uint64_t i = 0; i < pages; i++) fPTE[first + i] = 0;
    flush(&fPTE[first], pages * 8);
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
    // the IOVA may be handed out again only once no stale translation can survive; after a timed-out invalidation
    // the range is retired for good (counted) instead of being reused under a translation the unit may still hold
    if (invalidateAll(false)) freeIOVA(first, pages);
    else fLeakPages += (uint32_t)pages;
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    OSIncrementAtomic(&fUnmaps);
    return kIOReturnSuccess;
}

IOReturn A2XVTdMapper::iovmInsert(uint32_t options, uint64_t addr, uint64_t offset, uint64_t phys, uint64_t len)
{
    uint64_t a = addr + offset;
    if (a < kWinBase || a >= kWinBase + (kWinPages << 12)) return kIOReturnBadArgument;
    uint64_t first = (a - kWinBase) >> 12, pages = (len + 0xFFF) >> 12;
    uint64_t perm = (options & (kIODMAMapReadAccess | kIODMAMapWriteAccess)) ? 0 : (SL_R | SL_W);
    if (options & kIODMAMapReadAccess)  perm |= SL_R;
    if (options & kIODMAMapWriteAccess) perm |= SL_W;
    for (uint64_t i = 0; i < pages && first + i < kWinPages; i++) fPTE[first + i] = ((phys & ~0xFFFULL) + (i << 12)) | perm | fSnp;
    flush(&fPTE[first], pages * 8);
    return kIOReturnSuccess;
}

uint64_t A2XVTdMapper::mapToPhysicalAddress(uint64_t a)
{
    if (a < kWinBase || a >= kWinBase + (kWinPages << 12)) return a;   // identity below the window
    uint64_t e = fPTE[(a - kWinBase) >> 12];
    return e ? ((e & 0x000FFFFFFFFFF000ULL) | (a & 0xFFF)) : 0;
}

void A2XVTdMapper::report(void)
{
    for (uint32_t i = 0; i < fUnits; i++) {
        VTUnit &u = fUnit[i];
        uint32_t fsts = *(volatile uint32_t *)(u.r + VT_FSTS);
        IOLog("ACPI:   vtd 0x%llx gsts 0x%08x fsts 0x%08x\n", u.base, *(volatile uint32_t *)(u.r + VT_GSTS), fsts);
        uint32_t fro = (uint32_t)((u.cap >> 24) & 0x3FF) * 16, nfr = (uint32_t)((u.cap >> 40) & 0xFF) + 1;
        for (uint32_t f = 0; f < nfr && f < 8; f++) {
            uint64_t lo = *(volatile uint64_t *)(u.r + fro + f * 16), hi = *(volatile uint64_t *)(u.r + fro + f * 16 + 8);
            if (!(hi >> 63)) continue;
            IOLog("ACPI:   vtd fault: source %x:%x.%x reason 0x%02x %s address 0x%llx\n",
                  (unsigned)((hi >> 8) & 0xFF), (unsigned)((hi >> 3) & 0x1F), (unsigned)(hi & 7),
                  (unsigned)((hi >> 32) & 0xFF), ((hi >> 62) & 1) ? "read" : "write", lo & ~0xFFFULL);
        }
    }
    IOLog("ACPI:   vtd mapper: %u device(s) translated, %u maps, %u unmaps, %u failures, %u page mismatches, snoop %s\n",
          fXlated, fMaps, fUnmaps, fFails, fMismatch, fSnp ? "forced" : "not available");
    {
        uint64_t mx = fInvMaxNs;
        IOLog("ACPI:   vtd invalidations %u, timeouts %u, slowest %llu us, retired pages %u\n", fInvN, fInvTimeouts,
              mx / 1000, fLeakPages);
        for (uint32_t i = 0; i < fUnits; i++)
            IOLog("ACPI:   vtd unit 0x%llx cap 0x%llx ecap 0x%llx iotlb +0x%x\n", fUnit[i].base, fUnit[i].cap, fUnit[i].ecap, fUnit[i].iotlb);
    }
}

// ---- glue for the platform expert -------------------------------------------------------------------------------
static A2XVTdMapper *gA2XVTdMapper;


// USB host controllers are moved into the translated window only on Apple hardware, where it was proven (the Mac Pro
// 6,1 PCH EHCI and the Fresco xHCI fault on DMA above 4 GB without it). SMBIOS cannot tell a real Mac apart (boot
// loaders present a Mac model on PCs), so the firmware's own ACPI tables decide: Apple firmware writes OEM ID "APPLE "
// into the DSDT and FADT. Boot-arg a2x_vtdusb=1 forces it on, a2x_vtdusb=0 forces it off.
static int A2XVTdUSBAllowed(void)
{
    static int allowed = -1;
    if (allowed >= 0) return allowed;
    UInt32 v = 0;
    if (PE_parse_boot_argn("a2x_vtdusb", &v, sizeof v)) {
        allowed = v ? 1 : 0;
        IOLog("ACPI: VT-d: USB host mapping %s by a2x_vtdusb\n", allowed ? "on" : "off");
        return allowed;
    }
    ACPI_TABLE_HEADER *h = NULL;
    char oem[7] = "?";
    allowed = 0;
    if (ACPI_SUCCESS(AcpiGetTable((char *)ACPI_SIG_DSDT, 1, &h)) && h) {
        memcpy(oem, h->OemId, 6); oem[6] = 0;
        allowed = !memcmp(h->OemId, "APPLE", 5);
    }
    IOLog("ACPI: VT-d: firmware OEM \"%s\": USB host mapping %s\n", oem, allowed ? "on (Apple firmware)" : "off");
    return allowed;
}

static bool A2XVTdPublished(void *, void *, IOService *svc, IONotifier *)
{
    IOPCIDevice *d = OSDynamicCast(IOPCIDevice, svc);
    if (!d || !gA2XVTdMapper) return true;
    uint32_t cls = d->configRead32(kIOPCIConfigRevisionID) >> 8;
    bool usb = (cls >> 8) == 0x0c03 && A2XVTdUSBAllowed();
    if ((cls >> 16) != 0x02 && !usb) return true;      // network (0200, 0280) and USB hosts (0c03xx)
    uint32_t id = d->configRead32(kIOPCIConfigVendorID);
    bool ok = gA2XVTdMapper->translate(d);
    if (ok && usb) {   // EHCI 64-bit structures: CTRLDSSEGMENT = the window's high dword (see vtdehci_patch.py)
        UInt32 seg = (UInt32)(kWinBase >> 32);
        OSData *sd = OSData::withBytes(&seg, sizeof seg);
        if (sd) { d->setProperty("ctrldsseg", sd); sd->release(); }
    }
    // No config-space WRITES here: this runs inside the nub's first publish, and a Device Control write at that point
    // hung ETH0's publish and every PCI publish behind it (observed: 'waiting for root device', ETH0 busy,
    // NVMe never ready). SL_SNP in the page entries forces snooped access whatever the chip's No-Snoop setting.
    IOLog("ACPI: VT-d: %x:%x.%x %04x:%04x class %06x %s\n", d->getBusNumber(), d->getDeviceNumber(),
          d->getFunctionNumber(), id & 0xFFFF, id >> 16, cls, ok ? "maps through the VT-d window" : "NOT translated");
    return true;
}

// Called once from A2XACPIPlatformExpert::start after IOPlatformExpert::start and before any PCI nub exists.
// false = nothing was changed beyond what translation-off already does; the caller turns translation off.
bool A2XVTdStartMapper(const void *dmar, uint32_t len, IOService *platform)
{
    A2XVTdMapper *m = OSTypeAlloc(A2XVTdMapper);
    if (!m || !m->init()) { VTWHY("object init failed"); OSSafeReleaseNULL(m); return false; }
    if (!m->setupFromDMAR((const uint8_t *)dmar, len)) { m->release(); return false; }   // tables leak: a few MB, once
    if (!m->attach(platform) || !m->start(platform)) { VTWHY("IOMapper start failed after setup"); m->release(); return false; }
    m->registerService();
    gA2XVTdMapper = m;
    IONotifier *n = IOService::addMatchingNotification(gIOFirstPublishNotification, IOService::serviceMatching("IOPCIDevice"),
                                                       A2XVTdPublished, NULL, NULL, 1000);
    if (!n) IOLog("ACPI: VT-d: no PCI publish notification; network devices keep physical DMA\n");
    return true;
}

void A2XVTdReport(void)
{
    IOLog("ACPI:   vtd setup: %s\n", gA2XVTdWhy);
    if (gA2XVTdMapper) { gA2XVTdMapper->autoTune(); gA2XVTdMapper->report(); }
}

void A2XVTdNote(const char *why) { VTWHY("%s", why); }
