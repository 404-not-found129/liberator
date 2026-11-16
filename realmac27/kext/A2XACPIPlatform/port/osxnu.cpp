// SPDX-License-Identifier: BSD-3-Clause
//
// osxnu.cpp - ACPICA operating-system services layer for the macOS x86_64 kernel.
//
// Every service maps onto a public Kernel.framework interface (IOLib, IOLocks, IOMemoryDescriptor,
// thread_call, kern/clock, architecture/i386/pio). Nothing here touches private kernel state.
//
// Threading contract: ACPICA's interpreter, GPE dispatch and table code run in thread context. The SCI
// is not serviced at primary interrupt level; the platform expert receives it on its work loop and calls
// AcpiXnuDispatchSci(), so mapping and mutex services below may block. Only the spin lock services are
// interrupt-safe, which is all ACPICA requires of them.

#include <IOKit/IOLib.h>
#include "crumb.h"
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IORegistryEntry.h>
#include <libkern/c++/OSData.h>
#include <kern/thread_call.h>
#include <kern/clock.h>
#include <architecture/i386/pio.h>

extern "C" {
#include "acpi.h"
}

// ---------------------------------------------------------------- state

static IOLock       *gMapLock;      // guards gMaps
static IOLock       *gExecLock;     // guards gExecPending
static IOSimpleLock *gPciLock;      // serialises the 0xCF8/0xCFC address/data pair
static UInt32        gExecPending;

static ACPI_OSD_HANDLER gSciHandler;
static void            *gSciContext;
static UINT32           gSciIrq;

struct AcpiXnuMap {
    AcpiXnuMap          *next;
    void                *ret;       // pointer handed to ACPICA (mapping base + page offset)
    IOMemoryMap         *map;
    IOMemoryDescriptor  *md;
};
static AcpiXnuMap *gMaps;

// ---------------------------------------------------------------- init

ACPI_STATUS AcpiOsInitialize(void)
{
    if (!gMapLock)  gMapLock  = IOLockAlloc();
    if (!gExecLock) gExecLock = IOLockAlloc();
    if (!gPciLock)  gPciLock  = IOSimpleLockAlloc();
    return (gMapLock && gExecLock && gPciLock) ? AE_OK : AE_NO_MEMORY;
}

ACPI_STATUS AcpiOsTerminate(void)
{
    return AE_OK;   // locks are kept: the platform expert lives for the life of the kernel
}

// ---------------------------------------------------------------- tables

// The firmware loader publishes each EFI configuration table under IODeviceTree:/efi/configuration-table/<GUID>
// with its physical address in "table". ACPI 2.0 first, then ACPI 1.0, then the legacy BIOS-area scan.
static ACPI_PHYSICAL_ADDRESS AcpiXnuEfiTable(const char *path)
{
    ACPI_PHYSICAL_ADDRESS pa = 0;
    IORegistryEntry *e = IORegistryEntry::fromPath(path);
    if (!e) return 0;
    OSData *d = OSDynamicCast(OSData, e->getProperty("table"));
    if (d && d->getLength() == sizeof(UInt64)) {
        UInt64 v; memcpy(&v, d->getBytesNoCopy(), sizeof v); pa = (ACPI_PHYSICAL_ADDRESS)v;
    } else if (d && d->getLength() == sizeof(UInt32)) {
        UInt32 v; memcpy(&v, d->getBytesNoCopy(), sizeof v); pa = (ACPI_PHYSICAL_ADDRESS)v;
    }
    e->release();
    return pa;
}

ACPI_PHYSICAL_ADDRESS AcpiOsGetRootPointer(void)
{
    ACPI_PHYSICAL_ADDRESS pa;
    pa = AcpiXnuEfiTable("IODeviceTree:/efi/configuration-table/8868E871-E4F1-11D3-BC22-0080C73C8881");
    if (pa) return pa;
    pa = AcpiXnuEfiTable("IODeviceTree:/efi/configuration-table/EB9D2D30-2D88-11D3-9A16-0090273FC14D");
    if (pa) return pa;
    if (ACPI_SUCCESS(AcpiFindRootPointer(&pa))) return pa;
    IOLog("ACPI: no RSDP in the EFI configuration table or the BIOS area\n");
    return 0;
}

ACPI_STATUS AcpiOsPredefinedOverride(const ACPI_PREDEFINED_NAMES *, ACPI_STRING *NewValue)
{
    if (!NewValue) return AE_BAD_PARAMETER;
    *NewValue = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsTableOverride(ACPI_TABLE_HEADER *, ACPI_TABLE_HEADER **NewTable)
{
    if (!NewTable) return AE_BAD_PARAMETER;
    *NewTable = NULL;
    return AE_OK;
}

ACPI_STATUS AcpiOsPhysicalTableOverride(ACPI_TABLE_HEADER *, ACPI_PHYSICAL_ADDRESS *NewAddress, UINT32 *NewTableLength)
{
    if (!NewAddress || !NewTableLength) return AE_BAD_PARAMETER;
    *NewAddress = 0; *NewTableLength = 0;
    return AE_OK;
}

// ---------------------------------------------------------------- memory

// Allocations carry a 16-byte header holding the total size, because IOFree needs it and ACPICA does not pass it.
void *AcpiOsAllocate(ACPI_SIZE Size)
{
    if (Size == 0 || Size > (ACPI_SIZE)0x7fffffff) return NULL;
    UInt64 *p = (UInt64 *)IOMalloc((vm_size_t)Size + 16);
    if (!p) return NULL;
    p[0] = (UInt64)Size + 16;
    p[1] = 0;
    return p + 2;
}

void AcpiOsFree(void *Memory)
{
    if (!Memory) return;
    UInt64 *p = (UInt64 *)Memory - 2;
    IOFree(p, (vm_size_t)p[0]);
}

// Physical ranges are mapped with the default cache mode: RAM-backed pages (tables) stay cacheable, device
// ranges (operation regions) get the uncached attribute the pmap assigns to unmanaged memory.
void *AcpiOsMapMemory(ACPI_PHYSICAL_ADDRESS Where, ACPI_SIZE Length)
{
    if (Length == 0) return NULL;
    IOPhysicalAddress base = trunc_page_64(Where);
    UInt64 off = Where - base;
    IOByteCount len = (IOByteCount)round_page_64(off + Length);

    // kIOMemoryMapperNone: a CPU mapping of firmware memory never goes through the DMA mapper, and asking for the
    // system mapper here blocks until the platform has decided whether one exists (IOPlatformExpert::start), which
    // is after the tables we are reading to decide it. With the default options this hung the boot.
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(base, len, kIODirectionInOut | kIOMemoryMapperNone, NULL);
    if (!md) return NULL;
    IOMemoryMap *map = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapDefaultCache);
    if (!map) { md->release(); return NULL; }

    AcpiXnuMap *m = (AcpiXnuMap *)IOMalloc(sizeof *m);
    if (!m) { map->release(); md->release(); return NULL; }
    m->ret = (void *)(uintptr_t)(map->getVirtualAddress() + off);
    m->map = map;
    m->md  = md;
    IOLockLock(gMapLock);
    m->next = gMaps; gMaps = m;
    IOLockUnlock(gMapLock);
    return m->ret;
}

void AcpiOsUnmapMemory(void *LogicalAddress, ACPI_SIZE)
{
    AcpiXnuMap *m = NULL;
    IOLockLock(gMapLock);
    for (AcpiXnuMap **pp = &gMaps; *pp; pp = &(*pp)->next) {
        if ((*pp)->ret == LogicalAddress) { m = *pp; *pp = m->next; break; }
    }
    IOLockUnlock(gMapLock);
    if (!m) { IOLog("ACPI: unmap of an address that was never mapped: %p\n", LogicalAddress); return; }
    m->map->release();
    m->md->release();
    IOFree(m, sizeof *m);
}

ACPI_STATUS AcpiOsReadMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 *Value, UINT32 Width)
{
    A2XCrumbScope crumb(0x24, (UInt32)Address);
    if (!Value) return AE_BAD_PARAMETER;
    void *va = AcpiOsMapMemory(Address, Width / 8);
    if (!va) return AE_NO_MEMORY;
    ACPI_STATUS st = AE_OK;
    switch (Width) {
    case 8:  *Value = *(volatile UINT8  *)va; break;
    case 16: *Value = *(volatile UINT16 *)va; break;
    case 32: *Value = *(volatile UINT32 *)va; break;
    case 64: *Value = *(volatile UINT64 *)va; break;
    default: st = AE_BAD_PARAMETER; break;
    }
    AcpiOsUnmapMemory(va, Width / 8);
    return st;
}

ACPI_STATUS AcpiOsWriteMemory(ACPI_PHYSICAL_ADDRESS Address, UINT64 Value, UINT32 Width)
{
    A2XCrumbScope crumb(0x26, (UInt32)Address);
    void *va = AcpiOsMapMemory(Address, Width / 8);
    if (!va) return AE_NO_MEMORY;
    ACPI_STATUS st = AE_OK;
    switch (Width) {
    case 8:  *(volatile UINT8  *)va = (UINT8)Value;  break;
    case 16: *(volatile UINT16 *)va = (UINT16)Value; break;
    case 32: *(volatile UINT32 *)va = (UINT32)Value; break;
    case 64: *(volatile UINT64 *)va = Value;         break;
    default: st = AE_BAD_PARAMETER; break;
    }
    AcpiOsUnmapMemory(va, Width / 8);
    return st;
}

// ---------------------------------------------------------------- spin locks (interrupt-safe)

ACPI_STATUS AcpiOsCreateLock(ACPI_SPINLOCK *OutHandle)
{
    if (!OutHandle) return AE_BAD_PARAMETER;
    IOSimpleLock *l = IOSimpleLockAlloc();
    if (!l) return AE_NO_MEMORY;
    *OutHandle = (ACPI_SPINLOCK)l;
    return AE_OK;
}

void AcpiOsDeleteLock(ACPI_SPINLOCK Handle)
{
    if (Handle) IOSimpleLockFree((IOSimpleLock *)Handle);
}

ACPI_CPU_FLAGS AcpiOsAcquireLock(ACPI_SPINLOCK Handle)
{
    return (ACPI_CPU_FLAGS)IOSimpleLockLockDisableInterrupt((IOSimpleLock *)Handle);
}

void AcpiOsReleaseLock(ACPI_SPINLOCK Handle, ACPI_CPU_FLAGS Flags)
{
    IOSimpleLockUnlockEnableInterrupt((IOSimpleLock *)Handle, (IOInterruptState)Flags);
}

// ---------------------------------------------------------------- semaphores and mutexes
//
// One counting-semaphore mechanism serves both. A mutex is a semaphore of one unit, which gives AML Acquire()
// a real timed wait instead of a polling loop.

struct AcpiXnuSem {
    IOLock *lock;
    UINT32  units;
    UINT32  max;
};

ACPI_STATUS AcpiOsCreateSemaphore(UINT32 MaxUnits, UINT32 InitialUnits, ACPI_SEMAPHORE *OutHandle)
{
    if (!OutHandle || InitialUnits > MaxUnits) return AE_BAD_PARAMETER;
    AcpiXnuSem *s = (AcpiXnuSem *)IOMalloc(sizeof *s);
    if (!s) return AE_NO_MEMORY;
    s->lock = IOLockAlloc();
    if (!s->lock) { IOFree(s, sizeof *s); return AE_NO_MEMORY; }
    s->units = InitialUnits;
    s->max = MaxUnits;
    *OutHandle = (ACPI_SEMAPHORE)s;
    return AE_OK;
}

ACPI_STATUS AcpiOsDeleteSemaphore(ACPI_SEMAPHORE Handle)
{
    AcpiXnuSem *s = (AcpiXnuSem *)Handle;
    if (!s) return AE_BAD_PARAMETER;
    IOLockFree(s->lock);
    IOFree(s, sizeof *s);
    return AE_OK;
}

ACPI_STATUS AcpiOsWaitSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units, UINT16 Timeout)
{
    AcpiXnuSem *s = (AcpiXnuSem *)Handle;
    if (!s || Units == 0) return AE_BAD_PARAMETER;
    UInt64 deadline = 0;
    if (Timeout != ACPI_WAIT_FOREVER) clock_interval_to_deadline(Timeout, kMillisecondScale, &deadline);

    IOLockLock(s->lock);
    while (s->units < Units) {
        if (Timeout == 0) { IOLockUnlock(s->lock); return AE_TIME; }
        int r;
        if (Timeout == ACPI_WAIT_FOREVER) {
            r = IOLockSleep(s->lock, s, THREAD_UNINT);
        } else {
            AbsoluteTime d; memcpy(&d, &deadline, sizeof d);
            r = IOLockSleepDeadline(s->lock, s, d, THREAD_UNINT);
        }
        if (r == THREAD_TIMED_OUT && s->units < Units) { IOLockUnlock(s->lock); return AE_TIME; }
    }
    s->units -= Units;
    IOLockUnlock(s->lock);
    return AE_OK;
}

ACPI_STATUS AcpiOsSignalSemaphore(ACPI_SEMAPHORE Handle, UINT32 Units)
{
    AcpiXnuSem *s = (AcpiXnuSem *)Handle;
    if (!s || Units == 0) return AE_BAD_PARAMETER;
    IOLockLock(s->lock);
    if (s->units + Units > s->max) { IOLockUnlock(s->lock); return AE_LIMIT; }
    s->units += Units;
    IOLockWakeup(s->lock, s, false);
    IOLockUnlock(s->lock);
    return AE_OK;
}

ACPI_STATUS AcpiOsCreateMutex(ACPI_MUTEX *OutHandle)
{
    return AcpiOsCreateSemaphore(1, 1, (ACPI_SEMAPHORE *)OutHandle);
}

void AcpiOsDeleteMutex(ACPI_MUTEX Handle)
{
    (void)AcpiOsDeleteSemaphore((ACPI_SEMAPHORE)Handle);
}

ACPI_STATUS AcpiOsAcquireMutex(ACPI_MUTEX Handle, UINT16 Timeout)
{
    return AcpiOsWaitSemaphore((ACPI_SEMAPHORE)Handle, 1, Timeout);
}

void AcpiOsReleaseMutex(ACPI_MUTEX Handle)
{
    (void)AcpiOsSignalSemaphore((ACPI_SEMAPHORE)Handle, 1);
}

// ---------------------------------------------------------------- deferred work

struct AcpiXnuWork {
    ACPI_OSD_EXEC_CALLBACK fn;
    void                  *ctx;
    thread_call_t          call;
};

static void AcpiXnuRun(thread_call_param_t p0, thread_call_param_t)
{
    AcpiXnuWork *w = (AcpiXnuWork *)p0;
    w->fn(w->ctx);
    thread_call_t call = w->call;
    IOFree(w, sizeof *w);
    thread_call_free(call);     // permitted from inside the callout for THREAD_CALL_OPTIONS_ONCE calls

    IOLockLock(gExecLock);
    if (--gExecPending == 0) IOLockWakeup(gExecLock, &gExecPending, false);
    IOLockUnlock(gExecLock);
}

ACPI_STATUS AcpiOsExecute(ACPI_EXECUTE_TYPE Type, ACPI_OSD_EXEC_CALLBACK Function, void *Context)
{
    if (!Function) return AE_BAD_PARAMETER;
    AcpiXnuWork *w = (AcpiXnuWork *)IOMalloc(sizeof *w);
    if (!w) return AE_NO_MEMORY;
    thread_call_priority_t pri = (Type == OSL_GPE_HANDLER || Type == OSL_NOTIFY_HANDLER)
                                 ? THREAD_CALL_PRIORITY_KERNEL : THREAD_CALL_PRIORITY_USER;
    w->fn = Function;
    w->ctx = Context;
    w->call = thread_call_allocate_with_options(AcpiXnuRun, w, pri, THREAD_CALL_OPTIONS_ONCE);
    if (!w->call) { IOFree(w, sizeof *w); return AE_NO_MEMORY; }

    IOLockLock(gExecLock);
    gExecPending++;
    IOLockUnlock(gExecLock);
    thread_call_enter(w->call);
    return AE_OK;
}

void AcpiOsWaitEventsComplete(void)
{
    IOLockLock(gExecLock);
    while (gExecPending) IOLockSleep(gExecLock, &gExecPending, THREAD_UNINT);
    IOLockUnlock(gExecLock);
}

ACPI_THREAD_ID AcpiOsGetThreadId(void)
{
    return (ACPI_THREAD_ID)(uintptr_t)current_thread();
}

// ---------------------------------------------------------------- time

UINT64 AcpiOsGetTimer(void)
{
    UInt64 ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 100;            // ACPI timer units are 100 ns
}

void AcpiOsSleep(UINT64 Milliseconds)
{
    while (Milliseconds > 0x7fffffff) { IOSleep(0x7fffffff); Milliseconds -= 0x7fffffff; }
    IOSleep((unsigned)Milliseconds);
}

void AcpiOsStall(UINT32 Microseconds)
{
    IODelay(Microseconds);
}

// ---------------------------------------------------------------- interrupts
//
// ACPICA asks for exactly one interrupt: the SCI. The platform expert owns the interrupt source for it (it
// knows the controller), so this only records the handler; the expert calls AcpiXnuDispatchSci from its
// work loop.

ACPI_STATUS AcpiOsInstallInterruptHandler(UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine, void *Context)
{
    if (!ServiceRoutine) return AE_BAD_PARAMETER;
    if (gSciHandler) return AE_ALREADY_EXISTS;
    gSciIrq = InterruptNumber;
    gSciContext = Context;
    gSciHandler = ServiceRoutine;
    return AE_OK;
}

ACPI_STATUS AcpiOsRemoveInterruptHandler(UINT32 InterruptNumber, ACPI_OSD_HANDLER ServiceRoutine)
{
    if (!ServiceRoutine || ServiceRoutine != gSciHandler || InterruptNumber != gSciIrq) return AE_NOT_EXIST;
    gSciHandler = NULL;
    gSciContext = NULL;
    return AE_OK;
}

extern "C" UINT32 AcpiXnuSciIrq(void)
{
    return gSciIrq;
}

extern "C" UINT32 AcpiXnuDispatchSci(void)
{
    ACPI_OSD_HANDLER h = gSciHandler;
    return h ? h(gSciContext) : ACPI_INTERRUPT_NOT_HANDLED;
}

// ---------------------------------------------------------------- I/O ports and PCI configuration

ACPI_STATUS AcpiOsReadPort(ACPI_IO_ADDRESS Address, UINT32 *Value, UINT32 Width)
{
    A2XCrumbScope crumb(0x20, ((UInt32)Address << 8) | Width);
    if (!Value) return AE_BAD_PARAMETER;
    switch (Width) {
    case 8:  *Value = inb((u_short)Address); return AE_OK;
    case 16: *Value = inw((u_short)Address); return AE_OK;
    case 32: *Value = inl((u_short)Address); return AE_OK;
    default: return AE_BAD_PARAMETER;
    }
}

ACPI_STATUS AcpiOsWritePort(ACPI_IO_ADDRESS Address, UINT32 Value, UINT32 Width)
{
    A2XCrumbScope crumb(0x22, ((UInt32)Address << 8) | Width);
    switch (Width) {
    case 8:  outb((u_short)Address, (u_char)Value);  return AE_OK;
    case 16: outw((u_short)Address, (u_short)Value); return AE_OK;
    case 32: outl((u_short)Address, Value);          return AE_OK;
    default: return AE_BAD_PARAMETER;
    }
}

// Configuration mechanism #1 (0xCF8/0xCFC): segment 0, registers 0-255. Extended space (256-4095) and other
// segments need the MCFG/ECAM window, which the platform expert maps once it has parsed MCFG; until then they
// are refused rather than aliased onto the low 256 bytes.
static UINT32 AcpiXnuCf8(const ACPI_PCI_ID *id, UINT32 reg)
{
    return 0x80000000u | ((UINT32)(id->Bus & 0xff) << 16) | ((UINT32)(id->Device & 0x1f) << 11)
         | ((UINT32)(id->Function & 0x7) << 8) | (reg & 0xfc);
}

static UINT32 AcpiXnuPciRead32Locked(const ACPI_PCI_ID *id, UINT32 reg, UINT32 width)
{
    outl(0xcf8, AcpiXnuCf8(id, reg));
    u_short port = (u_short)(0xcfc + (reg & 3));
    if (width == 8)  return inb(port);
    if (width == 16) return inw(port);
    return inl(0xcfc);
}

static void AcpiXnuPciWrite32Locked(const ACPI_PCI_ID *id, UINT32 reg, UINT32 v, UINT32 width)
{
    outl(0xcf8, AcpiXnuCf8(id, reg));
    u_short port = (u_short)(0xcfc + (reg & 3));
    if (width == 8)       outb(port, (u_char)v);
    else if (width == 16) outw(port, (u_short)v);
    else                  outl(0xcfc, v);
}

static ACPI_STATUS AcpiXnuPciCheck(const ACPI_PCI_ID *id, UINT32 reg, UINT32 width)
{
    if (!id) return AE_BAD_PARAMETER;
    if (width != 8 && width != 16 && width != 32 && width != 64) return AE_BAD_PARAMETER;
    if (reg & ((width / 8) - 1)) return AE_BAD_PARAMETER;      // naturally aligned only
    if (id->Segment != 0 || reg + width / 8 > 256) return AE_SUPPORT;
    return AE_OK;
}

ACPI_STATUS AcpiOsReadPciConfiguration(ACPI_PCI_ID *PciId, UINT32 Reg, UINT64 *Value, UINT32 Width)
{
    if (!Value) return AE_BAD_PARAMETER;
    ACPI_STATUS st = AcpiXnuPciCheck(PciId, Reg, Width);
    if (ACPI_FAILURE(st)) return st;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(gPciLock);
    if (Width == 64) {
        UINT64 lo = AcpiXnuPciRead32Locked(PciId, Reg, 32);
        UINT64 hi = AcpiXnuPciRead32Locked(PciId, Reg + 4, 32);
        *Value = lo | (hi << 32);
    } else {
        *Value = AcpiXnuPciRead32Locked(PciId, Reg, Width);
    }
    IOSimpleLockUnlockEnableInterrupt(gPciLock, is);
    return AE_OK;
}

ACPI_STATUS AcpiOsWritePciConfiguration(ACPI_PCI_ID *PciId, UINT32 Reg, UINT64 Value, UINT32 Width)
{
    ACPI_STATUS st = AcpiXnuPciCheck(PciId, Reg, Width);
    if (ACPI_FAILURE(st)) return st;
    IOInterruptState is = IOSimpleLockLockDisableInterrupt(gPciLock);
    if (Width == 64) {
        AcpiXnuPciWrite32Locked(PciId, Reg, (UINT32)Value, 32);
        AcpiXnuPciWrite32Locked(PciId, Reg + 4, (UINT32)(Value >> 32), 32);
    } else {
        AcpiXnuPciWrite32Locked(PciId, Reg, (UINT32)Value, Width);
    }
    IOSimpleLockUnlockEnableInterrupt(gPciLock, is);
    return AE_OK;
}

// ---------------------------------------------------------------- output, signals, sleep

void AcpiOsVprintf(const char *Format, va_list Args)
{
    char buf[256];
    vsnprintf(buf, sizeof buf, Format, Args);
    IOLog("%s", buf);
}

void ACPI_INTERNAL_VAR_XFACE AcpiOsPrintf(const char *Format, ...)
{
    va_list ap;
    va_start(ap, Format);
    AcpiOsVprintf(Format, ap);
    va_end(ap);
}

ACPI_STATUS AcpiOsSignal(UINT32 Function, void *Info)
{
    if (Function == ACPI_SIGNAL_FATAL && Info) {
        ACPI_SIGNAL_FATAL_INFO *f = (ACPI_SIGNAL_FATAL_INFO *)Info;
        IOLog("ACPI: AML Fatal type 0x%x code 0x%x arg 0x%x\n", f->Type, f->Code, f->Argument);
    }
    return AE_OK;
}

ACPI_STATUS AcpiOsEnterSleep(UINT8, UINT32, UINT32)
{
    return AE_OK;   // no pre-sleep hook: the platform expert drives sleep through the power plane
}
