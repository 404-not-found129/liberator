# NullMoth Golden Gate: system package (macOS 27.2, build 26B123)

Lets Apple's programs that are built for Apple silicon only run on an Intel PC. It installs an interpreter, prepares
those programs to start through it, and adds the start-up scripts for the NVIDIA display.

1401 runs this for you in the last step of Golden Gate. By hand:

    sudo bash install-system.sh              shows what it would do, changes nothing
    sudo bash install-system.sh --apply      installs
    sudo bash install-system.sh --uninstall  puts every changed program and installed file back
    sudo bash install-system.sh --system "/Volumes/X" --data "/Volumes/X - Data" [--dry-run | --apply | --uninstall]

The last form sets up a macOS 27 that is not running, from another macOS on the same PC.

Every program it changes is kept first under `/Library/NullMoth/golden-gate/orig`. It writes one log,
`/Library/NullMoth/golden-gate/install.log`. It refuses any macOS build but 26B123 and any file of this package that
is missing or changed.
