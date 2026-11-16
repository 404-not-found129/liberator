#!/bin/bash
# on Tahoe, sudo: relink 27's aux KC (7 D300 drivers + NMXAudio from 27's /Library/Extensions) and install it
V27="/Volumes/macOS 27"; D27="/Volumes/macOS 27 - Data"
diskutil mount disk1s8 >/dev/null 2>&1; diskutil mount disk1s7 >/dev/null 2>&1
W=/Users/Shared/aux3; rm -rf $W/repo/NMXAudio.kext $W/out; mkdir -p $W/out
cp -R "$D27/Library/Extensions/NMXAudio.kext" $W/repo/ && chown -R root:wheel $W/repo && chmod -R go-w $W/repo
B=(); for id in com.apple.kext.AMDSupport com.apple.kext.AMD7000Controller com.apple.kext.AMDFramebuffer com.apple.kext.AMDRadeonX4000 com.apple.kext.AMDRadeonX4000HWServices com.apple.kext.AMDRadeonX4030HWLibs com.apple.kext.AMDRadeonServiceManager com.nullmoth.NMXAudio; do B+=(-b "$id"); done
kmutil create -n aux -a x86_64 --allow-missing-kdk --build 26B123 -z --no-authentication --kernel $W/kc/kernel -B $W/kc/BootKernelExtensions.kc -S $W/kc/SystemKernelExtensions.kc --repository $W/repo -x "${B[@]}" -A $W/out/aux.kc > $W/kmutil.log 2>&1; rc=$?
n=$(kmutil inspect -A $W/out/aux.kc -B $W/kc/BootKernelExtensions.kc -S $W/kc/SystemKernelExtensions.kc 2>/dev/null | grep -c -E "com\.(apple\.kext\.AMD|nullmoth\.NMXAudio)")
echo "kmutil rc $rc, $n of 8 inside"
[ "$rc" = 0 ] && [ "$n" = 8 ] || exit 1
K="$D27/Library/KernelCollections"; cp $W/out/aux.kc "$K/AuxiliaryKernelExtensions.kc" && chown root:wheel "$K/AuxiliaryKernelExtensions.kc" && chmod 644 "$K/AuxiliaryKernelExtensions.kc"
shasum -a 256 "$K/AuxiliaryKernelExtensions.kc" | cut -c1-8
