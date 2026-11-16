// SPDX-License-Identifier: BSD-3-Clause
//
// cpuint.cpp - the platform (CPU) interrupt controller. See cpuint.h for the routing contract.

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSNumber.h>
#include "cpuint.h"
#include "crumb.h"
#include <kern/debug.h>
#include <mach/mach_time.h>

extern "C" void lapic_end_of_interrupt(void);
extern "C" kern_return_t ml_processor_register(void *cpu_id, uint32_t lapic_id, processor_t *processor_out,
                                               boolean_t boot_cpu, boolean_t start);

// Boot diagnostic: deliveries per vector (CPU 0), and how many were claimed by a child.
static volatile UInt32 gA2XVecCount[256];
static volatile UInt32 gA2XVecClaimed[256];
void A2XDumpVectorCounts(void)
{
    char line[512];
    int n = 0, off = 0;
    for (int v = 0; v < 256 && off < (int)sizeof line - 32; v++) {
        if (gA2XVecCount[v]) {
            off += snprintf(line + off, sizeof line - off, " %02x:%u/%u", v, gA2XVecCount[v], gA2XVecClaimed[v]);
            n++;
        }
    }
    line[off] = 0;
    IOLog("ACPI: vectors (%d)%s\n", n, line);
    {   // PCH EHCI 0:1d.0 registers (BAR0 as firmware assigned it on the Mac Pro; checked by HCIVERSION)
        static IOMemoryMap *sMap;
        if (!sMap) {
            IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(0xa0827000ULL, 0x1000, kIODirectionInOut);
            if (md) { sMap = md->map(kIOMapInhibitCache); md->release(); }
        }
        if (sMap) {
            volatile UInt8 *cap = (volatile UInt8 *)sMap->getVirtualAddress() + 0x800;
            UInt32 c0 = *(volatile UInt32 *)cap;
            if ((c0 >> 16) == 0x0100) {
                volatile UInt32 *op = (volatile UInt32 *)(cap + (c0 & 0xff));
                IOLog("ACPI: ehci hcc %08x cmd %08x sts %08x intr %08x frindex %08x seg %08x periodic %08x async %08x "
                      "cfg %08x portsc %08x %08x\n", *(volatile UInt32 *)(cap + 8), op[0], op[1], op[2], op[3], op[4],
                      op[5], op[6], op[16], op[17], op[18]);
            } else IOLog("ACPI: ehci at 0xa0827800 reads 0x%08x - not an EHCI capability block\n", c0);
        }
    }
}

// IOPCIFamily (exported, non-virtual). Indices are relative to the controller's Base Vector Number.
class IOPCIMessagedInterruptController : public IOInterruptController
{
public:
    bool reserveVectors(UInt32 vector, UInt32 count);
};

#define kA2XMSRAPICBase     0x1b
#define kA2XAPICBaseX2      (1ULL << 10)
#define kA2XMSRx2ISR        0x810          // ISR word 0; words 1..7 follow
#define kA2XLAPICISR        0x100          // xAPIC ISR word 0; words are 0x10 apart
#define kA2XMSRx2APICID     0x802
#define kA2XLAPICID         0x20           // xAPIC id register, id in bits 31:24

static inline void a2x_wrmsr(UInt32 msr, UInt64 v)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((UInt32)v), "d"((UInt32)(v >> 32)));
}
#define kA2XMSRx2ICR        0x830
#define kA2XLAPICICRLo      0x300
#define kA2XLAPICICRHi      0x310

static inline UInt64 a2x_rdmsr(UInt32 msr)
{
    UInt32 lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((UInt64)hi << 32) | lo;
}

// Some firmware (seen on a Lenovo laptop) leaves the boot processor's time-stamp counter offset by about -1.7 s
// in IA32_TSC_ADJUST (MSR 0x3B) and every other processor at 0. The processors then disagree about the time, and the
// kernel stops with "Non-monotonic time" as soon as a thread moves between them. On macOS 26 a kext that hides this
// was in the PC's own build; it has never run on macOS 27. The register exists for exactly this repair (Intel SDM
// vol. 3, 18.17.3: writing it moves the counter by the difference), and CPUID leaf 7 says whether it is there.
// Only a negative offset is cleared: that moves this processor's counter forward, which time may do. A positive one
// would move it back, so it is reported and left. Called on the boot processor while it is the only one running.
#define kA2XMSRTSCAdjust    0x3B
static void a2xClearBootTSCAdjust(void)
{
    UInt32 a = 7, b = 0, c = 0, d = 0;
    __asm__ volatile ("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (!(b & (1u << 1))) return;                       // no IA32_TSC_ADJUST on this processor
    SInt64 adj = (SInt64)a2x_rdmsr(kA2XMSRTSCAdjust);
    if (adj == 0) return;
    if (adj > 0) { IOLog("ACPI: boot processor clock offset +%lld counts left as it is\n", adj); return; }
    UInt32 lo = 0, hi = 0;
    __asm__ volatile ("wrmsr" : : "c"((UInt32)kA2XMSRTSCAdjust), "a"(lo), "d"(hi));
    IOLog("ACPI: boot processor clock offset %lld counts cleared before the other processors start\n", adj);
}

static UInt32 a2xNumberProp(IORegistryEntry *e, const char *key, UInt32 dflt, bool *found)
{
    OSNumber *n = e ? OSDynamicCast(OSNumber, e->getProperty(key)) : NULL;
    if (found) *found = (n != NULL);
    return n ? n->unsigned32BitValue() : dflt;
}

// ================================================================ boot CPU

OSDefineMetaClassAndStructors(A2XBootCPU, IOCPU)

bool A2XBootCPU::initNumber(UInt32 n)
{
    if (!IOService::init()) return false;
    setCPUNumber(n);
    return true;
}

void A2XBootCPU::initCPU(bool boot) {}
void A2XBootCPU::quiesceCPU(void) {}
kern_return_t A2XBootCPU::startCPU(vm_offset_t, vm_offset_t) { return KERN_FAILURE; }
void A2XBootCPU::haltCPU(void) {}
const OSSymbol *A2XBootCPU::getCPUName(void)
{
    char n[16];
    snprintf(n, sizeof n, "CPU%u", (unsigned)getCPUNumber());
    return OSSymbol::withCString(n);
}

// ================================================================ controller

OSDefineMetaClassAndStructors(AppleACPICPU, A2XBootCPU)

bool AppleACPICPU::publishOn(IOService *procNub)
{
    UInt32 n = getCPUNumber();
    if (!attach(procNub)) return false;
    if (!IOCPU::start(procNub)) { detach(procNub); return false; }
    setCPUNumber(n);                      // IOCPU::start resets it to 0
    setCPUState(3);                       // kIOCPUStateRunning (xnu IOCPU.h enum: Unregistered 0, Uninitalized, Stopped, Running); ml_processor_register ran in startProcessors
    registerService();
    return true;
}

void A2XCPUInterruptController::publishProcessors(IOService * const *nubs, UInt32 n)
{
    for (UInt32 i = 0; i < fCPUCount && i < n; i++) {
        AppleACPICPU *c = OSDynamicCast(AppleACPICPU, fCPU[i]);
        if (c && nubs[i] && !c->publishOn(nubs[i])) IOLog("ACPI: CPU%u not published on %s\n", i, nubs[i]->getName());
    }
}

OSDefineMetaClassAndStructors(A2XCPUInterruptController, IOCPUInterruptController)

bool A2XCPUInterruptController::initController(IOService *platform, const UInt32 *lapicIDs, UInt32 count)
{
    fLock = IOLockAlloc();
    if (!fLock) return false;

    fX2APIC = (a2x_rdmsr(kA2XMSRAPICBase) & kA2XAPICBaseX2) != 0;
    if (!fX2APIC) {
        UInt64 base = a2x_rdmsr(kA2XMSRAPICBase) & 0xfffff000ULL;
        IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(base, 0x1000,
                                     kIODirectionInOut | kIOMemoryMapperNone, NULL);
        if (md) {
            fLAPICMap = md->map(kIOMapInhibitCache);
            md->release();
        }
        if (fLAPICMap) fLAPIC = (volatile UInt32 *)fLAPICMap->getVirtualAddress();
    }
    IOLog("ACPI: CPU interrupt controller: %s local APIC%s\n", fX2APIC ? "x2" : "x",
          (fX2APIC || fLAPIC) ? "" : " (in-service register not mapped)");

    // The processor list: this (boot) processor first, then every other enabled MADT entry by ascending id.
    // Without our own id, or with a2xcpus=1, only the boot processor runs (the single-processor configuration).
    bool haveSelf = fX2APIC || fLAPIC;
    UInt32 self = fX2APIC ? (UInt32)a2x_rdmsr(kA2XMSRx2APICID) : (fLAPIC ? (fLAPIC[kA2XLAPICID / 4] >> 24) : 0);
    UInt32 limit = kA2XMaxCPUs, arg = 0;
    if (PE_parse_boot_argn("a2xcpus", &arg, sizeof arg) && arg >= 1 && arg < limit) limit = arg;
    fCPUCount = 1;
    fCPULapic[0] = self;
    bool selfListed = false;
    for (UInt32 i = 0; i < count; i++) if (lapicIDs[i] == self) selfListed = true;
    if (haveSelf && selfListed) {
        UInt32 last = self, have = 1;
        while (fCPUCount < limit) {
            bool found = false; UInt32 next = 0;
            for (UInt32 i = 0; i < count; i++) {
                UInt32 id = lapicIDs[i];
                if (id == self || (have > 1 && id <= last)) continue;
                if (!found || id < next) { next = id; found = true; }
            }
            if (!found) break;
            fCPULapic[fCPUCount++] = next; last = next; have++;
        }
    }
    IOLog("ACPI: %u processor%s (MADT %u, boot local APIC 0x%x%s%s)\n", fCPUCount, fCPUCount == 1 ? "" : "s",
          count, self, haveSelf ? "" : ", own id unreadable", selfListed ? "" : ", not in MADT");

    for (UInt32 i = 0; i < fCPUCount; i++) {
        fCPU[i] = OSTypeAlloc(AppleACPICPU);
        if (!fCPU[i] || !fCPU[i]->initNumber(i)) { OSSafeReleaseNULL(fCPU[i]); return false; }
    }

    // Register every processor before the maximum is published. Publishing it releases commpage_populate, which
    // copies the kernel's logical and physical CPU counts into the commpage once; those counts grow during this
    // pass. User space sizes per-CPU structures from the commpage value and indexes them by real CPU number.
    if (fCPUCount > 1) {
        processor_t proc;
        for (UInt32 i = 0; i < fCPUCount; i++) {
            kern_return_t kr = ml_processor_register(fCPU[i], fCPULapic[i], &proc, i == 0, FALSE);
            if (kr != KERN_SUCCESS) IOLog("ACPI: CPU%u (local APIC 0x%x) register: %d\n", i, fCPULapic[i], kr);
        }
    }

    // The source count is also the kernel's maximum CPU count (ml_set_max_cpus).
    if (IOCPUInterruptController::initCPUInterruptController(fCPUCount, fCPUCount) != kIOReturnSuccess) return false;

    fBootCPU = fCPU[0];

    if (!attach(platform)) return false;
    registerCPUInterruptController();       // IOPlatformInterruptController, through the platform expert
    enableCPUInterrupt(fBootCPU);           // installs handleInterrupt as the kernel's platform handler
    return true;
}

void A2XCPUInterruptController::free(void)
{
    // The kernel holds this object as its interrupt handler once enabled; free only runs on a failed init.
    OSSafeReleaseNULL(fLAPICMap);
    for (UInt32 i = 0; i < kA2XMaxCPUs; i++) OSSafeReleaseNULL(fCPU[i]);
    fBootCPU = NULL;
    if (fLock) { IOLockFree(fLock); fLock = NULL; }
    IOCPUInterruptController::free();
}

// Second of the kernel's two passes (initController ran the first, start == FALSE): start each processor in CPU-number
// order (start == TRUE); the last start completes CPU initialization (ml_cpu_init_completed). Each processor's
// interrupt is enabled after its start, so IOService::cpusRunning fires once all of them are accounted for.
void A2XCPUInterruptController::startProcessors(void)
{
    if (fCPUCount <= 1) return;
    a2xClearBootTSCAdjust();
    processor_t proc;
    for (UInt32 i = 0; i < fCPUCount; i++) {
        kern_return_t kr = ml_processor_register(fCPU[i], fCPULapic[i], &proc, i == 0, TRUE);
        IOLog("ACPI: CPU%u (local APIC 0x%x) start: %s\n", i, fCPULapic[i], kr == KERN_SUCCESS ? "ok" : "refused");
        if (i) enableCPUInterrupt(fCPU[i]);
    }
}

void A2XCPUInterruptController::noteController(IOInterruptController *ctl)
{
    if (!ctl || ctl == this || fMSI) return;
    if (!ctl->metaCast("IOPCIMessagedInterruptController")) return;
    bool hb, hc;
    UInt32 base  = a2xNumberProp(ctl, "Base Vector Number", 0, &hb);
    UInt32 count = a2xNumberProp(ctl, "Vector Count", 0, &hc);
    if (!hb || !hc || !count) {
        IOLog("ACPI: PCI message controller without a vector range; MSI will not be delivered\n");
        return;
    }
    fMSIBase  = base;
    fMSICount = count;
    __atomic_store_n(&fMSI, ctl, __ATOMIC_RELEASE);
    IOLog("ACPI: PCI message controller sources 0x%x..0x%x\n", base, base + count - 1);
}

void A2XCPUInterruptController::limitMessagesToBootCPU(void)
{
    IOInterruptController *msi = fMSI;
    if (!msi || fMSIBase >= 0x100 || fMSIBase + fMSICount <= 0x100) return;
    // One source at a time: IOPCIFamily has already reserved parts of this range, and a range call fails whole.
    UInt32 taken = 0;
    for (UInt32 s = 0x100; s < fMSIBase + fMSICount; s++) {
        if (((IOPCIMessagedInterruptController *)msi)->reserveVectors(s - fMSIBase, 1)) taken++;
    }
    IOLog("ACPI: PCI message sources 0x100..0x%x withheld (boot processor only; %u newly reserved)\n",
          fMSIBase + fMSICount - 1, taken);
}

bool A2XCPUInterruptController::sendVector(UInt32 vector, UInt32 apic)
{
    if (vector < 0x20 || vector > 0xff) return false;
    bool sent = false;
    boolean_t was = ml_set_interrupts_enabled(FALSE);     // ICR high and low are one write pair on this processor
    if (fX2APIC) {
        a2x_wrmsr(kA2XMSRx2ICR, ((UInt64)apic << 32) | vector);
        sent = true;
    } else if (fLAPIC) {
        int i = 0;
        while (i < 1000 && (fLAPIC[kA2XLAPICICRLo / 4] & (1u << 12))) i++;   // previous send still pending?
        if (i < 1000) {
            fLAPIC[kA2XLAPICICRHi / 4] = (apic & 0xff) << 24;
            fLAPIC[kA2XLAPICICRLo / 4] = vector;            // fixed, physical, edge, no shorthand
            sent = true;
        }
    }
    ml_set_interrupts_enabled(was);
    return sent;
}

// ---- display interrupt kick (see gpukick_patch.py) ----
#include <kern/thread_call.h>
static A2XCPUInterruptController *gA2XKickCtl;
static thread_call_t gA2XKickCall;
static UInt32 gA2XKickVec[8], gA2XKickApic[8];
static volatile UInt32 gA2XKickN;
static volatile UInt64 gA2XKicks[8];
static UInt32 gA2XKickUS = 1000;
#include <IOKit/pci/IOPCIDevice.h>
static UInt32 gA2XKickTick;
// Thread context (the kick's own thread call): every display function with MSI enabled, its vector and destination.
static void A2XKickScan(void)
{
    UInt32 vec8[8], apic8[8], n = 0;
    OSDictionary *m = IOService::serviceMatching("IOPCIDevice");
    OSIterator *it = m ? IOService::getMatchingServices(m) : NULL;
    if (m) m->release();
    if (it) {
        IOService *o;
        while ((o = OSDynamicCast(IOService, it->getNextObject())) && n < 8) {
            IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o);
            if (!d) continue;
            OSData *cc = OSDynamicCast(OSData, d->getProperty("class-code"));
            if (!cc || cc->getLength() < 4 || ((*(const UInt32 *)cc->getBytesNoCopy() >> 16) & 0xff) != 0x03) continue;
            IOByteCount off = 0;
            if (!d->extendedFindPCICapability(kIOPCICapabilityIDMSI, &off) || !off) continue;
            UInt16 ctl = d->configRead16(off + 2);
            if (ctl == 0xffff || !(ctl & 1)) continue;                   // gone, or MSI not enabled yet
            UInt32 lo = d->configRead32(off + 4);
            UInt32 vec = d->configRead16(off + ((ctl & 0x80) ? 0xC : 0x8)) & 0xff;
            UInt32 apic = (lo >> 12) & 0xff;
            UInt32 hi = (ctl & 0x80) ? d->configRead32(off + 8) : 0;
            if ((lo & 0xfff00000) != 0xfee00000 || hi || (lo & 0xc) || !gA2XKickCtl->isBootMessageTarget(vec, apic)) {
                static UInt32 sRejLogged;
                if (sRejLogged++ < 8)
                    IOLog("ACPI: display %x:%x.%x MSI addr 0x%08x vector 0x%x NOT raised (outside the message range or not the boot processor)\n",
                          d->getBusNumber(), d->getDeviceNumber(), d->getFunctionNumber(), (unsigned)lo, vec);
                continue;
            }
            UInt32 i;
            for (i = 0; i < n && vec8[i] != vec; i++) { }
            if (i < n) continue;
            vec8[n] = vec; apic8[n] = apic; n++;
            bool known = false;
            for (UInt32 k = 0; k < gA2XKickN; k++) if (gA2XKickVec[k] == vec) known = true;
            if (!known)
                IOLog("ACPI: display %x:%x.%x MSI vector 0x%x (apic %u%s) is also raised every %u us (a2x_gpukick)\n",
                      d->getBusNumber(), d->getDeviceNumber(), d->getFunctionNumber(), vec, apic,
                      (ctl & 0x70) ? ", multi-message: base vector only" : "", gA2XKickUS);
        }
        it->release();
    }
    // Same thread as the sender (A2XKickFire2 calls this), so the table can be replaced in place.
    for (UInt32 i = 0; i < n; i++) {
        if (i >= gA2XKickN || gA2XKickVec[i] != vec8[i]) gA2XKicks[i] = 0;
        gA2XKickVec[i] = vec8[i]; gA2XKickApic[i] = apic8[i];
    }
    __atomic_store_n(&gA2XKickN, n, __ATOMIC_RELEASE);
}
static void A2XKickFire2(thread_call_param_t, thread_call_param_t)
{
    if ((gA2XKickTick++ % (1000000 / gA2XKickUS + 1)) == 0) A2XKickScan();   // about once a second
    UInt32 n = __atomic_load_n(&gA2XKickN, __ATOMIC_ACQUIRE);
    for (UInt32 i = 0; i < n; i++) if (gA2XKickCtl->sendVector(gA2XKickVec[i], gA2XKickApic[i])) gA2XKicks[i]++;
    uint64_t dl; clock_interval_to_deadline(gA2XKickUS, NSEC_PER_USEC, &dl);
    thread_call_enter_delayed(gA2XKickCall, dl);
}
// Thread context, once, after the namespace is published.
void A2XStartDisplayKick(A2XCPUInterruptController *ctl, UInt32 msiDest)
{
    if (ctl) ctl->fMSIDest = msiDest;
    UInt32 v;
    if (PE_parse_boot_argn("a2x_gpukick", &v, sizeof v)) gA2XKickUS = v;
    if (!ctl || !gA2XKickUS || gA2XKickCall) return;
    if (gA2XKickUS < 250) gA2XKickUS = 250;
    if (gA2XKickUS > 100000) gA2XKickUS = 100000;
    gA2XKickCtl = ctl;
    gA2XKickCall = thread_call_allocate(A2XKickFire2, NULL);   // the _with_priority form is not in 27 kpi.mach
    if (!gA2XKickCall) { IOLog("ACPI: display kick: no thread call\n"); return; }
    uint64_t dl; clock_interval_to_deadline(30, NSEC_PER_SEC, &dl);   // first scan 30 s in: PCI setup is over, AMD arrives ~100 s
    thread_call_enter_delayed(gA2XKickCall, dl);
    IOLog("ACPI: display kick armed, %u us\n", gA2XKickUS);
}
void A2XKickReport(void)
{
    UInt32 n = gA2XKickN;
    for (UInt32 i = 0; i < n; i++)
        IOLog("ACPI:   gpukick vector 0x%x kicks sent %llu, deliveries of this vector %u (kicks coalesce: compare runs, not a difference)\n",
              gA2XKickVec[i], gA2XKicks[i], gA2XVecCount[gA2XKickVec[i] & 0xff]);
}

// An I/O APIC driver registers on its nub, which names this controller. Its vector window comes from the nub.
IOReturn A2XCPUInterruptController::registerInterrupt(IOService *nub, int source, void *target,
                                                      IOInterruptHandler handler, void *refCon)
{
    bool hb, hl;
    UInt32 base  = a2xNumberProp(nub, "Base Vector Number", 0, &hb);
    UInt32 limit = a2xNumberProp(nub, "Vector Limit", 0, &hl);
    if (!nub || !handler || !hb || !hl || base >= limit || limit > 0x100) {
        IOLog("ACPI: refused interrupt registration from %s (base %s 0x%x, limit %s 0x%x)\n",
              nub ? nub->getName() : "?", hb ? "" : "missing", base, hl ? "" : "missing", limit);
        return kIOReturnBadArgument;
    }

    IOLockLock(fLock);
    for (UInt32 i = 0; i < fChildCount; i++) {
        if (fChild[i].nub == nub) { IOLockUnlock(fLock); return kIOReturnNoResources; }
    }
    if (fChildCount >= kA2XMaxIntChildren) { IOLockUnlock(fLock); return kIOReturnNoResources; }
    Child *c = &fChild[fChildCount];
    c->nub = nub; c->target = target; c->handler = handler; c->refCon = refCon;
    c->base = base; c->limit = limit; c->enabled = false;
    __atomic_store_n(&fChildCount, fChildCount + 1, __ATOMIC_RELEASE);
    IOLockUnlock(fLock);
    IOLog("ACPI: interrupt controller %s vectors 0x%x..0x%x\n", nub->getName(), base, limit - 1);
    return kIOReturnSuccess;
}

IOReturn A2XCPUInterruptController::unregisterInterrupt(IOService *nub, int source)
{
    IOReturn r = kIOReturnNoInterrupt;
    IOLockLock(fLock);
    for (UInt32 i = 0; i < fChildCount; i++) {
        if (fChild[i].nub == nub) { fChild[i].enabled = false; r = kIOReturnSuccess; }
    }
    IOLockUnlock(fLock);
    return r;
}

IOReturn A2XCPUInterruptController::getInterruptType(IOService *nub, int source, int *interruptType)
{
    if (!interruptType) return kIOReturnBadArgument;
    *interruptType = kIOInterruptTypeEdge;
    return kIOReturnSuccess;
}

IOReturn A2XCPUInterruptController::enableInterrupt(IOService *nub, int source)
{
    for (UInt32 i = 0; i < __atomic_load_n(&fChildCount, __ATOMIC_ACQUIRE); i++) {
        if (fChild[i].nub == nub) { fChild[i].enabled = true; return kIOReturnSuccess; }
    }
    return kIOReturnNoInterrupt;
}

IOReturn A2XCPUInterruptController::disableInterrupt(IOService *nub, int source)
{
    for (UInt32 i = 0; i < __atomic_load_n(&fChildCount, __ATOMIC_ACQUIRE); i++) {
        if (fChild[i].nub == nub) { fChild[i].enabled = false; return kIOReturnSuccess; }
    }
    return kIOReturnNoInterrupt;
}

IOReturn A2XCPUInterruptController::causeInterrupt(IOService *nub, int source)
{
    return kIOReturnUnsupported;
}

bool A2XCPUInterruptController::inService(UInt32 vector)
{
    UInt32 word = vector >> 5, bit = vector & 31;
    if (fX2APIC) return (a2x_rdmsr(kA2XMSRx2ISR + word) >> bit) & 1;
    return (fLAPIC[(kA2XLAPICISR + 0x10 * word) / 4] >> bit) & 1;
}

// Interrupt context. source = (cpu << 8) | vector, exactly as the kernel passed it.
IOReturn A2XCPUInterruptController::handleInterrupt(void *refCon, IOService *nub, int source)
{
    UInt32 raw = (UInt32)source;   // no per-interrupt crumb: see irqcrumb_patch.py
    UInt32 vector = raw & 0xff;
    bool toIOAPIC = false, claimed = false;

    if (raw < 0x100) {
        Child *best = NULL;
        UInt32 n = __atomic_load_n(&fChildCount, __ATOMIC_ACQUIRE);
        for (UInt32 i = 0; i < n; i++) {
            Child *c = &fChild[i];
            if (vector >= c->base && vector < c->limit && (!best || c->base > best->base)) best = c;
        }
        if (best) {
            toIOAPIC = true;
            if (best->enabled) { best->handler(best->target, best->refCon, best->nub, source); claimed = true; }
        }
    }
    if (!toIOAPIC) {
        IOInterruptController *msi = __atomic_load_n(&fMSI, __ATOMIC_ACQUIRE);
        if (msi && raw - fMSIBase < fMSICount) { msi->handleInterrupt(NULL, NULL, source); claimed = true; }
    }
    OSIncrementAtomic((volatile SInt32 *)&gA2XVecCount[vector]);
    if (claimed) OSIncrementAtomic((volatile SInt32 *)&gA2XVecClaimed[vector]);
    if (!claimed) fUnclaimed++;
    {   // storm tripwire: only CPU 0 runs, so plain statics are safe here
        static UInt64 sWin;
        static UInt32 sHits[256];
        UInt64 now = mach_absolute_time();
        if (now - sWin > 100000000ULL) { sWin = now; for (int i = 0; i < 256; i++) sHits[i] = 0; }
        if (__builtin_expect(++sHits[vector] > 50000, 0))
            panic("A2X interrupt storm: vector 0x%x (raw 0x%x) %u hits in 100 ms; claimed %d, ioapic %d; "
                  "vector totals %u delivered / %u claimed; unclaimed overall %u",
                  vector, raw, sHits[vector], (int)claimed, (int)toIOAPIC,
                  gA2XVecCount[vector], gA2XVecClaimed[vector], (unsigned)fUnclaimed);
    }

    // Exactly one EOI per delivered vector: the I/O APIC driver issues its own, so only a vector still in
    // service is retired here. Without a readable in-service register, follow the contract by origin.
    if (fX2APIC || fLAPIC) { if (inService(vector)) lapic_end_of_interrupt(); }
    else if (!(toIOAPIC && claimed)) lapic_end_of_interrupt();
    return kIOReturnSuccess;
}
