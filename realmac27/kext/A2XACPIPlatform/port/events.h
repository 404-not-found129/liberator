// SPDX-License-Identifier: BSD-3-Clause
//
// events.h - ACPI event controller: the SCI, fixed events and GPEs presented as an IOInterruptController named
// "ACPIEventController". A driver gets a vector on its nub from IOACPIPlatformDevice::installInterruptForGPE() /
// installInterruptForFixedEvent() and then uses the ordinary registerInterrupt/enableInterrupt calls on that index.

#ifndef A2X_ACPI_EVENTS_H
#define A2X_ACPI_EVENTS_H

#include <IOKit/IOInterruptController.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOLocks.h>

extern "C" {
#include "acpi.h"
}

#define kA2XEventControllerName "ACPIEventController"

// Vector numbers on the event controller (first word of the nub's interrupt specifier).
//   0 .. 4                 fixed events, by ACPI fixed-event number (RTC = 4, power button = 2)
//   0x10000 + n            GPE n of the FADT GPE blocks
//   0x20000 + slot         a GPE of a GPE block device, numbered on first use
enum : UInt32 {
    kA2XGpeVectorBase      = 0x10000,
    kA2XGpeBlockVectorBase = 0x20000,
    kA2XEventSlots         = 64,
};

class A2XACPIEventController : public IOInterruptController
{
    OSDeclareDefaultStructors(A2XACPIEventController)

public:
    struct Slot {
        UInt32             vector;
        bool               used;
        bool               fixed;          // fixed event (else GPE)
        ACPI_HANDLE        gpeDevice;      // NULL = FADT blocks
        UInt32             gpe;            // GPE number, or fixed-event number
        // registration
        bool               registered;
        IOService         *nub;
        int                source;
        void              *target;
        IOInterruptHandler handler;
        void              *refCon;
        // state, guarded by fLock; ACPICA is never called with fLock held
        bool               armed;          // driver has the interrupt enabled
        bool               held;           // we hold an ACPICA runtime reference on the GPE
        bool               inFlight;       // ACPICA dispatched it; the GPE stays masked until finished
    };

    bool             initController(void);
    virtual void     free(void) APPLE_KEXT_OVERRIDE;

    // Vector for a fixed event or a GPE; allocates the slot on first use. -1 when out of slots or invalid.
    SInt64           vectorForFixedEvent(UInt32 fixedEvent);
    SInt64           vectorForGPE(ACPI_HANDLE gpeDevice, UInt32 gpe);

    // Connects the SCI (GSI + specifier flags on the named controller). Runs on its own kernel thread, since
    // resolving the I/O APIC controller waits until that driver has registered it.
    bool             startSCI(UInt32 pin, UInt32 flags, const OSSymbol *controllerName);

    // IOInterruptController
    virtual IOReturn registerInterrupt(IOService *nub, int source, void *target, IOInterruptHandler handler,
                                       void *refCon) APPLE_KEXT_OVERRIDE;
    virtual IOReturn unregisterInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getInterruptType(IOService *nub, int source, int *interruptType) APPLE_KEXT_OVERRIDE;
    virtual IOReturn enableInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn disableInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn causeInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn handleInterrupt(void *refCon, IOService *nub, int source) APPLE_KEXT_OVERRIDE;

    // ACPICA callbacks (thunks pass the slot)
    UInt32           dispatchGPE(Slot *s);
    UInt32           dispatchFixed(Slot *s);
    void             sciThread(void);

private:
    Slot             fSlots[kA2XEventSlots];
    UInt32           fNextBlockVector;
    IOSimpleLock    *fLock;
    IOLock          *fAllocLock;
    IOWorkLoop      *fWorkLoop;
    IOInterruptEventSource *fSCISource;

    Slot            *slotForVector(UInt32 vector);
    Slot            *slotForSource(IOService *nub, int source);       // registered; lock-free, any context
    Slot            *slotFromSpecifier(IOService *nub, int source);   // thread context only
    static void      sciAction(OSObject *owner, IOInterruptEventSource *sender, int count);
};

#endif
