# NullMoth Golden Gate: driver package (macOS 27.2, build 26B123)

Installs the NullMoth NVIDIA driver for macOS 27: the kernel extensions and their kernel collection, the loader that
hands the collection to the kernel at start-up, and the Metal driver with its Vulkan back end and settings.

1401 runs this for you in the last step of Golden Gate. By hand:

    sudo bash install-driver.sh              shows what it would do, changes nothing
    sudo bash install-driver.sh --apply      installs
    sudo bash install-driver.sh --uninstall  puts back every file it replaced and removes every file it added
    sudo bash install-driver.sh --system "/Volumes/X" --data "/Volumes/X - Data" [--dry-run | --apply | --uninstall]

Every file is checked against MANIFEST before anything is touched. A replaced file is kept first under
`/Library/NullMoth/golden-gate/driver/orig`. It writes one log, `/Library/NullMoth/golden-gate/driver/install.log`.
It refuses any macOS build but 26B123.
