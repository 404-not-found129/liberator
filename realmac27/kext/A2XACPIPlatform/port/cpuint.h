// SPDX-License-Identifier: BSD-3-Clause
//
// cpuint.h - the platform (CPU) interrupt controller, registered as "IOPlatformInterruptController".
//
// The kernel delivers every external vector it does not handle itself to the one platform handler installed by
// IOCPUInterruptController::enableCPUInterrupt, as source = (cpu << 8) | vector for cpu <= lapic_max_interrupt_cpunum.
// This controller passes that raw source on unchanged:
//   - cpu 0, vector in an I/O APIC's [Base Vector Number, Vector Limit)  -> that I/O APIC driver's handler
//   - source in the PCI message controller's [base, base + count)       -> IOPCIMessagedInterruptController
// and afterwards issues the local APIC EOI if the vector is still in service (the I/O APIC driver issues its own;
// the message controller never does).

#ifndef A2X_ACPI_CPUINT_H
#define A2X_ACPI_CPUINT_H

#include <IOKit/IOService.h>
#include <IOKit/IOInterruptController.h>
#include <IOKit/IOMemoryDescriptor.h>

extern "C" {
#include <kern/processor.h>
}

// IOCPU and IOCPUInterruptController are com.apple.kpi.private and not in the public headers. These declarations
// reproduce the kernel's vtable order and field layout; build-kext.sh checks the two slots the kernel calls
// (getCPUNumber at 0x8a0 of IOCPU, handleInterrupt at 0x888 of IOCPUInterruptController).
typedef void (*a2x_ipi_handler_t)(void);

class IOCPU : public IOService
{
    OSDeclareAbstractStructors(IOCPU)
private:
    OSArray  *_cpuGroup;
    UInt32    _cpuNumber;
    UInt32    _cpuState;
protected:
    IOService        *cpuNub;
    processor_t       machProcessor;
    a2x_ipi_handler_t ipi_handler;
    struct ExpansionData { };
    ExpansionData    *iocpu_reserved;

    virtual void setCPUNumber(UInt32 cpuNumber);
    virtual void setCPUState(UInt32 cpuState);
public:
    virtual bool      start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void      detach(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual OSObject *getProperty(const OSSymbol *aKey) const APPLE_KEXT_OVERRIDE;
    virtual bool      setProperty(const OSSymbol *aKey, OSObject *anObject) APPLE_KEXT_OVERRIDE;
    virtual bool      serializeProperties(OSSerialize *serialize) const APPLE_KEXT_OVERRIDE;
    virtual IOReturn  setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE;
    virtual void      initCPU(bool boot) = 0;
    virtual void      quiesceCPU(void) = 0;
    virtual kern_return_t startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr) = 0;
    virtual void      haltCPU(void) = 0;
    virtual void      signalCPU(IOCPU *target);
    virtual void      signalCPUDeferred(IOCPU *target);
    virtual void      signalCPUCancel(IOCPU *target);
    virtual void      enableCPUTimeBase(bool enable);
    virtual UInt32    getCPUNumber(void);
    virtual UInt32    getCPUState(void);
    virtual OSArray  *getCPUGroup(void);
    virtual UInt32    getCPUGroupSize(void);
    virtual processor_t getMachProcessor(void);
    virtual const OSSymbol *getCPUName(void) = 0;

    OSMetaClassDeclareReservedUnused(IOCPU, 0);
    OSMetaClassDeclareReservedUnused(IOCPU, 1);
    OSMetaClassDeclareReservedUnused(IOCPU, 2);
    OSMetaClassDeclareReservedUnused(IOCPU, 3);
    OSMetaClassDeclareReservedUnused(IOCPU, 4);
    OSMetaClassDeclareReservedUnused(IOCPU, 5);
    OSMetaClassDeclareReservedUnused(IOCPU, 6);
    OSMetaClassDeclareReservedUnused(IOCPU, 7);
};

class IOCPUInterruptController : public IOInterruptController
{
    OSDeclareDefaultStructors(IOCPUInterruptController)
private:
    int enabledCPUs;
protected:
    int numCPUs;
    int numSources;
    struct ExpansionData { };
    ExpansionData *iocpuic_reserved;
public:
    virtual IOReturn initCPUInterruptController(int sources);
    virtual void     registerCPUInterruptController(void);
    virtual void     enableCPUInterrupt(IOCPU *cpu);
    virtual void     setCPUInterruptProperties(IOService *service) APPLE_KEXT_OVERRIDE;
    virtual IOReturn registerInterrupt(IOService *nub, int source, void *target, IOInterruptHandler handler,
                                       void *refCon) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getInterruptType(IOService *nub, int source, int *interruptType) APPLE_KEXT_OVERRIDE;
    virtual IOReturn enableInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn disableInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn causeInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn handleInterrupt(void *refCon, IOService *nub, int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn initCPUInterruptController(int sources, int cpus);

    OSMetaClassDeclareReservedUnused(IOCPUInterruptController, 1);
    OSMetaClassDeclareReservedUnused(IOCPUInterruptController, 2);
    OSMetaClassDeclareReservedUnused(IOCPUInterruptController, 3);
    OSMetaClassDeclareReservedUnused(IOCPUInterruptController, 4);
    OSMetaClassDeclareReservedUnused(IOCPUInterruptController, 5);
};

// One processor as the kernel sees it: the cpu_id handed to ml_processor_register, which the kernel passes back
// to PE_cpu_machine_init on that processor. The kernel starts application processors itself (INIT/SIPI from the
// local APIC id), so startCPU is never the start path on x86.
class A2XBootCPU : public IOCPU
{
    OSDeclareDefaultStructors(A2XBootCPU)
public:
    bool              initNumber(UInt32 n);
    virtual void      initCPU(bool boot) APPLE_KEXT_OVERRIDE;
    virtual void      quiesceCPU(void) APPLE_KEXT_OVERRIDE;
    virtual kern_return_t startCPU(vm_offset_t start_paddr, vm_offset_t arg_paddr) APPLE_KEXT_OVERRIDE;
    virtual void      haltCPU(void) APPLE_KEXT_OVERRIDE;
    virtual const OSSymbol *getCPUName(void) APPLE_KEXT_OVERRIDE;
};

// The class name the Apple 27 platform plug-ins name as their provider (IOProviderClass AppleACPICPU, matched with
// IOPropertyMatch IOCPUNumber 0, IOParentMatch IOACPIPlatformDevice). The 27 system does not ship the class, so it is ours.
class AppleACPICPU : public A2XBootCPU
{
    OSDeclareDefaultStructors(AppleACPICPU)
public:
    // Attaches to the processor nub in the service plane, runs IOCPU::start (gIOCPUs, frequencies on the nub),
    // restores the CPU number IOCPU::start resets to 0, marks it Running and registers it for matching.
    bool              publishOn(IOService *procNub);
};

#define kA2XMaxIntChildren 8
#define kA2XMaxCPUs        64

class A2XCPUInterruptController : public IOCPUInterruptController
{
    OSDeclareDefaultStructors(A2XCPUInterruptController)

public:
    // Creates the controller, registers it as IOPlatformInterruptController and installs it as the kernel's
    // platform interrupt handler. Must run before any I/O APIC nub or PCI bridge is published.
    // lapicIDs: every enabled processor in the MADT. The count of processors that will be started is fixed here,
    // because it is also the kernel's maximum CPU count (ml_set_max_cpus).
    bool             initController(IOService *platform, const UInt32 *lapicIDs, UInt32 count);

    // Registers every processor with the kernel, then starts them in CPU-number order (boot processor first,
    // the rest by ascending local APIC id, which is the order the kernel's topology sort assigns numbers in).
    void             startProcessors(void);

    // Gives each registered processor its registry presence (AppleACPICPU on the nub whose processor-index matches).
    // Call after startProcessors and after the namespace is published; nubs[i] is the nub for CPU number i or NULL.
    void             publishProcessors(IOService * const *nubs, UInt32 n);
    virtual void     free(void) APPLE_KEXT_OVERRIDE;

    // Called by the platform expert for every controller it registers; picks up the PCI message controller.
    void             noteController(IOInterruptController *ctl);

    IOInterruptController *messageController(void) const { return fMSI; }

    // Only the boot processor takes interrupts: takes every processor-1 message source out of the PCI message
    // controller's pool, so no device is given a vector on a processor that is not running.
    void             limitMessagesToBootCPU(void);

    // Raises vector on the processor with local APIC id apic, as a fixed interrupt (thread context).
    bool             sendVector(UInt32 vector, UInt32 apic);   // false = ICR busy, nothing sent
    // A vector the PCI message controller owns, aimed at the boot processor.
    bool             isBootMessageTarget(UInt32 vector, UInt32 apic) const
    { return fMSI && vector < 0x100 && vector - fMSIBase < fMSICount && apic == fMSIDest; }
    UInt32           fMSIDest;    // the APIC id GetMessagedInterruptAddress aims every MSI at (set at kick arm)

    virtual IOReturn registerInterrupt(IOService *nub, int source, void *target, IOInterruptHandler handler,
                                       void *refCon) APPLE_KEXT_OVERRIDE;
    virtual IOReturn unregisterInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn getInterruptType(IOService *nub, int source, int *interruptType) APPLE_KEXT_OVERRIDE;
    virtual IOReturn enableInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn disableInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn causeInterrupt(IOService *nub, int source) APPLE_KEXT_OVERRIDE;
    virtual IOReturn handleInterrupt(void *refCon, IOService *nub, int source) APPLE_KEXT_OVERRIDE;

private:
    struct Child {
        IOService         *nub;
        void              *target;
        IOInterruptHandler handler;
        void              *refCon;
        UInt32             base;     // first vector
        UInt32             limit;    // one past the last vector
        volatile bool      enabled;
    };
    Child                  fChild[kA2XMaxIntChildren];
    volatile UInt32        fChildCount;      // entries [0, count) are complete; written under fLock
    IOLock                *fLock;
    IOInterruptController * volatile fMSI;  // the PCI message controller, once registered
    UInt32                 fMSIBase;
    UInt32                 fMSICount;
    A2XBootCPU            *fBootCPU;        // fCPU[0]
    A2XBootCPU            *fCPU[kA2XMaxCPUs];
    UInt32                 fCPULapic[kA2XMaxCPUs];
    UInt32                 fCPUCount;
    bool                   fX2APIC;
    IOMemoryMap           *fLAPICMap;
    volatile UInt32       *fLAPIC;
    UInt64                 fUnclaimed;

    bool             inService(UInt32 vector);
};

#endif
