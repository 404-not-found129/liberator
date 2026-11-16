// SPDX-License-Identifier: BSD-3-Clause
//
// buttons.cpp - ACPI power and sleep buttons for the ACPI platform (macOS 27, x86_64).
//
// Two kinds of button reach power management through here:
//   - the fixed-hardware power button: the "power-button" nub the platform expert publishes, whose interrupt 0
//     is the ACPI fixed power-button event (delivered from the SCI, interrupt context);
//   - control-method buttons: PNP0C0C (power) and PNP0C0E (sleep) namespace nubs, which receive Notify(0x80)
//     as kIOACPIMessageDeviceNotification (thread context).
// Either way the press is handed to IOPMrootDomain from a thread call, never from interrupt context.

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <kern/thread_call.h>

class A2XACPIButton : public IOService
{
    OSDeclareDefaultStructors(A2XACPIButton)

    IOService     *fNub;
    thread_call_t  fCall;
    UInt32         fMessage;      // kIOPMPowerButton or kIOPMSleepNow
    bool           fFixed;

    static void interruptEntry(OSObject *target, void *refCon, IOService *nub, int source);
    static void pressed(thread_call_param_t self, thread_call_param_t);

public:
    virtual bool     start(IOService *provider) override;
    virtual void     stop(IOService *provider) override;
    virtual void     free(void) override;
    virtual IOReturn message(UInt32 type, IOService *provider, void *argument) override;
};

OSDefineMetaClassAndStructors(A2XACPIButton, IOService)

bool A2XACPIButton::start(IOService *provider)
{
    if (!IOService::start(provider)) return false;
    fNub = provider;
    fFixed = !strcmp(provider->getName(), "power-button");     // the platform expert's fixed-event nub
    OSString *matched = OSDynamicCast(OSString, provider->getProperty(gIONameMatchedKey));
    fMessage = (matched && matched->isEqualTo("PNP0C0E")) ? kIOPMSleepNow : kIOPMPowerButton;

    fCall = thread_call_allocate(pressed, this);
    if (!fCall) return false;

    if (fFixed) {
        IOReturn r = provider->registerInterrupt(0, this, interruptEntry, NULL);
        if (r == kIOReturnSuccess) r = provider->enableInterrupt(0);
        if (r != kIOReturnSuccess) {
            IOLog("ACPI: power button event not installed: 0x%x\n", r);
            thread_call_free(fCall); fCall = NULL;
            return false;
        }
    }
    IOLog("ACPI: %s button on %s (%s)\n", fMessage == kIOPMSleepNow ? "sleep" : "power", provider->getName(),
          fFixed ? "fixed event" : "notify");
    registerService();
    return true;
}

void A2XACPIButton::stop(IOService *provider)
{
    if (fFixed && fNub) { fNub->disableInterrupt(0); fNub->unregisterInterrupt(0); }
    if (fCall) thread_call_cancel_wait(fCall);
    IOService::stop(provider);
}

void A2XACPIButton::free(void)
{
    if (fCall) { thread_call_free(fCall); fCall = NULL; }
    IOService::free();
}

// SCI context: only queue the work.
void A2XACPIButton::interruptEntry(OSObject *target, void *, IOService *, int)
{
    A2XACPIButton *b = OSDynamicCast(A2XACPIButton, target);
    if (b && b->fCall) thread_call_enter(b->fCall);
}

void A2XACPIButton::pressed(thread_call_param_t self, thread_call_param_t)
{
    A2XACPIButton *b = (A2XACPIButton *)self;
    IOPMrootDomain *root = getPMRootDomain();
    if (root) root->receivePowerNotification(b->fMessage);
}

// Notify(0x80) is "button pressed" for both PNP0C0C and PNP0C0E (ACPI 6.5 s4.8.3.1, s5.6.6).
IOReturn A2XACPIButton::message(UInt32 type, IOService *provider, void *argument)
{
    if (type == kIOACPIMessageDeviceNotification && argument && *(UInt32 *)argument == 0x80 && fCall) {
        thread_call_enter(fCall);
        return kIOReturnSuccess;
    }
    return IOService::message(type, provider, argument);
}
