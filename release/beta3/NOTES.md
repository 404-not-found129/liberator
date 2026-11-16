# Liberator Beta 3

Liberator moves an OpenCore PC that runs macOS 15 or macOS 26 to macOS 27.2 with one button, **Prepare for Golden Gate**. Beta 3 brings the 3.0 NVIDIA driver to macOS 27 and fixes the start-up problems Beta 2 reports showed.

## What is new

- The 3.0 NVIDIA driver, built for macOS 27. It is the same driver generation as the 3.0 release for macOS 15 and 26: kernel drivers, Metal driver and shader translator together.
- A hard freeze as the desktop started is fixed. The driver described the whole card memory window to macOS instead of the part the screen uses, and the machine stopped when the desktop took over the screen.
- The driver starts again cleanly after a first failed attempt in the same start, instead of staying off.
- Setup no longer stops on a boot partition that holds more than one OpenCore folder. It uses the one OpenCore started from.
- Fetching Apple's signing keys is retried, so one failed connection no longer ends the preparation.
- A PC prepared from the Mac app has its disk and network controllers marked built in. macOS 27 then reads the internal disk as internal and the wired port as built in, which signing in to an Apple account needs.
- Start reports, if you agree. Liberator asks once. With a yes, macOS 27 sends NullMoth one short summary of the PC and of its first start that reached the desktop, and sends the start-up logs of a start that did not. With a no, nothing is sent. The summary holds hardware models and driver state, no names, files or serial numbers.
- Widgets that are set up through Shortcuts (Weather, Photos and others) now show their content instead of staying empty.
- Icons drawn before the driver fix (Safari, Maps, FaceTime in the Dock) are redrawn after the update.
- Real Macs: every Intel Mac from 2013 on that runs macOS Tahoe. Liberator reads the Mac's own Tahoe and carries every driver it uses that macOS 27 lacks (graphics, Wi-Fi, Bluetooth, audio, USB, Thunderbolt) over to macOS 27, so start Tahoe with everything working first. Brought up on a Mac Pro (Late 2013); other Macs are untried, please send a report.
- The source of everything Liberator installs on macOS 27 is now in the Liberator repository: the interpreter, the translator, the Sandbox policy, the kit's installers and the real-Mac drivers.
- For testing: a package for PCs that have only AMD or Intel built-in graphics. It is untried on real hardware. Please send a report either way.

## What you need

- macOS 15 or macOS 26 running through OpenCore, with the NullMoth NVIDIA driver working
- about 92 GB free on the macOS disk
- a wired network connection
- a backup of anything you cannot lose

## Updating from Beta 2

Download `@APP_ZIP@`, open it, and press **Prepare for Golden Gate**. Your macOS 27 volumes are kept; the driver and the startup files are renewed.

## Known problems

- Apps on macOS 27 run without Apple's app sandbox: macOS 27 has no Sandbox for Intel, and the stand-in Liberator installs does not enforce app profiles yet. Install only apps you trust.
- On rare starts the desktop restarts once before it settles
- A laptop's own screen is untested with this driver
- AMD processors are built for but have no confirmed start yet
- No Wi-Fi yet on macOS 27
- Bluetooth does not pair yet
- Apple Intelligence and Siri are not available
- Browsers and other programs that write their own machine code while running do not work yet
- Spotlight search does not work
- App Store apps are untested
- Real Intel Macs are not supported yet

## Files

| File | Size | SHA-256 |
|---|---|---|
| `@APP_ZIP@` | @APP_SIZE@ | `@APP_SHA@` |
| `@KIT_NAME@` | @KIT_SIZE@ | `@KIT_SHA@` |

Liberator downloads the second file itself and checks it against the SHA-256 it was built with. If the download fails, choose **Use a file I already downloaded** in the app.

## If something goes wrong

Start your macOS 15 or 26 from the boot picker and press **Send a report** in Liberator. Quote the report ID when you ask for help.

Liberator is free. Donations help keep the work going: nullmothsystems.com/donate
