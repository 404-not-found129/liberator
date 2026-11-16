// SPDX-License-Identifier: BSD-3-Clause
//
// events.cpp - ACPI event controller (see events.h).
//
// Context rules this file is built around:
//   * The SCI is serviced on this controller's work loop (thread context): IOInterruptEventSource masks the
//     level-triggered SCI at primary level and the action calls ACPICA's SCI handler.
//   * ACPICA calls a GPE handler with its GPE spin lock held. dispatchGPE therefore only signals the driver; the
//     GPE stays masked (ACPICA disabled it for dispatch) and is finished from enableInterrupt, in thread context.
//   * fLock is only ever taken inside ACPICA's GPE lock, never around an ACPICA call, so the order cannot invert.

#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/thread.h>
#include "events.h"

extern "C" UINT32 AcpiXnuDispatchSci(void);

OSDefineMetaClassAndStructors(A2XACPIEventController, IOInterruptController)

// The slot does not carry its controller; one controller exists per platform.
static A2XACPIEventController *gA2XEvents;

static UINT32 A2XGpeEntry(ACPI_HANDLE, UINT32, void *ctx)
{
    return gA2XEvents ? gA2XEvents->dispatchGPE((A2XACPIEventController::Slot *)ctx) : 0;
}

static UINT32 A2XFixedEntry(void *ctx)
{
    return gA2XEvents ? gA2XEvents->dispatchFixed((A2XACPIEventController::Slot *)ctx) : ACPI_INTERRUPT_NOT_HANDLED;
}

bool A2XACPIEventController::initController(void)
{
    if (!IOInterruptController::init((OSDictionary *)NULL)) return false;
    bzero(fSlots, sizeof fSlots);
    fNextBlockVector = kA2XGpeBlockVectorBase;
    fLock = IOSimpleLockAlloc();
    fAllocLock = IOLockAlloc();
    fWorkLoop = NULL;
    fSCISource = NULL;
    if (!fLock || !fAllocLock) return false;
    OSString *n = OSString::withCString(kA2XEventControllerName);
    if (n) { setProperty("InterruptControllerName", n); n->release(); }
    gA2XEvents = this;
    return true;
}

void A2XACPIEventController::free(void)
{
    if (gA2XEvents == this) gA2XEvents = NULL;
    if (fSCISource) {
        fSCISource->disable();
        if (fWorkLoop) fWorkLoop->removeEventSource(fSCISource);
        OSSafeReleaseNULL(fSCISource);
    }
    OSSafeReleaseNULL(fWorkLoop);
    if (fLock) { IOSimpleLockFree(fLock); fLock = NULL; }
    if (fAllocLock) { IOLockFree(fAllocLock); fAllocLock = NULL; }
    IOInterruptController::free();
}

// ---------------------------------------------------------------- vectors

A2XACPIEventController::Slot *A2XACPIEventController::slotForVector(UInt32 vector)
{
    for (UInt32 i = 0; i < kA2XEventSlots; i++)
        if (fSlots[i].used && fSlots[i].vector == vector) return &fSlots[i];
    return NULL;
}

SInt64 A2XACPIEventController::vectorForFixedEvent(UInt32 fixedEvent)
{
    if (fixedEvent > ACPI_EVENT_MAX) return -1;
    SInt64 v = -1;
    IOLockLock(fAllocLock);
    Slot *s = slotForVector(fixedEvent);
    for (UInt32 i = 0; !s && i < kA2XEventSlots; i++) {
        if (!fSlots[i].used) {
            s = &fSlots[i];
            bzero(s, sizeof *s);
            s->fixed = true; s->vector = fixedEvent; s->gpe = fixedEvent;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            s->used = true;             // published last: readers scan without a lock
        }
    }
    if (s) v = s->vector;
    IOLockUnlock(fAllocLock);
    return v;
}

SInt64 A2XACPIEventController::vectorForGPE(ACPI_HANDLE gpeDevice, UInt32 gpe)
{
    if (!gpeDevice && gpe >= kA2XGpeVectorBase) return -1;
    SInt64 v = -1;
    IOLockLock(fAllocLock);
    Slot *s = NULL;
    for (UInt32 i = 0; i < kA2XEventSlots; i++)
        if (fSlots[i].used && !fSlots[i].fixed && fSlots[i].gpeDevice == gpeDevice && fSlots[i].gpe == gpe) { s = &fSlots[i]; break; }
    for (UInt32 i = 0; !s && i < kA2XEventSlots; i++) {
        if (!fSlots[i].used) {
            s = &fSlots[i];
            bzero(s, sizeof *s);
            s->gpeDevice = gpeDevice; s->gpe = gpe;
            s->vector = gpeDevice ? fNextBlockVector++ : kA2XGpeVectorBase + gpe;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            s->used = true;
        }
    }
    if (s) v = s->vector;
    IOLockUnlock(fAllocLock);
    return v;
}

// After registration: by (nub, source), no locks and no registry access. enable/disable can run inside a GPE
// handler, under ACPICA's GPE spin lock with interrupts off, where taking a mutex is not allowed.
A2XACPIEventController::Slot *A2XACPIEventController::slotForSource(IOService *nub, int source)
{
    for (UInt32 i = 0; i < kA2XEventSlots; i++) {
        Slot *s = &fSlots[i];
        if (s->used && s->registered && s->nub == nub && s->source == source) return s;
    }
    return NULL;
}

// Before registration (thread context): the vector is the first word of the nub's specifier at this index.
A2XACPIEventController::Slot *A2XACPIEventController::slotFromSpecifier(IOService *nub, int source)
{
    if (!nub || source < 0) return NULL;
    OSArray *specs = OSDynamicCast(OSArray, nub->getProperty(gIOInterruptSpecifiersKey));
    OSData *d = specs ? OSDynamicCast(OSData, specs->getObject((unsigned)source)) : NULL;
    if (!d || d->getLength() < sizeof(UInt32)) return NULL;
    UInt32 vector = *(const UInt32 *)d->getBytesNoCopy();
    IOLockLock(fAllocLock);
    Slot *s = slotForVector(vector);
    IOLockUnlock(fAllocLock);
    return s;
}

// ---------------------------------------------------------------- IOInterruptController

IOReturn A2XACPIEventController::registerInterrupt(IOService *nub, int source, void *target,
                                                   IOInterruptHandler handler, void *refCon)
{
    Slot *s = slotFromSpecifier(nub, source);
    if (!s || !handler) return kIOReturnNoInterrupt;
    if (s->registered) return kIOReturnNoResources;     // ACPI events are not shared

    s->nub = nub; s->source = source; s->target = target; s->handler = handler; s->refCon = refCon;
    s->armed = false; s->held = false; s->inFlight = false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);      // fields visible before ACPICA can dispatch to the slot

    ACPI_STATUS st;
    if (s->fixed) {
        st = AcpiInstallFixedEventHandler(s->gpe, A2XFixedEntry, s);
        if (ACPI_SUCCESS(st)) (void)AcpiDisableEvent(s->gpe, 0);   // registered interrupts start disabled
    } else {
        // Trigger type: an _Exx method marks an edge GPE in the FADT blocks; everything else is treated as level,
        // which is also safe for edge sources (status is latched while masked).
        UINT32 type = ACPI_GPE_LEVEL_TRIGGERED;
        if (!s->gpeDevice && s->gpe <= 0xff) {
            char path[16];
            ACPI_HANDLE m;
            snprintf(path, sizeof path, "\\_GPE._E%02X", s->gpe);
            if (ACPI_SUCCESS(AcpiGetHandle(NULL, path, &m))) type = ACPI_GPE_EDGE_TRIGGERED;
        }
        st = AcpiInstallGpeHandler(s->gpeDevice, s->gpe, type, A2XGpeEntry, s);
    }
    if (ACPI_FAILURE(st)) {
        IOLog("ACPI: %s %u: install handler for %s: %s\n", s->fixed ? "fixed event" : "GPE", s->gpe,
              nub->getName(), AcpiFormatException(st));
        s->handler = NULL; s->nub = NULL;
        return kIOReturnError;
    }
    s->registered = true;
    return kIOReturnSuccess;
}

IOReturn A2XACPIEventController::unregisterInterrupt(IOService *nub, int source)
{
    Slot *s = slotForSource(nub, source);
    if (!s || !s->registered || s->nub != nub) return kIOReturnNoInterrupt;
    (void)disableInterrupt(nub, source);
    if (s->fixed) {
        (void)AcpiRemoveFixedEventHandler(s->gpe, A2XFixedEntry);
    } else {
        (void)AcpiRemoveGpeHandler(s->gpeDevice, s->gpe, A2XGpeEntry);
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
        bool drop = s->held;
        s->held = false; s->inFlight = false;
        IOSimpleLockUnlockEnableInterrupt(fLock, is);
        if (drop) (void)AcpiDisableGpe(s->gpeDevice, s->gpe);
    }
    s->registered = false;
    s->handler = NULL; s->nub = NULL; s->target = NULL; s->refCon = NULL;
    return kIOReturnSuccess;
}

IOReturn A2XACPIEventController::getInterruptType(IOService *nub, int source, int *interruptType)
{
    Slot *s = slotForSource(nub, source);
    if (!s) s = slotFromSpecifier(nub, source);      // event sources ask before registering
    if (!s || !interruptType) return kIOReturnNoInterrupt;
    // GPEs are masked by ACPICA until finished, which is the level contract: the event source disables and
    // re-enables around its action, and the re-enable finishes the GPE.
    *interruptType = s->fixed ? kIOInterruptTypeEdge : kIOInterruptTypeLevel;
    return kIOReturnSuccess;
}

IOReturn A2XACPIEventController::enableInterrupt(IOService *nub, int source)
{
    Slot *s = slotForSource(nub, source);
    if (!s || !s->registered) return kIOReturnNoInterrupt;
    if (s->fixed) {
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
        s->armed = true;
        IOSimpleLockUnlockEnableInterrupt(fLock, is);
        return ACPI_SUCCESS(AcpiEnableEvent(s->gpe, 0)) ? kIOReturnSuccess : kIOReturnError;
    }
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
    s->armed = true;
    bool take = !s->held;
    bool finish = s->inFlight;
    s->held = true;
    s->inFlight = false;
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    if (take && ACPI_FAILURE(AcpiEnableGpe(s->gpeDevice, s->gpe))) {
        is = IOSimpleLockLockDisableInterrupt(fLock);
        s->held = false;
        IOSimpleLockUnlockEnableInterrupt(fLock, is);
        return kIOReturnError;
    }
    if (finish) (void)AcpiFinishGpe(s->gpeDevice, s->gpe);
    return kIOReturnSuccess;
}

IOReturn A2XACPIEventController::disableInterrupt(IOService *nub, int source)
{
    Slot *s = slotForSource(nub, source);
    if (!s || !s->registered) return kIOReturnNoInterrupt;
    if (s->fixed) {
        IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
        s->armed = false;
        IOSimpleLockUnlockEnableInterrupt(fLock, is);
        (void)AcpiDisableEvent(s->gpe, 0);
        return kIOReturnSuccess;
    }
    // While a GPE is in flight it is already masked by ACPICA and this may be running under ACPICA's GPE lock
    // (an event source disabling from inside its handler), so only the flag changes.
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
    s->armed = false;
    bool drop = s->held && !s->inFlight;
    if (drop) s->held = false;
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    if (drop) (void)AcpiDisableGpe(s->gpeDevice, s->gpe);
    return kIOReturnSuccess;
}

IOReturn A2XACPIEventController::causeInterrupt(IOService *, int)
{
    return kIOReturnUnsupported;
}

IOReturn A2XACPIEventController::handleInterrupt(void *, IOService *, int)
{
    return kIOReturnUnsupported;    // events arrive through ACPICA, never as a hardware vector here
}

// ---------------------------------------------------------------- dispatch

UInt32 A2XACPIEventController::dispatchGPE(Slot *s)
{
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
    s->inFlight = true;
    bool deliver = s->registered && s->armed && s->handler;
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    if (!deliver) return 0;                     // stays masked until a driver enables it

    s->handler(s->target, s->refCon, s->nub, s->source);

    // A handler that left the interrupt enabled (a direct handler) is done: finish now. One that disabled it
    // (an event source) finishes when it re-enables.
    is = IOSimpleLockLockDisableInterrupt(fLock);
    bool done = s->armed;
    if (done) s->inFlight = false;
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    return done ? ACPI_REENABLE_GPE : 0;
}

UInt32 A2XACPIEventController::dispatchFixed(Slot *s)
{
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(fLock);
    bool deliver = s->registered && s->armed && s->handler;
    IOSimpleLockUnlockEnableInterrupt(fLock, is);
    if (deliver) s->handler(s->target, s->refCon, s->nub, s->source);
    return ACPI_INTERRUPT_HANDLED;
}

// ---------------------------------------------------------------- SCI

void A2XACPIEventController::sciAction(OSObject *, IOInterruptEventSource *, int)
{
    (void)AcpiXnuDispatchSci();
}

static void A2XSciThreadEntry(void *param, wait_result_t)
{
    A2XACPIEventController *ec = (A2XACPIEventController *)param;
    ec->sciThread();
    ec->release();
}

bool A2XACPIEventController::startSCI(UInt32 pin, UInt32 flags, const OSSymbol *controllerName)
{
    if (!controllerName) return false;
    UInt32 spec[2] = { pin, flags };
    OSData *d = OSData::withBytes(spec, sizeof spec);
    OSArray *specs = d ? OSArray::withObjects((const OSObject **)&d, 1) : NULL;
    OSArray *ctrls = OSArray::withObjects((const OSObject **)&controllerName, 1);
    bool ok = specs && ctrls;
    if (ok) {
        setProperty(gIOInterruptSpecifiersKey, specs);
        setProperty(gIOInterruptControllersKey, ctrls);
    }
    OSSafeReleaseNULL(d); OSSafeReleaseNULL(specs); OSSafeReleaseNULL(ctrls);
    if (!ok) return false;

    thread_t t = NULL;
    retain();
    if (kernel_thread_start(A2XSciThreadEntry, this, &t) != KERN_SUCCESS) { release(); return false; }
    thread_deallocate(t);
    return true;
}

void A2XACPIEventController::sciThread(void)
{
    fWorkLoop = IOWorkLoop::workLoop();
    if (!fWorkLoop) { IOLog("ACPI: SCI: no work loop\n"); return; }
    // Resolving index 0 waits here until the I/O APIC controller named in the specifier is registered.
    fSCISource = IOInterruptEventSource::interruptEventSource(this, sciAction, this, 0);
    if (!fSCISource || fWorkLoop->addEventSource(fSCISource) != kIOReturnSuccess) {
        IOLog("ACPI: SCI: cannot attach the interrupt source\n");
        OSSafeReleaseNULL(fSCISource);
        return;
    }
    fSCISource->enable();
    // Runtime GPEs with _Lxx/_Exx methods are armed only once the SCI can deliver them.
    ACPI_STATUS s = AcpiUpdateAllGpes();
    if (ACPI_FAILURE(s)) IOLog("ACPI: update GPEs: %s\n", AcpiFormatException(s));
    IOLog("ACPI: SCI connected\n");
}
