# Liberator

Liberator moves an OpenCore PC with an NVIDIA card from macOS 15 or 26 to macOS 27, and sets up the NullMoth NVIDIA
driver there. It is a Mac app with one main button: **Prepare for Golden Gate**.

This is a beta. macOS 27 has been started this way on one desktop PC: an Intel processor with an RTX 50-series card.
Other boards, AMD processors and laptops are untried.

A PC without an NVIDIA card can make the same move on its AMD or Intel graphics. That is untried on real hardware:
see [AMD and Intel graphics](#amd-and-intel-graphics).

- Changes: [CHANGELOG.md](CHANGELOG.md)
- NVIDIA driver for macOS 15 and 26: [nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver)
- Building the first macOS install on a PC: [1401](https://github.com/nullmoth/1401)

## What you need

- A PC that already runs macOS 15 or macOS 26 through OpenCore, with the NullMoth NVIDIA driver working. Or, with no
  NVIDIA card, a PC that runs macOS 15 or 26 through OpenCore on a Radeon card or on AMD or Intel processor graphics.
- About 92 GB free on the macOS disk for a first run. About 27 GB of that is downloads, most of it macOS 27 from Apple.
- A wired network connection. Wi-Fi and Bluetooth do not work on macOS 27 yet.
- About 30 minutes, plus the download.
- A backup of anything you cannot lose.

## Using it

1. Open Liberator on macOS 15 or 26.
2. Leave **Set up as a new Mac instead** unticked to bring your account over (name, password, administrator rights;
   not the files in your home folder). Tick it to start at Apple's setup instead.
3. Press **Prepare for Golden Gate** and confirm once. Liberator checks the PC, downloads what it needs, builds the
   startup files macOS 27 needs on this PC, puts macOS 27 on its own new volumes beside your current macOS, sets it
   up, and makes macOS 27 the default at startup.
4. When it says it is done, restart. The start text pauses at "PCI configuration" for up to two minutes. Wait.

If it stops part-way, press the button again. Steps that finished are not repeated and the downloads are kept.

## AMD and Intel graphics

Liberator does not need an NVIDIA card. When it finds none that the NVIDIA driver takes, it looks for an AMD or Intel
graphics chip and sets macOS 27 up on that one instead:

- **Which driver.** macOS 27 has no AMD or Intel graphics driver of Apple's. The macOS 27 support Liberator downloads
  carries NullMoth's own driver for those chips (NMXGPU, the "test" driver), with the chip's firmware from
  linux-firmware. Liberator leaves the NVIDIA driver and its settings out and turns that one on. This holds for every
  Radeon on a PC, including the ones Apple's own driver runs under macOS 15 and 26 (RX 400/500, Vega, RX 5000,
  RX 6000): a PC's macOS has Apple's Radeon drivers only inside its kernel collections and shared library cache, not
  as files that could be carried over, so they cannot follow to macOS 27. Only a real Mac takes Apple's drivers along.
  Whether the test driver takes a particular card is decided by the driver itself, not by Liberator. The driver in
  the Beta 3 support lists the RX 470/480/570/580/590 (Polaris 10) and its firmware.
- **The driver's own rule about Apple's cards.** That driver keeps a list of the Radeon and Intel chips Apple's
  drivers run (71 Radeon device IDs in Beta 3) and leaves a chip on it to Apple's driver, unless the boot argument
  `nmxamd_native` (for Intel, `nmxintel_native`) is not 0. On a PC's macOS 27 there is no Apple driver to leave it
  to, so Liberator adds `nmxamd_native=1` (and `nmxintel_native=1` where the PC has Intel graphics) to the macOS 27
  startup files of a PC on this path. Without it the driver would not take such a card at all. The startup files
  are shared with macOS 15 or 26, so if NullMoth's test driver is also installed there, it takes the card there too.
- **What you get.** The first start draws the desktop in software on the driver's framebuffer. There is no graphics
  acceleration and no Metal at that point, so a card that is fast under macOS 26 will be slow under macOS 27. The
  check at the start says this before anything is changed.
- **Startup files.** An NVIDIA PC reaches Liberator through 1401, which makes its startup files. A PC without an
  NVIDIA card never did, so on AMD or Intel graphics Liberator accepts the PC's own OpenCore files: any whole
  OpenCore config that loads VirtualSMC or FakeSMC. Drivers and kernel patches in it that the macOS 27 support does
  not know stay at macOS 26 and are named when the files are staged.
- **AMD processors.** The kit's own macOS 27 kernel patches are added for an AMD processor, also when the PC's
  config has no patch list at all. The usual AMD patch list is carried to macOS 27 only where the kit has checked a
  patch against the macOS 27 kernel; any other patch is named and stays off there, and macOS 27 may then not start.
  A patch is compared the way OpenCore reads it, so the usual list's `_cpuid_set_info ` (written with a space
  after the name) counts as the checked patch it is. Staged from one Ryzen PC's real config, 13 patches are
  carried, 1 is replaced by the kit's own two, and 1 (IOPCIFamily, 10 bit tags) stays at macOS 26.

- **Acceleration, as a second step.** After macOS 27 has reached its desktop once, start macOS 15 or 26 again and
  press **Turn on graphics acceleration**. It adds the kit's accelerator (NMXAccel, with the Radeon or Intel Vulkan
  driver behind the Metal plugin) to macOS 27 and switches it on with the boot argument `nmxfb_pipe=1`. The kit's
  start-up gate goes back to the software desktop when the accelerated display cannot be armed. **Turn it off**
  takes it back. System Integrity Protection must be lowered on the macOS you start from; the check says so.

None of this has been run on real hardware. Your current macOS and its startup files are kept, and
**Cancel Golden Gate** puts them back.

Building the app from this source, with the macOS 27 support of Beta 3:

    V3_KIT_PIN="$PWD/release/beta3/v3-kit.pin" bash mac/build.sh

The app is then at `mac/build/Liberator.app`. It is signed for this Mac only, and an update offered inside the app
would replace it with NullMoth's own build, which does not have this path for Radeon cards Apple's driver runs.

## Going back

Your current macOS is not changed. In the OpenCore boot picker choose your macOS 15 or 26 disk, open Liberator
there, and press **Cancel Golden Gate** to put the old startup files back, or **Remove macOS 27** to also erase the
macOS 27 volumes Liberator made.

## Known limits of this beta

- No Wi-Fi and no Bluetooth on macOS 27.
- Books, Journal, Maps, Reminders and Voice Memos do not work yet.
- Spotlight search does not work.
- The login window can show a message that the System keychain cannot be found. Press Cancel.
- System Integrity Protection is lowered and Apple's code-signing enforcement is off on macOS 27. Treat it as a test system.
- The Dock and other glass surfaces can look flat and dark.
- A run that fails after the system copy can leave one extra volume named "Macintosh HD" in the same container.
  Delete it in Disk Utility before the next try.

## If something goes wrong

Start your macOS 15 or 26 from the boot picker and press **Send a report** in Liberator. It can read the macOS 27
logs from there, so this works when macOS 27 never reached its desktop. Quote the report ID when you ask for help.

## Licence

See [LICENSE](LICENSE) and [NOTICE.md](NOTICE.md).
