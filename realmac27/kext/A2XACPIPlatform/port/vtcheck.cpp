// Calls the two virtual slots the kernel uses; build-kext.sh asserts their offsets.
#include "cpuint.h"
extern "C" UInt32 a2x_vt_cpunum(IOCPU *c) { return c->getCPUNumber(); }
extern "C" IOReturn a2x_vt_handle(IOCPUInterruptController *c) { return c->handleInterrupt(0, 0, 0); }
extern "C" IOReturn a2x_vt_regctl(IOPlatformExpert *p) { return p->registerInterruptController(0, 0); }
