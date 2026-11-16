// pcibridge.cpp - PCI root bridge for the ACPI platform (macOS 27, x86_64).
//
// Matches the PNP0A03 / PNP0A08 nub the platform expert publishes for each host bridge and gives IOPCIFamily
// configuration-space access to the buses below it. Everything else (enumeration, BAR assignment from the
// _CRS windows, MSI, INTx routing) is IOPCIFamily's and the platform expert's.
//
// Config access: PCIe ECAM through the MCFG allocation that covers this bridge's segment and buses (all 4 KB of
// each function), else the legacy 0xCF8/0xCFC mechanism (first 256 bytes only, segment 0).

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pci/IOPCIBridge.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <architecture/i386/pio.h>
extern "C" boolean_t PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg);
extern "C" {
#include "acpi.h"
#include "crumb.h"
}
#include "confollow.h"

// IOPCIFamily 27 keeps the shared host-bridge data in IOPCIHostBridge (probe sets it, configure
// publishes it to reserved->hostBridgeData) and IOPCIBridge::configOp dereferences it.
// The class is exported but not in the SDK header; it adds no virtuals and is 0xb8 bytes.
class IOPCIHostBridge : public IOPCIBridge
{
    OSDeclareDefaultStructors(IOPCIHostBridge)
protected:
    OSObject   *fA2XBridgeData;
    UInt64      fA2XPad[2];
public:
    virtual IOService *probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
    virtual void free(void) override;
    virtual bool configure(IOService *provider) override;
};
static_assert(sizeof(IOPCIHostBridge) == 0xb8, "IOPCIHostBridge size in IOPCIFamily 27");

class A2XACPIPCI : public IOPCIHostBridge
{
    OSDeclareDefaultStructors(A2XACPIPCI)

    IOACPIPlatformDevice *fACPI;
    IOMemoryMap          *fECAMMap;
    volatile UInt8       *fECAM;       // config space of bus fBusMin, function 0, register 0
    IODeviceMemory       *fIOMem;      // x86 I/O port space, 64 KB
    IOSimpleLock         *fCF8Lock;    // 0xCF8 address/data pair is one shared register window
    IOSimpleLock         *fCapLock;    // guards gCapMap
    UInt16                fSegment;
    UInt64                fHideLo[3], fHideHi[3];   // hidden bridge windows: 0 mem, 1 pf, 2 io (hi < lo = none)
    UInt32                fMask[4];                 // masked functions: 0x80000000 | bus<<16 | dev<<8 | fn
    UInt32                fHold[4];                 // held functions (same encoding): configured, not matched
    UInt32 heldFix(IOPCIAddressSpace space, UInt8 dword, UInt32 v) const;
    IOReturn releaseHold(UInt32 key);
    void  hideBridgeFromConfigurator(void);
    bool  addRangeExcluding(unsigned kind, UInt64 min, UInt64 len, bool pf);
    static UInt32 cfRead(void *ctx, UInt8 bus, UInt8 dev, UInt8 fn, UInt8 reg);   // confollow.h reader
    void  trackConsole(void);
    friend int ::a2x_console_where(void *, uint64_t *);
    UInt8                 fBusMin;
    UInt8                 fBusMax;

    inline volatile UInt8 *ecam(IOPCIAddressSpace space, UInt8 offset) const
    {
        UInt32 reg = ((UInt32)space.es.registerNumExtended << 8) | offset;
        UInt32 bus = space.es.busNum;
        if (bus < fBusMin || bus > fBusMax) return NULL;
        UInt32 key = 0x80000000u | (bus << 16) | ((UInt32)space.es.deviceNum << 8) | space.es.functionNum;
        if (key == fMask[0] || key == fMask[1] || key == fMask[2] || key == fMask[3]) return NULL;   // masked: reads all ones, writes dropped
        return fECAM + ((UInt64)(bus - fBusMin) << 20) + ((UInt32)space.es.deviceNum << 15)
                     + ((UInt32)space.es.functionNum << 12) + reg;
    }
    inline UInt32 cf8(IOPCIAddressSpace space, UInt8 offset) const
    {
        return 0x80000000u | ((UInt32)space.s.busNum << 16) | ((UInt32)space.s.deviceNum << 11)
                           | ((UInt32)space.s.functionNum << 8) | (offset & 0xfc);
    }
    bool findECAM(void);
    bool crsBusRange(void);
    unsigned addCRSRanges(void);
    UInt32 rawRead32(IOPCIAddressSpace space, UInt8 offset);
    UInt16 rawRead16(IOPCIAddressSpace space, UInt8 offset);
    UInt8  rawRead8(IOPCIAddressSpace space, UInt8 offset);
    UInt32 capDword(IOPCIAddressSpace space, UInt8 offset);

public:
    virtual bool start(IOService *provider) override;
    virtual void free(void) override;

    virtual UInt8 firstBusNum(void) override { return fBusMin; }
    virtual UInt8 lastBusNum(void) override { return fBusMax; }
    virtual IOPCIAddressSpace getBridgeSpace(void) override
    {
        IOPCIAddressSpace s; s.bits = 0; s.s.busNum = fBusMin; return s;
    }
    virtual IODeviceMemory *ioDeviceMemory(void) override { return fIOMem; }

    virtual UInt32 configRead32(IOPCIAddressSpace space, UInt8 offset) override;
    virtual void   configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data) override;
    virtual UInt16 configRead16(IOPCIAddressSpace space, UInt8 offset) override;
    virtual void   configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data) override;
    virtual UInt8  configRead8(IOPCIAddressSpace space, UInt8 offset) override;
    virtual IOReturn setProperties(OSObject *properties) override;
    virtual void   configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data) override;

    virtual IOReturn setLinkSpeed(tIOPCILinkSpeed linkSpeed, bool retrain) override { return kIOReturnUnsupported; }
    virtual IOReturn getLinkSpeed(tIOPCILinkSpeed *linkSpeed) override { return kIOReturnUnsupported; }
};

OSDefineMetaClassAndStructors(A2XACPIPCI, IOPCIHostBridge)

// The MCFG allocation for this bridge's segment that contains its first bus. A root bridge with no _BBN starts
// at bus 0; with no _SEG it is segment 0 (ACPI 6.5 6.5.5, 6.5.6).
bool A2XACPIPCI::findECAM(void)
{
    ACPI_TABLE_HEADER *hdr = NULL;
    if (ACPI_FAILURE(AcpiGetTable((char *)ACPI_SIG_MCFG, 1, &hdr)) || !hdr) return false;
    UInt8 *p = (UInt8 *)hdr + sizeof(ACPI_TABLE_MCFG);
    UInt8 *end = (UInt8 *)hdr + hdr->Length;
    bool found = false;
    for (; p + sizeof(ACPI_MCFG_ALLOCATION) <= end; p += sizeof(ACPI_MCFG_ALLOCATION)) {
        ACPI_MCFG_ALLOCATION *a = (ACPI_MCFG_ALLOCATION *)p;
        if (a->PciSegment != fSegment || fBusMin < a->StartBusNumber || fBusMin > a->EndBusNumber) continue;
        if (fBusMax > a->EndBusNumber) fBusMax = a->EndBusNumber;
        UInt64 base = a->Address + ((UInt64)(fBusMin - a->StartBusNumber) << 20);
        UInt64 len  = (UInt64)(fBusMax - fBusMin + 1) << 20;
        IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)base, (IOByteCount)len, kIODirectionInOut);
        if (md) {
            fECAMMap = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache);
            md->release();
        }
        if (fECAMMap) {
            fECAM = (volatile UInt8 *)fECAMMap->getVirtualAddress();
            IOLog("ACPI: PCI segment %u buses %u..%u ECAM 0x%llx (%llu MB)\n", fSegment, fBusMin, fBusMax,
                  base, len >> 20);
            found = true;
        } else {
            IOLog("ACPI: PCI ECAM 0x%llx could not be mapped\n", base);
        }
        break;
    }
    AcpiPutTable(hdr);
    return found;
}

// The 27 configurator allocates BARs only inside windows the host bridge registers
// (IOPCIConfigurator::addHostBridge reads them back). They come from the bridge's _CRS:
// word/dword/qword address descriptors of type memory (0) or I/O (1).
static inline UInt64 crsRead(const UInt8 *p, unsigned n)
{
    UInt64 v = 0;
    for (unsigned i = 0; i < n; i++) v |= (UInt64)p[i] << (8 * i);
    return v;
}

// The bus-number range this root bridge decodes: the _CRS word/dword/qword address descriptor of type 2. Without it every
// root bridge claimed _BBN..255 and two bridges (PC00, PC02 on the box) configured overlapping trees.
bool A2XACPIPCI::crsBusRange(void)
{
    OSObject *obj = NULL;
    if (fACPI->evaluateObject("_CRS", &obj) != kIOReturnSuccess || !obj) return false;
    OSData *crs = OSDynamicCast(OSData, obj);
    bool found = false;
    if (crs) {
        const UInt8 *b = (const UInt8 *)crs->getBytesNoCopy();
        UInt32 len = crs->getLength(), i = 0;
        while (i < len) {
            UInt8 tag = b[i];
            if (!(tag & 0x80)) {
                if (((tag >> 3) & 0xf) == 0xf) break;
                i += 1 + (tag & 7);
                continue;
            }
            if (i + 3 > len) break;
            UInt32 dlen = (UInt32)crsRead(b + i + 1, 2);
            const UInt8 *d = b + i + 3;
            if (i + 3 + dlen > len) break;
            unsigned w = (tag == 0x88) ? 2 : (tag == 0x87) ? 4 : (tag == 0x8a) ? 8 : 0;
            if (w && dlen >= 3 + 5 * w && (d[0] == 2 && !(d[1] & 1))) {
                UInt64 min = crsRead(d + 3 + 1 * w, w), rlen = crsRead(d + 3 + 4 * w, w);
                if (rlen && min <= 0xff) {
                    UInt64 max = min + rlen - 1; if (max > 0xff) max = 0xff;
                    fBusMin = (UInt8)min; fBusMax = (UInt8)max; found = true;
                    break;
                }
            }
            i += 3 + dlen;
        }
    }
    obj->release();
    return found;
}

// See the patch note: hide one bridge on this root bus from the configurator and remember its windows.
void A2XACPIPCI::hideBridgeFromConfigurator(void)
{
    for (int k = 0; k < 3; k++) { fHideLo[k] = 1; fHideHi[k] = 0; }
    fMask[0] = fMask[1] = fMask[2] = fMask[3] = 0;
    fHold[0] = fHold[1] = fHold[2] = fHold[3] = 0;
    {
        static const char *const hargs[4] = { "a2x_pcihold", "a2x_pcihold2", "a2x_pcihold3", "a2x_pcihold4" };
        for (int a = 0; a < 4; a++) {
            UInt32 h = 0;
            if (!PE_parse_boot_argn(hargs[a], &h, sizeof h) || !h) continue;
            UInt32 hb = (h >> 16) & 0xff;
            if (fSegment != 0 || hb < fBusMin || hb > fBusMax) continue;
            fHold[a] = 0x80000000u | (h & 0xff1f07);
            IOLog("ACPI: PCI %x:%x.%x HELD by %s (configured, no driver until released)\n", hb, (h >> 8) & 0x1f, h & 7, hargs[a]);
        }
    }
    {   // a2x_pcibus=0xBBDDFFSSUU: give a root-bus bridge that firmware left without bus numbers (secondary 0 or ff)
        // the range macOS gives it on the same machine (Mac Pro 6,1 PEG0 0:1.0 -> 16..254, Thunderbolt + USB 3), so
        // the configurator numbers its subtree there and never over the buses firmware already gave the PCH ports.
        UInt64 pb = 0;
        if (PE_parse_boot_argn("a2x_pcibus", &pb, sizeof pb) && pb) {
            UInt32 bus = (UInt32)(pb >> 32) & 0xff, dev = (UInt32)(pb >> 24) & 0x1f, fn = (UInt32)(pb >> 16) & 7;
            UInt32 sec = (UInt32)(pb >> 8) & 0xff, sub = (UInt32)pb & 0xff;
            IOPCIAddressSpace s; s.bits = 0; s.es.busNum = bus; s.es.deviceNum = dev; s.es.functionNum = fn;
            if (fSegment == 0 && bus >= fBusMin && bus <= fBusMax && sec > bus && sub >= sec && sub <= fBusMax) {
                UInt32 id = rawRead32(s, 0x00), hdr = (rawRead32(s, 0x0c) >> 16) & 0x7f, bn = rawRead32(s, 0x18);
                UInt32 osec = (bn >> 8) & 0xff;
                if ((id & 0xffff) != 0xffff && hdr == 1 && (osec == 0 || osec == 0xff)) {
                    configWrite32(s, 0x18, (bn & 0xff000000u) | (sub << 16) | (sec << 8) | bus);
                    IOLog("ACPI: PCI %x:%x.%x buses %02x-%02x (firmware left %02x-%02x) by a2x_pcibus, reads back 0x%08x\n",
                          bus, dev, fn, sec, sub, osec, (bn >> 16) & 0xff, rawRead32(s, 0x18));
                } else IOLog("ACPI: a2x_pcibus %x:%x.%x not applied (id 0x%08x hdr %u buses 0x%08x)\n", bus, dev, fn, id, hdr, bn);
            }
        }
    }
    static const char *const args[4] = { "a2x_pcihide", "a2x_pcihide2", "a2x_pcihide3", "a2x_pcihide4" };
    for (int a = 0; a < 4; a++) {
        UInt32 hide = 0;
        if (!PE_parse_boot_argn(args[a], &hide, sizeof hide) || !hide) continue;
        UInt32 bus = (hide >> 16) & 0xff, dev = (hide >> 8) & 0x1f, fn = hide & 7;
        if (fSegment != 0 || bus < fBusMin || bus > fBusMax) continue;     // not under this root bridge
        IOPCIAddressSpace s; s.bits = 0; s.es.busNum = bus; s.es.deviceNum = dev; s.es.functionNum = fn;
        UInt32 id = rawRead32(s, 0x00), hdr = (rawRead32(s, 0x0c) >> 16) & 0x7f;
        if ((id & 0xffff) == 0xffff) { IOLog("ACPI: %s %x:%x.%x: no device there\n", args[a], bus, dev, fn); continue; }
        if (hdr == 1 && fHideHi[0] < fHideLo[0] && fHideHi[1] < fHideLo[1] && fHideHi[2] < fHideLo[2]) {
            UInt32 v = rawRead32(s, 0x20);                            // memory base/limit
            fHideLo[0] = (UInt64)(v & 0xfff0) << 16; fHideHi[0] = ((UInt64)(v >> 16 & 0xfff0) << 16) | 0xfffff;
            v = rawRead32(s, 0x24);                                   // prefetchable base/limit (+ upper dwords if 64-bit)
            fHideLo[1] = (UInt64)(v & 0xfff0) << 16; fHideHi[1] = ((UInt64)(v >> 16 & 0xfff0) << 16) | 0xfffff;
            if ((v & 0xf) == 1) { fHideLo[1] |= (UInt64)rawRead32(s, 0x28) << 32; fHideHi[1] |= (UInt64)rawRead32(s, 0x2c) << 32; }
            v = rawRead32(s, 0x1c);                                   // I/O base/limit (+ upper words if 32-bit I/O)
            UInt32 up = ((v & 0xf) == 1) ? rawRead32(s, 0x30) : 0;
            fHideLo[2] = ((UInt64)(v & 0xf0) << 8) | ((UInt64)(up & 0xffff) << 16);
            fHideHi[2] = ((UInt64)(v >> 8 & 0xf0) << 8) | 0xfff | ((UInt64)(up >> 16) << 16);
        }
        fMask[a] = 0x80000000u | (bus << 16) | (dev << 8) | fn;           // from here on the slot reads empty
        if (hdr == 1) {   // record the hidden subtree exactly as firmware left it (ioreg a2x-hidden-tree)
            static char tree[6144]; int o = 0;
            UInt32 bn = rawRead32(s, 0x18);
            UInt32 sec = (bn >> 8) & 0xff, sub = (bn >> 16) & 0xff;
            o += snprintf(tree + o, sizeof tree - o, "%x:%x.%x bus %02x-%02x;", bus, dev, fn, sec, sub);
            for (UInt32 b = sec; b && b <= sub && b <= fBusMax && o < (int)sizeof tree - 160; b++)
                for (UInt32 d = 0; d < 32 && o < (int)sizeof tree - 160; d++)
                    for (UInt32 f = 0; f < 8 && o < (int)sizeof tree - 160; f++) {
                        IOPCIAddressSpace t; t.bits = 0; t.es.busNum = b; t.es.deviceNum = d; t.es.functionNum = f;
                        UInt32 tid = rawRead32(t, 0x00);
                        if ((tid & 0xffff) == 0xffff) { if (f == 0) break; continue; }
                        UInt32 th = rawRead32(t, 0x0c), tc = rawRead32(t, 0x08);
                        o += snprintf(tree + o, sizeof tree - o, " %x:%x.%x %04x:%04x c%06x h%02x", b, d, f,
                                      tid & 0xffff, tid >> 16, tc >> 8, (th >> 16) & 0xff);
                        if (((th >> 16) & 0x7f) == 1) {
                            UInt32 tb = rawRead32(t, 0x18);
                            o += snprintf(tree + o, sizeof tree - o, " b%02x-%02x-%02x m%08x p%08x", tb & 0xff,
                                          (tb >> 8) & 0xff, (tb >> 16) & 0xff, rawRead32(t, 0x20), rawRead32(t, 0x24));
                        } else o += snprintf(tree + o, sizeof tree - o, " bar0 %08x", rawRead32(t, 0x10));
                        o += snprintf(tree + o, sizeof tree - o, ";");
                        if (f == 0 && !((th >> 16) & 0x80)) break;
                    }
            setProperty("a2x-hidden-tree", tree);
        }
        IOLog("ACPI: PCI %x:%x.%x (id 0x%08x, hdr %u) MASKED by %s%s\n", bus, dev, fn, id, hdr, args[a],
              hdr == 1 ? " - bridge windows kept out of the host ranges" : "");
    }
}

// Register [min, min+len) minus the hidden bridge's windows of the same kind (memory windows exclude from any memory
// range). kind: 0 memory, 2 I/O.
bool A2XACPIPCI::addRangeExcluding(unsigned kind, UInt64 min, UInt64 len, bool pf)
{
    UInt64 lo[4] = { min }, hi[4] = { min + len - 1 }; unsigned n = 1;
    for (int k = 0; k < 3; k++) {
        if ((kind == 2) != (k == 2) || fHideHi[k] < fHideLo[k]) continue;
        UInt64 nlo[4], nhi[4]; unsigned m = 0;
        for (unsigned i = 0; i < n; i++) {
            if (fHideHi[k] < lo[i] || fHideLo[k] > hi[i]) { nlo[m] = lo[i]; nhi[m++] = hi[i]; continue; }
            if (fHideLo[k] > lo[i] && m < 4) { nlo[m] = lo[i]; nhi[m++] = fHideLo[k] - 1; }
            if (fHideHi[k] < hi[i] && m < 4) { nlo[m] = fHideHi[k] + 1; nhi[m++] = hi[i]; }
        }
        for (unsigned i = 0; i < m; i++) { lo[i] = nlo[i]; hi[i] = nhi[i]; }
        n = m;
    }
    bool any = false;
    for (unsigned i = 0; i < n; i++) {
        UInt64 l = hi[i] - lo[i] + 1;
        bool ok = (kind == 2) ? addBridgeIORange((IOByteCount)lo[i], (IOByteCount)l)
                : pf ? addBridgePrefetchableMemoryRange((addr64_t)lo[i], (addr64_t)l)
                     : addBridgeMemoryRange((IOPhysicalAddress)lo[i], (IOPhysicalLength)l, true);
        IOLog("ACPI: PCI window %s 0x%llx len 0x%llx %s%s\n", kind == 2 ? "io" : pf ? "pfmem" : "mem", lo[i], l,
              ok ? "ok" : "FAILED", (n != 1 || lo[i] != min || l != len) ? " (hidden bridge cut out)" : "");
        any |= ok;
    }
    return any;
}

// See confollow.h. The host bridge whose root bus holds the boot screen records where it is BEFORE its configurator
// runs (after masking, so a masked port is never tracked); the platform expert asks again at the outermost enable.
static A2XACPIPCI   *gConHost;
static struct cf_loc gConLoc;

UInt32 A2XACPIPCI::cfRead(void *ctx, UInt8 bus, UInt8 dev, UInt8 fn, UInt8 reg)
{
    A2XACPIPCI *h = (A2XACPIPCI *)ctx;
    if (bus < h->fBusMin || bus > h->fBusMax) return 0xffffffffu;
    IOPCIAddressSpace s; s.bits = 0; s.es.busNum = bus; s.es.deviceNum = dev & 0x1f; s.es.functionNum = fn & 7;
    return h->rawRead32(s, reg);
}

void A2XACPIPCI::trackConsole(void)
{
    if (gConHost) return;                                         // one boot screen
    PE_Video v; bzero(&v, sizeof v);
    IOPlatformExpert *pe = getPlatform();
    if (!pe || pe->getConsoleInfo(&v) != kIOReturnSuccess || !v.v_baseAddr) return;
    const uint64_t fb = (uint64_t)v.v_baseAddr & ~3ULL;
    struct cf_loc l;
    if (!cf_locate(&A2XACPIPCI::cfRead, this, fBusMin, fb, &l)) {
        IOLog("ACPI: PCI segment %u bus %u: boot screen 0x%llx is not below this bridge\n", fSegment, fBusMin, fb);
        return;
    }
    gConLoc = l;
    retain();                                                     // held for the life of the boot
    __atomic_store_n(&gConHost, this, __ATOMIC_RELEASE);
    IOLog("ACPI: boot screen 0x%llx = %s%x:%x.%x -> function %x.%x BAR%u +0x%llx (id 0x%08x); the console follows it\n",
          fb, l.viaBridge ? "behind bridge " : "on root bus ", l.rootBus, l.bdev, l.bfn, l.dev, l.fn, l.bar,
          (unsigned long long)l.off, l.id);
}

extern "C" int a2x_console_where(void *, uint64_t *phys)
{
    A2XACPIPCI *h = __atomic_load_n(&gConHost, __ATOMIC_ACQUIRE);
    if (!h) return -1;
    int rc = cf_where(&A2XACPIPCI::cfRead, h, &gConLoc, phys);
    IOLog("ACPI: boot screen now: %s 0x%llx\n", rc > 0 ? "at" : rc == 0 ? "NOT DECODING" : "untracked", rc > 0 ? *phys : 0ULL);
    return rc;
}

unsigned A2XACPIPCI::addCRSRanges(void)
{
    OSObject *obj = NULL;
    if (fACPI->evaluateObject("_CRS", &obj) != kIOReturnSuccess || !obj) {
        IOLog("ACPI: PCI segment %u has no _CRS\n", fSegment);
        return 0;
    }
    OSData *crs = OSDynamicCast(OSData, obj);
    unsigned added = 0;
    if (crs) {
        const UInt8 *b = (const UInt8 *)crs->getBytesNoCopy();
        UInt32 len = crs->getLength(), i = 0;
        while (i < len) {
            UInt8 tag = b[i];
            if (!(tag & 0x80)) {                         // small descriptor
                if (((tag >> 3) & 0xf) == 0xf) break;    // end tag
                i += 1 + (tag & 7);
                continue;
            }
            if (i + 3 > len) break;
            UInt32 dlen = (UInt32)crsRead(b + i + 1, 2);
            const UInt8 *d = b + i + 3;
            if (i + 3 + dlen > len) break;
            unsigned w = (tag == 0x88) ? 2 : (tag == 0x87) ? 4 : (tag == 0x8a) ? 8 : 0;
            if (w && dlen >= 3 + 5 * w) {
                UInt8  type  = d[0];
                UInt8  gflag = d[1];
                UInt8  tflag = d[2];
                UInt64 min   = crsRead(d + 3 + 1 * w, w);
                UInt64 tra   = crsRead(d + 3 + 3 * w, w);
                UInt64 rlen  = crsRead(d + 3 + 4 * w, w);
                if (gflag & 1) {                         // ResourceConsumer: the bridge's own registers, not a window
                    if (rlen) IOLog("ACPI: PCI segment %u: consumer range type %u 0x%llx len 0x%llx skipped\n",
                                    fSegment, type, min, rlen);
                    rlen = 0;
                }
                min += tra;                              // bus address -> CPU address
                if (rlen && type == 0) {
                    added += addRangeExcluding(0, min, rlen, ((tflag >> 1) & 3) == 3);
                } else if (rlen && type == 1) {
                    added += addRangeExcluding(2, min, rlen, false);
                }
            }
            i += 3 + dlen;
        }
    }
    obj->release();
    return added;
}

bool A2XACPIPCI::start(IOService *provider)
{
    fACPI = OSDynamicCast(IOACPIPlatformDevice, provider);
    if (!fACPI) return false;

    UInt32 v = 0;
    fSegment = (fACPI->evaluateInteger("_SEG", &v) == kIOReturnSuccess) ? (UInt16)v : 0;
    fBusMin  = (fACPI->evaluateInteger("_BBN", &v) == kIOReturnSuccess) ? (UInt8)v : 0;
    fBusMax  = 0xff;
    if (crsBusRange()) IOLog("ACPI: PCI segment %u buses %u..%u from _CRS\n", fSegment, fBusMin, fBusMax);

    fIOMem = IODeviceMemory::withRange(0, 0x10000);
    fCapLock = IOSimpleLockAlloc();
    if (!fCapLock) return false;
    if (!findECAM()) {
        if (fSegment != 0) { IOLog("ACPI: PCI segment %u has no MCFG allocation\n", fSegment); return false; }
        fCF8Lock = IOSimpleLockAlloc();
        if (!fCF8Lock) return false;
        IOLog("ACPI: PCI buses %u..%u through 0xCF8 (no MCFG allocation)\n", fBusMin, fBusMax);
    }
    a2x_crumb_ring(0xB0000000u | ((UInt32)(fSegment & 0xff) << 16) | ((UInt32)fBusMin << 8) | fBusMax, 0xB0B0B0B0u);
    a2x_crumb_ring_done();
    IOLog("ACPI: PCI segment %u bridge buses 0x%x..0x%x handed to the configurator\n", fSegment, fBusMin, fBusMax);
    hideBridgeFromConfigurator();
    if (fSegment == 0) trackConsole();
    if (!addCRSRanges()) IOLog("ACPI: PCI segment %u: no windows from _CRS, BARs cannot be assigned\n", fSegment);
    {   // _OSC: take native PCIe control before the configurator touches the root ports (see patch note)
        static const UInt8 uuid[16] = { 0x5b,0x4d,0xdb,0x33, 0xf7,0x1f, 0x1c,0x40, 0x96,0x57, 0x74,0x41,0xc0,0x3d,0xd7,0x66 };
        UInt32 cdw[3] = { 0, 0x1F, 0x5 };    // Apple: support 0x1F, control 5 (hot-plug | PME)
        OSData   *u  = OSData::withBytes(uuid, sizeof uuid);
        OSNumber *rv = OSNumber::withNumber(1ULL, 32);
        OSNumber *cn = OSNumber::withNumber(3ULL, 32);
        OSData   *cb = OSData::withBytes(cdw, sizeof cdw);
        OSObject *args[4] = { u, rv, cn, cb };
        OSObject *res = NULL;
        IOReturn r = (u && rv && cn && cb) ? fACPI->evaluateObject("_OSC", &res, args, 4) : kIOReturnNoMemory;
        OSData *rd = OSDynamicCast(OSData, res);
        if (r == kIOReturnSuccess && rd && rd->getLength() >= 12) {
            const UInt32 *g = (const UInt32 *)rd->getBytesNoCopy();
            IOLog("ACPI: PCI segment %u bus %u _OSC: status 0x%x, control granted 0x%x (asked 0x5)\n", fSegment, fBusMin, g[0], g[2]);
        } else {
            IOLog("ACPI: PCI segment %u bus %u _OSC not evaluated (0x%x)\n", fSegment, fBusMin, r);
        }
        OSSafeReleaseNULL(res); OSSafeReleaseNULL(u); OSSafeReleaseNULL(rv); OSSafeReleaseNULL(cn); OSSafeReleaseNULL(cb);
    }
    return IOPCIHostBridge::start(provider);
}

void A2XACPIPCI::free(void)
{
    OSSafeReleaseNULL(fECAMMap);
    OSSafeReleaseNULL(fIOMem);
    if (fCF8Lock) { IOSimpleLockFree(fCF8Lock); fCF8Lock = NULL; }
    if (fCapLock) { IOSimpleLockFree(fCapLock); fCapLock = NULL; }
    IOPCIHostBridge::free();
}

// A function that does not exist reads all ones, as on the bus.
UInt32 A2XACPIPCI::rawRead32(IOPCIAddressSpace space, UInt8 offset)
{
    if (fECAM) { volatile UInt8 *a = ecam(space, offset); return a ? *(volatile UInt32 *)a : 0xffffffff; }
    if (space.es.registerNumExtended) return 0xffffffff;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fCF8Lock);
    outl(0xcf8, cf8(space, offset)); UInt32 r = inl(0xcfc);
    IOSimpleLockUnlockEnableInterrupt(fCF8Lock, is);
    return r;
}

void A2XACPIPCI::configWrite32(IOPCIAddressSpace space, UInt8 offset, UInt32 data)
{
    A2XCrumbScope crumb(0x12, (((UInt32)space.es.busNum << 16) | ((UInt32)space.es.deviceNum << 11) | ((UInt32)space.es.functionNum << 8) | offset));
    struct RingDone { ~RingDone() { a2x_crumb_ring_done(); } } ringDone;
    a2x_crumb_ring(0x40000000u | (((UInt32)space.es.registerNumExtended & 0xf) << 24) | ((UInt32)space.es.busNum << 16) | ((UInt32)space.es.deviceNum << 11) | ((UInt32)space.es.functionNum << 8) | offset, (UInt32)data);   // top nibble = width in bytes
    if (fECAM) { volatile UInt8 *a = ecam(space, offset); if (a) *(volatile UInt32 *)a = data; return; }
    if (space.es.registerNumExtended) return;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fCF8Lock);
    outl(0xcf8, cf8(space, offset)); outl(0xcfc, data);
    IOSimpleLockUnlockEnableInterrupt(fCF8Lock, is);
}

UInt16 A2XACPIPCI::rawRead16(IOPCIAddressSpace space, UInt8 offset)
{
    if (fECAM) { volatile UInt8 *a = ecam(space, offset); return a ? *(volatile UInt16 *)a : 0xffff; }
    if (space.es.registerNumExtended) return 0xffff;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fCF8Lock);
    outl(0xcf8, cf8(space, offset)); UInt16 r = inw(0xcfc + (offset & 2));
    IOSimpleLockUnlockEnableInterrupt(fCF8Lock, is);
    return r;
}

void A2XACPIPCI::configWrite16(IOPCIAddressSpace space, UInt8 offset, UInt16 data)
{
    A2XCrumbScope crumb(0x12, (((UInt32)space.es.busNum << 16) | ((UInt32)space.es.deviceNum << 11) | ((UInt32)space.es.functionNum << 8) | offset));
    struct RingDone { ~RingDone() { a2x_crumb_ring_done(); } } ringDone;
    a2x_crumb_ring(0x20000000u | (((UInt32)space.es.registerNumExtended & 0xf) << 24) | ((UInt32)space.es.busNum << 16) | ((UInt32)space.es.deviceNum << 11) | ((UInt32)space.es.functionNum << 8) | offset, (UInt32)data);   // top nibble = width in bytes
    if (fECAM) { volatile UInt8 *a = ecam(space, offset); if (a) *(volatile UInt16 *)a = data; return; }
    if (space.es.registerNumExtended) return;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fCF8Lock);
    outl(0xcf8, cf8(space, offset)); outw(0xcfc + (offset & 2), data);
    IOSimpleLockUnlockEnableInterrupt(fCF8Lock, is);
}

UInt8 A2XACPIPCI::rawRead8(IOPCIAddressSpace space, UInt8 offset)
{
    if (fECAM) { volatile UInt8 *a = ecam(space, offset); return a ? *a : 0xff; }
    if (space.es.registerNumExtended) return 0xff;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fCF8Lock);
    outl(0xcf8, cf8(space, offset)); UInt8 r = inb(0xcfc + (offset & 3));
    IOSimpleLockUnlockEnableInterrupt(fCF8Lock, is);
    return r;
}

void A2XACPIPCI::configWrite8(IOPCIAddressSpace space, UInt8 offset, UInt8 data)
{
    A2XCrumbScope crumb(0x12, (((UInt32)space.es.busNum << 16) | ((UInt32)space.es.deviceNum << 11) | ((UInt32)space.es.functionNum << 8) | offset));
    struct RingDone { ~RingDone() { a2x_crumb_ring_done(); } } ringDone;
    a2x_crumb_ring(0x10000000u | (((UInt32)space.es.registerNumExtended & 0xf) << 24) | ((UInt32)space.es.busNum << 16) | ((UInt32)space.es.deviceNum << 11) | ((UInt32)space.es.functionNum << 8) | offset, (UInt32)data);   // top nibble = width in bytes
    if (fECAM) { volatile UInt8 *a = ecam(space, offset); if (a) *a = data; return; }
    if (space.es.registerNumExtended) return;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fCF8Lock);
    outl(0xcf8, cf8(space, offset)); outb(0xcfc + (offset & 3), data);
    IOSimpleLockUnlockEnableInterrupt(fCF8Lock, is);
}

// Capability list order.
//
// IOPCIFamily 27 caches each function's capabilities, and IOPCIDevice::extendedFindPCICapability continues a
// search by returning the first cached entry whose offset is above the caller's. That only enumerates every
// instance when the list is linked in ascending offset order. A device that links it descending (QEMU's
// virtio-pci: 0x98 -> 0x84 -> 0x70 -> 0x60 ...) makes "next" go backwards, and AppleVirtIOPCITransport::start
// then alternates between two vendor capabilities forever on the matching thread. The bridge therefore
// presents every function's standard capability chain (0x34 and each header's next byte) sorted ascending.
// The capabilities themselves are unchanged; only the links are reordered.

struct A2XCapMap {
    UInt32 key;        // bit 31 set when the slot is used
    UInt8  head;       // value presented at 0x34
    UInt8  hasCaps;
    UInt64 isCap;      // bit (offset >> 2) set for each capability header
    UInt8  next[64];   // presented next pointer, indexed by header offset >> 2
};
static A2XCapMap gCapMap[256];
static bool      gCapMapFullLogged;

UInt32 A2XACPIPCI::capDword(IOPCIAddressSpace space, UInt8 offset)
{
    UInt8  d = offset & 0xfc;
    UInt32 v = rawRead32(space, d);
    if (space.es.registerNumExtended || (d != 0x34 && d < 0x40)) return v;

    UInt32 key = 0x80000000u | ((UInt32)fSegment << 16) | ((UInt32)space.es.busNum << 8)
               | ((UInt32)space.es.deviceNum << 3) | space.es.functionNum;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fCapLock);
    A2XCapMap *m = NULL, *freeSlot = NULL;
    for (unsigned i = 0; i < 256; i++) {
        if (gCapMap[i].key == key) { m = &gCapMap[i]; break; }
        if (!gCapMap[i].key && !freeSlot) freeSlot = &gCapMap[i];
    }
    bool reordered = false;
    if (!m) {
        // A missing function reads all ones; it is not cached, so it is walked again once it appears.
        if ((rawRead32(space, 0) & 0xffff) == 0xffff) { IOSimpleLockUnlockEnableInterrupt(fCapLock, is); return v; }
        if (!freeSlot) {
            bool log = !gCapMapFullLogged; gCapMapFullLogged = true;
            IOSimpleLockUnlockEnableInterrupt(fCapLock, is);
            if (log) IOLog("ACPI: PCI capability map full, later functions keep their own link order\n");
            return v;
        }
        m = freeSlot;
        bzero(m, sizeof(*m));
        if (rawRead16(space, 0x06) & 0x10) {
            UInt8 caps[48]; unsigned n = 0;
            UInt8 p = rawRead8(space, 0x34) & 0xfc;
            while (p >= 0x40 && n < 48 && !(m->isCap & (1ULL << (p >> 2)))) {
                m->isCap |= 1ULL << (p >> 2);
                caps[n++] = p;
                p = rawRead8(space, p + 1) & 0xfc;
            }
            for (unsigned i = 1; i < n; i++) {
                UInt8 c = caps[i]; unsigned j = i;
                while (j && caps[j - 1] > c) { caps[j] = caps[j - 1]; j--; reordered = true; }
                caps[j] = c;
            }
            if (n) {
                m->hasCaps = 1;
                m->head = caps[0];
                for (unsigned i = 0; i < n; i++) m->next[caps[i] >> 2] = (i + 1 < n) ? caps[i + 1] : 0;
            }
        }
        m->key = key;
    }
    if (m->hasCaps) {
        if (d == 0x34) v = (v & ~0xffu) | m->head;
        else if (m->isCap & (1ULL << (d >> 2))) v = (v & ~0xff00u) | ((UInt32)m->next[d >> 2] << 8);
    }
    IOSimpleLockUnlockEnableInterrupt(fCapLock, is);
    if (reordered)
        IOLog("ACPI: PCI %u:%u.%u capability links presented in ascending order\n",
              space.es.busNum, space.es.deviceNum, space.es.functionNum);
    return v;
}

UInt32 A2XACPIPCI::configRead32(IOPCIAddressSpace space, UInt8 offset)
{
    A2XCrumbScope crumb(0x10, (((UInt32)space.es.busNum << 16) | ((UInt32)space.es.deviceNum << 11) | ((UInt32)space.es.functionNum << 8) | offset));
    return heldFix(space, offset & 0xfc, capDword(space, offset));
}

// Held function: dword 0x00 reads vendor:0xFFFE, dword 0x08 reads class 0xFF0000 (revision kept). See patch note.
UInt32 A2XACPIPCI::heldFix(IOPCIAddressSpace space, UInt8 dword, UInt32 v) const
{
    if (space.es.registerNumExtended || (dword != 0x00 && dword != 0x08) || !(fHold[0] | fHold[1] | fHold[2] | fHold[3])) return v;
    UInt32 key = 0x80000000u | ((UInt32)space.es.busNum << 16) | ((UInt32)space.es.deviceNum << 8) | space.es.functionNum;
    if (key != fHold[0] && key != fHold[1] && key != fHold[2] && key != fHold[3]) return v;
    return dword == 0x00 ? ((v & 0xffff) | 0xfffe0000u) : (0xff000000u | (v & 0xff));
}

IOReturn A2XACPIPCI::releaseHold(UInt32 key)
{
    key = 0x80000000u | (key & 0xff1f07);
    int slot = -1;
    for (int i = 0; i < 4; i++) if (fHold[i] == key) slot = i;
    if (slot < 0) return kIOReturnNotFound;
    fHold[slot] = 0;
    UInt32 bus = (key >> 16) & 0xff, dev = (key >> 8) & 0x1f, fn = key & 7;
    IOPCIAddressSpace sp; sp.bits = 0; sp.es.busNum = bus; sp.es.deviceNum = dev; sp.es.functionNum = fn;
    UInt32 id = rawRead32(sp, 0x00), cls = rawRead32(sp, 0x08);
    IOPCIDevice *found = NULL;
    OSDictionary *m = IOService::serviceMatching("IOPCIDevice");
    OSIterator *it = m ? IOService::getMatchingServices(m) : NULL;
    OSSafeReleaseNULL(m);
    if (it) {
        IOPCIDevice *d;
        while ((d = OSDynamicCast(IOPCIDevice, it->getNextObject())))
            if (d->getBusNumber() == bus && d->getDeviceNumber() == dev && d->getFunctionNumber() == fn) { found = d; found->retain(); break; }
        it->release();
    }
    if (!found) { IOLog("ACPI: release %x:%x.%x: no nub\n", bus, dev, fn); return kIOReturnNotFound; }
    if (found->savedConfig) { found->savedConfig[0] = id; found->savedConfig[2] = cls; }
    UInt32 did = id >> 16, ccode = cls >> 8;
    found->setProperty("device-id", &did, 4);
    found->setProperty("class-code", &ccode, 4);
    IOLog("ACPI: PCI %x:%x.%x RELEASED (id 0x%08x class 0x%06x) - matching drivers now\n", bus, dev, fn, id, ccode);
    found->registerService();
    found->release();
    return kIOReturnSuccess;
}

IOReturn A2XACPIPCI::setProperties(OSObject *properties)
{
    OSDictionary *d = OSDynamicCast(OSDictionary, properties);
    OSNumber *n = d ? OSDynamicCast(OSNumber, d->getObject("a2x-release")) : NULL;
    if (n) return releaseHold(n->unsigned32BitValue());
    return IOPCIHostBridge::setProperties(properties);
}

UInt16 A2XACPIPCI::configRead16(IOPCIAddressSpace space, UInt8 offset)
{
    A2XCrumbScope crumb(0x10, (((UInt32)space.es.busNum << 16) | ((UInt32)space.es.deviceNum << 11) | ((UInt32)space.es.functionNum << 8) | offset));
    if (!space.es.registerNumExtended && ((offset & 0xfc) == 0x34 || offset >= 0x40)) return (UInt16)(capDword(space, offset) >> ((offset & 2) * 8));
    if (!space.es.registerNumExtended && ((offset & 0xfc) == 0x00 || (offset & 0xfc) == 0x08))
        return (UInt16)(heldFix(space, offset & 0xfc, rawRead32(space, offset & 0xfc)) >> ((offset & 2) * 8));
    return rawRead16(space, offset);
}

UInt8 A2XACPIPCI::configRead8(IOPCIAddressSpace space, UInt8 offset)
{
    A2XCrumbScope crumb(0x10, (((UInt32)space.es.busNum << 16) | ((UInt32)space.es.deviceNum << 11) | ((UInt32)space.es.functionNum << 8) | offset));
    if (!space.es.registerNumExtended && ((offset & 0xfc) == 0x34 || offset >= 0x40)) return (UInt8)(capDword(space, offset) >> ((offset & 3) * 8));
    if (!space.es.registerNumExtended && ((offset & 0xfc) == 0x00 || (offset & 0xfc) == 0x08))
        return (UInt8)(heldFix(space, offset & 0xfc, rawRead32(space, offset & 0xfc)) >> ((offset & 3) * 8));
    return rawRead8(space, offset);
}
