#!/bin/bash
# build.sh - build out/Sandbox.kext (com.apple.security.sandbox, permissive) for the macOS 27 x86_64 kernel.
set -euo pipefail
A=$(cd "$(dirname "$0")" && pwd); cd "$A"
K="$(xcrun --show-sdk-path)/System/Library/Frameworks/Kernel.framework/Headers"
CC=(xcrun clang -arch x86_64 -mkernel -nostdinc -O2 -DKERNEL -I"$K" -Wall -Wno-unused-parameter -fno-stack-protector)
mkdir -p kobj out
cat > kobj/info.c <<EOI
#include <mach/mach_types.h>
extern kern_return_t a2x_sandbox_start(kmod_info_t *ki, void *data);
extern kern_return_t a2x_sandbox_stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.apple.security.sandbox, "300.0", a2x_sandbox_start, a2x_sandbox_stop)
__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
EOI
"${CC[@]}" -c kobj/info.c -o kobj/info.o
"${CC[@]}" -c sandbox.c -o kobj/sandbox.o
B=out/Sandbox.kext
rm -rf "$B"; mkdir -p "$B/Contents/MacOS"
xcrun clang -arch x86_64 -Xlinker -kext -nostdlib -Xlinker -export_dynamic -Xlinker -undefined -Xlinker dynamic_lookup \
    kobj/info.o kobj/sandbox.o -lkmod -lcc_kext -o "$B/Contents/MacOS/Sandbox"
cat > "$B/Contents/Info.plist" <<EOI
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>AppleSecurityExtension</key><true/>
	<key>CFBundleDevelopmentRegion</key><string>English</string>
	<key>CFBundleExecutable</key><string>Sandbox</string>
	<key>CFBundleIdentifier</key><string>com.apple.security.sandbox</string>
	<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
	<key>CFBundleName</key><string>Seatbelt sandbox policy</string>
	<key>CFBundlePackageType</key><string>KEXT</string>
	<key>CFBundleShortVersionString</key><string>300.0</string>
	<key>CFBundleVersion</key><string>300.0</string>
	<key>OSBundleCompatibleVersion</key><string>1.0.0d1</string>
	<key>OSBundleRequired</key><string>Root</string>
	<key>OSBundleLibraries</key>
	<dict>
		<key>com.apple.kpi.bsd</key><string>10.0.0d3</string>
		<key>com.apple.kpi.iokit</key><string>10.0.0d3</string>
		<key>com.apple.kpi.dsep</key><string>10.0.0d3</string>
		<key>com.apple.kpi.libkern</key><string>10.0.0d3</string>
		<key>com.apple.kpi.mach</key><string>10.0.0d3</string>
	</dict>
</dict>
</plist>
EOI
plutil -lint "$B/Contents/Info.plist"
echo "undefined:"; nm -u "$B/Contents/MacOS/Sandbox"
echo "exported sandbox symbols: $(nm -gU "$B/Contents/MacOS/Sandbox" | grep -cE ' _(sandbox_|SANDBOX_)')"
md5 -q "$B/Contents/MacOS/Sandbox"
