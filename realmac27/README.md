# macOS 27.2 on a real Mac Pro 6,1 (late 2013, two FirePro D300)

This tree holds everything that makes macOS 27.2 boot and run on the Mac Pro 6,1, with the real AMD graphics
drivers, Ethernet, USB 2, USB 3, Bluetooth and audio. It is meant to be wired into the installer so that a real Mac
can be brought up the same way the PCs are. The kit's existing pieces (interpreter, kernel collections from the KDK,
lay-down script, OpenCore) are reused unchanged; this tree adds what a real Mac needs on top.

Status on the reference machine (Mac Pro 6,1, 16 GB, macOS 27.2 build 26B123, booted through OpenCore):

| Area | State |
|---|---|
| Boot, login, desktop on the D300 | works |
| Ethernet (both ports), ssh | works (DHCP started by a helper script, see Open items) |
| USB 2 + Bluetooth (BCM20702) | works |
| USB 3 (Fresco FL1100), keyboard, pen display, flash drive | works; reads 55 MB/s vs 105 MB/s on Tahoe |
| Thunderbolt 2 (three controllers) | controllers and Apple drivers load; untested with a device |
| FAT/exFAT drives (FSKit) | mount |
| Audio (onboard CS4208 + HDMI) | all outputs play; the internal speaker fix is built but not yet confirmed by ear |
| Dock / app icons | correct, on RenderBox's CPU renderer (see Graphics) |
| Wi-Fi (BCM4360) | not done |

## Components

### kext/A2XACPIPlatform - platform driver (required on every Intel Mac)
macOS 27 ships no Intel platform expert, ACPI driver, PCI host bridge or interrupt controller. This kext is all of
them: ACPICA (upstream/acpica, BSD-3) plus our XNU layer in port/.
- `expert.cpp` - the platform expert: ACPI namespace, device nubs, power, restart/halt, panic log saved to NVRAM.
- `cpuint.cpp` - local APIC / IO-APIC interrupt controller, MSI.
- `pcibridge.cpp` - PCI host bridge over ECAM (MCFG), `_CRS` windows, `_OSC`, and the bus-number fix below.
- `vtdmap.cpp` - an Intel VT-d DMA mapper (IOMapper, not the system mapper). Network (class 02) and USB host (class
  0c03) functions get a 1 GB translated window at IOVA 512 GB; everything else stays pass-through. Needed because
  27's Broadcom Ethernet driver requires a mapper to turn a scattered ring into one DMA segment, and because USB
  host controllers in 64-bit mode put every queue head and descriptor under one high dword: USB hosts get the
  property `ctrldsseg` = window base >> 32 (0x80), which EHCI loads into CTRLDSSEGMENT (same pattern Apple uses with
  its own DMA mappers).
- `confollow.h`, `crumb.cpp`, `devprops.cpp`, `events.cpp`, `buttons.cpp` - boot-screen following, a crash-surviving
  breadcrumb ring, device properties, ACPI events, power button.

Boot-args the kext reads (set in OpenCore `boot-args`):
- `a2x_pcibus=0xBBDDFFSSUU` - give a root-bus bridge that firmware left with no bus numbers the range macOS itself
  uses. Mac Pro 6,1: `a2x_pcibus=0x10010fe` (bridge 0:1.0 "PEG0" -> buses 0x10-0xfe). Firmware leaves PEG0 at ff-ff;
  without this the configurator numbers the Thunderbolt/USB 3 subtree over the buses the PCH ports (Ethernet, SSD,
  Wi-Fi) already use. With it, 27 gets exactly Tahoe's numbering and USB 3 + Thunderbolt appear. Mac Pro only.
- `a2x_nondrv=1` - keep IONDRVFramebuffer off the GPU display functions so the AMD framebuffer driver gets them.
- `a2x_pcihide`, `a2x_pcihold` - mask or hold a PCI function (bring-up tools; not needed on the Mac Pro now).
- `a2x_vtdmap=0` - turn the VT-d mapper off.
- `a2x_vtdusb=0|1` - force the USB-host part of the mapper off or on. Default: on only when the firmware's DSDT
  OEM ID is "APPLE" (real Macs), off on PCs, so one kext serves both.
- `a2xbusydump=2` - 30-second heartbeat in the kernel log (interrupt counts, VT-d health). Diagnostic only.

### audio/NMXAudio - HD Audio driver (required on every Intel Mac; 27 has no AppleHDA)
FreeBSD's snd_hda (hdac/hdacc/hdaa + quirks) on our FreeBSD-on-XNU layer (audio/bsdcompat), with an IOAudioFamily
front end (`nmx_audio_kext.cpp`, `nmx_pcm.c`). Matches any HD Audio controller by PCI class. Publishes one CoreAudio
device per codec association (speaker, headphones, line out, digital, each HDMI pin).
Mac-specific quirks added in `freebsd-hda/hdaa_patches.c` (diffs beside it):
- every Apple Cirrus CS4208: GPIO0 high = internal speaker amplifier on (Linux does the same for all Apple CS4208).
- Mac Pro 6,1 (codec subsystem 0x106b7800) and Mac mini 7,1 (0x106b6c00): inverted jack sense, mic (0x18) and SPDIF
  (0x21) pins without presence detect, CS4208 coefficient init.
- the analog codec's live state (GPIO, every pin, associations) is published as the registry property
  `nmx-hda-diag` (`ioreg -rc NMXAudio -l | grep nmx-hda-diag`).
The kext must be linked into the auxiliary kernel collection (it depends on IOAudioFamily, which lives in the system
collection); `kmutil load` of a third-party kext is not available on 27 on this hardware.

### Graphics on the D300
- Kernel: Tahoe's AMD drivers for this GPU - AMDSupport, AMD7000Controller, AMDFramebuffer, AMDRadeonX4000,
  AMDRadeonX4000HWServices (with its AMDRadeonX4030HWLibs plug-in), AMDRadeonServiceManager - linked into 27's
  auxiliary kernel collection against 27's own kernel and boot/system collections (tools/link-aux-kc.sh).
- User space: Tahoe's AMD bundles copied into 27's /System/Library/Extensions (AMDMTLBronzeDriver, AMDShared,
  AMDRadeonX4000GLDriver, AMDRadeonVADriver, AMDRadeonVADriver2, AMDRawCounterPlugin; the X5000/X6000 bundles are
  present too but unused on this GPU).
- `gpu/amdil_rtfix` - 27's precompiled shaders describe render targets in two newer AIR forms that this AMD shader
  compiler (libAMDIL902, from Tahoe) rejects, so every pipeline failed. The dylib hooks the compiler's
  initShaderArgType and rewrites those to the classic shape. It is loaded by an added LC_LOAD_DYLIB in
  AMDShared.bundle/Contents/PlugIns/libAMDIL902.dylib (addlc.py adds it; the binary is re-signed ad hoc; the original
  is kept). Offsets are for one exact libAMDIL902 build (18,187,216 bytes); the hook refuses to install if the
  prologue differs.
- RenderBox runs on its CPU renderer: `launchctl setenv RB_DISABLE_GPU 1` from a LaunchDaemon and a LaunchAgent
  (config/). Reason: 27's RenderBox does its blending in the shader with framebuffer fetch (render targets read as
  fragment inputs), which this GPU does not have, so on the GPU path every icon composites to transparent and
  IconServices shows placeholders. Proven by measurement: pipelines build, command buffers complete, draws run,
  IOSurface readback is correct (tools/oracles/rt.m, ios.m pass 10/10 on the D300); the only bad thing is the
  framebuffer reads. Making the GPU path work needs framebuffer-fetch emulation in the AMD compiler and driver.

## What the installer has to do for a Mac Pro 6,1 (run from Tahoe on the same Mac)
1. Lay down 27 and its kernel collections exactly as for the PCs (laydown/, make-kernel-collections.sh). The boot
   collection must be the kit's (sha256 de7681c1...); the auxiliary collection is linked against it.
2. OpenCore: inject A2XACPIPlatform.kext (built from this tree) with the kit's other early kexts; boot-args add
   `a2x_pcibus=0x10010fe a2x_nondrv=1`.
3. Copy Tahoe's AMD user-space bundles (list above) into the 27 system volume's /System/Library/Extensions.
4. Patch libAMDIL902 in 27's AMDShared.bundle with addlc.py to load amdil_rtfix.dylib from its install path; install
   the dylib; keep the original.
5. Install NMXAudio.kext into 27's /Library/Extensions, then link the auxiliary collection on Tahoe with root:
   `kmutil create -n aux -a x86_64 --allow-missing-kdk --build 26B123 -z --no-authentication --kernel <27 kernel>
   -B <27 BootKernelExtensions.kc> -S <27 SystemKernelExtensions.kc> --repository <dir with the 7 AMD kexts +
   NMXAudio> -x -b <each bundle id> -A <out>` and place it as 27's /Library/KernelCollections/
   AuxiliaryKernelExtensions.kc (root:wheel 644). tools/link-aux-kc.sh is the working version. This step must run
   on Tahoe: 27's own kmutil refuses this boot/system collection pair, and kmutil skips kexts not owned by root.
6. Install the two RB_DISABLE_GPU plists (config/).
7. Make the default audio output the internal one (tools/audsel.c sets it; macOS otherwise picks an HDMI output).
8. Restart into 27 once (bless --nextonly from Tahoe); Tahoe stays the default.

## Generic vs Mac Pro only vs not done (other Macs)
| Piece | Other Intel Macs |
|---|---|
| A2XACPIPlatform (ACPI, interrupts, PCI, VT-d mapper) | all of them |
| NMXAudio | 2012-2015 Macs (CS4206/CS4208 quirks; the remaining SSID rows are small additions). Not 2016-17 (CS8409 + amps, needs a new driver) and not T2 Macs (audio is behind the T2) |
| RenderBox CPU path | any Mac without an Apple GPU |
| a2x_pcibus value | Mac Pro 6,1 only; other models need their own value or none |
| D300 kexts + amdil_rtfix | Mac Pro 6,1 / GCN1 AMD only. Newer AMD Macs need their Tahoe driver family (X5000/X6000) and their own compiler check; Intel-graphics Macs untested |
| Keyboard / trackpad | 2012-2015 USB-HID MacBooks likely work with Apple's drivers already in 27; SPI keyboards (2015-17) and T2 Macs need new work |
| Wi-Fi | not done on any Mac |

The USB-host part of the VT-d mapper (class 0c03 translated + `ctrldsseg`) engages only on Apple firmware (DSDT OEM
ID "APPLE"; SMBIOS cannot be used, boot loaders show a Mac model on PCs), so PC behaviour is unchanged.

## Open items
- Internal speaker: confirm by ear (play on "Cirrus Logic CS4208 (Analog)"). If silent, read `nmx-hda-diag` and
  check the DAC/mixer widgets feeding pin 0x12.
- VT-d IOTLB invalidation times out (10 ms bound) on this unit in every mode tried; timed-out IOVA ranges are retired
  instead of reused (safe; leaks ~9 pages/min of a 262,144-page window). USB 3 runs at about half of Tahoe's speed
  because of it. A queued-invalidation path (as Apple's own VT-d driver uses) is the likely real fix.
- HDMI audio publishes 12 devices; only connected displays should appear.
- sharingd is disabled on the reference machine (it crashed in the Swift runtime inside a logged-in session);
  needs a repro with a user logged in.
- Ethernet DHCP is started by a helper script (netup27.sh on the reference machine), not by configd on its own.
- Dock icons on the GPU path: needs framebuffer-fetch emulation (see Graphics).

## Building
- Kext: `cd kext/A2XACPIPlatform; bash build-acpica.sh; SDK27_KERNEL_HEADERS=<Kernel.framework/Headers from the
  macOS 26.5 SDK with IOKit/pci replaced by 27's> bash build-kext.sh` -> out/A2XACPIPlatform.kext. The build runs
  port/confollow_selftest.c first and stops if it fails; it also checks three vtable slots against the kernel.
- NMXAudio: `XKPI=<FreeBSD-on-XNU tree> FREEBSD_SYS=<freebsd-src/sys with freebsd-hda/ applied> KPI=<kpi tree>
  bash audio/NMXAudio/build.sh <out dir>`. It links the layer objects built by audio/bsdcompat/ccsrc.sh and ccxx.sh.
- amdil_rtfix: `clang -arch x86_64 -dynamiclib -O2 amdil_rtfix.c -o amdil_rtfix.dylib; codesign -s - amdil_rtfix.dylib`.
- Tools: audsel/audvol `clang -arch x86_64 X.c -framework CoreAudio -framework CoreFoundation`; oracles
  `clang -arch x86_64 -fobjc-arc rt.m -framework Metal -framework Foundation` (ios.m also needs IOSurface).
