#!/bin/bash
# build-acpica.sh - compile the ACPICA core (BSD-3) for the macOS 27 x86_64 kernel. Output: obj/*.o, libacpica.a
# Excludes the debugger and disassembler (not built into the platform driver).
set -u
A=$(cd "$(dirname "$0")" && pwd); cd "$A"
KINC="$(xcrun --show-sdk-path)/System/Library/Frameworks/Kernel.framework/Headers"
CF=(-arch x86_64 -mkernel -nostdinc -fno-builtin -O2 -std=gnu11 -DKERNEL -DACPI_XNU_KERNEL
    -I"$KINC" -I"$A/acpica/source/include" -w)
mkdir -p obj; rm -f obj/*.o libacpica.a build.log
n=0; f=0
for c in dispatcher events executer hardware namespace parser resources tables utilities; do
  for s in acpica/source/components/$c/*.c; do
    case "$(basename "$s")" in rsdump.c|rsdumpinfo.c) continue;; esac  # debug-only dump tables (ACPI_DEBUG_OUTPUT)
    n=$((n+1)); xcrun clang "${CF[@]}" -c "$s" -o "obj/$(basename "$s" .c).o" >> build.log 2>&1 || f=$((f+1))
  done
done
echo "files=$n failed=$f"
[ "$f" = 0 ] && xcrun libtool -static -o libacpica.a obj/*.o && echo "libacpica.a $(stat -f %z libacpica.a) bytes"
exit $f
