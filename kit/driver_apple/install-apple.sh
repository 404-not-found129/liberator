#!/bin/bash
# install-apple.sh - macOS 27.2 on a Mac Pro (Late 2013, MacPro6,1) with its AMD FirePro graphics, run from Tahoe on the
# same Mac (Golden Gate's finish step does that). macOS 27 has no driver for this GPU; Tahoe does. This package:
#  1. copies Tahoe's own AMD kernel drivers (7) into a private folder and its AMD user-space bundles (6) onto the
#     macOS 27 system volume. They are read from this Mac's running Tahoe and never shipped.
#  2. adds one load command to macOS 27's copy of libAMDIL902 (Tahoe's shader compiler) so it loads amdil_rtfix.dylib,
#     which lets it compile macOS 27's shaders. Only the exact 18,187,216-byte file is changed; the original is kept.
#  3. links the auxiliary kernel collection (the 7 AMD drivers + NMXAudio, the audio driver) against the laid-down
#     macOS 27.2 boot and system collections. kmutil must run here: macOS 27's own refuses this pair.
#  4. installs three helpers on macOS 27: RenderBox on its CPU renderer (this GPU has no framebuffer fetch), the
#     internal speakers as the default output, and a DHCP request for wired ports that come up without an address.
#
#   sudo bash install-apple.sh --payload <this folder> --system "/Volumes/<name>" --data "/Volumes/<name> - Data"
#        [--esp <partition>] [--dry-run | --apply | --uninstall]
#
# No mode word is a dry run. Code 0 done or clean dry run, 1 refused or failed, 2 unknown option.
# A file it adds is listed in added.list under <data>/Library/NullMoth/golden-gate/apple; a file it replaces is kept
# first under .../apple/orig/<path>. --uninstall removes the added files and puts the kept ones back.
set -u
PATH=/usr/bin:/bin:/usr/sbin:/sbin; export PATH
HERE=$(cd "$(dirname "$0")" && pwd)
PAYLOAD=$HERE; SYS=""; DATA=""; MODE="--dry-run"
# test-hook-begin
KMUTIL=/usr/bin/kmutil
TAHOE_EXT=""                           # one folder holding Tahoe's AMD drivers and bundles, in place of the search
MODEL=""                               # the Mac's model, in place of reading it
# test-hook-end
while [ $# -gt 0 ]; do case "$1" in
  --payload) PAYLOAD=${2:-}; shift;; --system) SYS=${2:-}; shift;; --data) DATA=${2:-}; shift;; --esp) shift;;
  --dry-run|--apply|--uninstall) MODE=$1;;
  # test-hook-begin
  --kmutil) KMUTIL=${2:-}; shift;; --tahoe-ext) TAHOE_EXT=${2:-}; shift;; --model) MODEL=${2:-}; shift;;
  # test-hook-end
  *) echo "STOP unknown option $1"; exit 2;; esac; shift; done
say() { echo "  $*"; }
die() { echo "STOP $*"; exit 1; }

[ -n "$SYS" ] && [ -d "$SYS/System/Library" ] || die "--system must name the mounted macOS 27 system volume"
[ -n "$DATA" ] && [ -d "$DATA" ] || die "--data must name the mounted macOS 27 data volume"
STATE="$DATA/Library/NullMoth/golden-gate/apple"; ADDED="$STATE/added.list"; KEPT="$STATE/orig"
AUX="$DATA/Library/KernelCollections/AuxiliaryKernelExtensions.kc"
BOOTKC="$SYS/System/Library/KernelCollections/BootKernelExtensions.kc"
SYSKC="$SYS/System/Library/KernelCollections/SystemKernelExtensions.kc"
KERNEL="$SYS/System/Library/Kernels/kernel"
P="$PAYLOAD/payload"; MAN="$PAYLOAD/MANIFEST"
AMDKEXTS="AMDSupport AMD7000Controller AMDFramebuffer AMDRadeonX4000 AMDRadeonX4000HWServices AMDRadeonX4030HWLibs AMDRadeonServiceManager"
AMDBUNDLES="AMDMTLBronzeDriver AMDShared AMDRadeonX4000GLDriver AMDRadeonVADriver AMDRadeonVADriver2 AMDRawCounterPlugin"
IL=System/Library/Extensions/AMDShared.bundle/Contents/PlugIns/libAMDIL902.dylib
IL_SIZE=18187216
HOOK=/usr/local/lib/nullmoth/amdil_rtfix.dylib

rel() { case "$1" in "$SYS"/*) echo "S:${1#"$SYS"}";; *) echo "D:${1#"$DATA"}";; esac; }
abs() { case "$1" in S:*) echo "$SYS${1#S:}";; D:*) echo "$DATA${1#D:}";; esac; }

if [ "$MODE" = --uninstall ]; then
  echo "STEP removing the Mac Pro package"
  [ -f "$ADDED" ] || { say "nothing was installed by this package"; exit 0; }
  while IFS= read -r r; do [ -n "$r" ] && rm -rf "$(abs "$r")"; done < "$ADDED"
  if [ -d "$KEPT" ]; then (cd "$KEPT" && find . -type f -o -type l) | while IFS= read -r f; do
    f=${f#./}; t=$(abs "${f%%/*}:/${f#*/}"); mkdir -p "${t%/*}" && mv -f "$KEPT/$f" "$t"; done; fi
  rm -rf "$STATE"; say "removed; restart to finish"; exit 0
fi

echo "STEP checking this Mac and the package"
[ -n "$MODEL" ] || MODEL=$(ioreg -rd1 -c IOPlatformExpertDevice 2>/dev/null | awk -F'"' '/"board-id"/{print $4}')
[ -n "$MODEL" ] || MODEL=$(sysctl -n hw.model 2>/dev/null)
case "$MODEL" in Mac-F60DEB81FF30ACF6|MacPro6,1) say "Mac Pro (Late 2013)";;
  *) die "this package is for the Mac Pro (Late 2013) only; this Mac reads as '$MODEL'. Nothing was changed";; esac
[ -f "$MAN" ] || die "the package has no MANIFEST. Nothing was changed"
while read -r kind mode sum path; do
  case "$kind" in F) [ "$(md5 -q "$P$path" 2>/dev/null)" = "$sum" ] || die "$path does not match the package's MANIFEST. Nothing was changed";; esac
done < <(grep -v '^#' "$MAN")
say "package files match MANIFEST"
# Apple's own copies are in /System/Library/Extensions; a root patch that restored this GPU's drivers may have put them
# in /Library/Extensions instead. Each one is taken from the first of the two that has it.
where() { local d; for d in ${TAHOE_EXT:-/System/Library/Extensions /Library/Extensions}; do [ -d "$d/$1" ] && { echo "$d/$1"; return 0; }; done; return 1; }
for k in $AMDKEXTS; do where "$k.kext" >/dev/null || die "Tahoe has no $k.kext: start this Mac's Tahoe with its AMD graphics working first. Nothing was changed"; done
for b in $AMDBUNDLES; do where "$b.bundle" >/dev/null || die "Tahoe has no $b.bundle. Nothing was changed"; done
say "Tahoe's 7 AMD drivers and 6 AMD bundles are present"
for f in "$KERNEL" "$BOOTKC" "$SYSKC"; do [ -f "$f" ] || die "the macOS 27 volume has no ${f##*/}: lay down macOS 27 and its kernel collections first"; done
[ "$MODE" = --apply ] || { echo "dry run finished; nothing was changed. Run again with --apply to install."; exit 0; }

WORK=$(mktemp -d /private/var/tmp/gg-apple.XXXXXX) || die "no private work folder"
trap 'rm -rf "$WORK"' EXIT
echo "STEP linking the auxiliary kernel collection"
mkdir "$WORK/repo"
for k in $AMDKEXTS; do cp -RX "$(where "$k.kext")" "$WORK/repo/" || die "could not copy $k.kext"; say "$k.kext from $(dirname "$(where "$k.kext")")"; done
cp -RX "$P/Library/Extensions/NMXAudio.kext" "$WORK/repo/" || die "could not copy NMXAudio.kext"
chown -R root:wheel "$WORK/repo"; chmod -R go-w "$WORK/repo"
IDS="com.nullmoth.NMXAudio"; for k in $AMDKEXTS; do IDS="$IDS com.apple.kext.$k"; done
args=""; for id in $IDS; do args="$args -b $id"; done
# shellcheck disable=SC2086
"$KMUTIL" create -n aux -a x86_64 --allow-missing-kdk --build 26B123 -z --no-authentication --kernel "$KERNEL" \
  -B "$BOOTKC" -S "$SYSKC" --repository "$WORK/repo" -x $args -A "$WORK/aux.kc" > "$WORK/kmutil.log" 2>&1 \
  || { grep -v '^$' "$WORK/kmutil.log" | tail -12; die "the drivers did not link against the macOS 27.2 collections. Nothing was changed"; }
[ -f "$WORK/aux.kc" ] || die "kmutil wrote no collection. Nothing was changed"
inside=$("$KMUTIL" inspect -A "$WORK/aux.kc" -B "$BOOTKC" -S "$SYSKC" 2>/dev/null)
for id in $IDS; do printf '%s\n' "$inside" | grep -qF "$id" || die "linked, but $id is not in the collection. Nothing was changed"; done
say "linked: $(stat -f %z "$WORK/aux.kc") bytes, 8 extensions"

echo "STEP preparing the shader compiler"
src="$(where AMDShared.bundle)/Contents/PlugIns/libAMDIL902.dylib"
[ "$(stat -f %z "$src")" = "$IL_SIZE" ] || die "Tahoe's libAMDIL902 is not the expected build ($IL_SIZE bytes). Nothing was changed"
python3 "$PAYLOAD/addlc.py" "$src" "$WORK/libAMDIL902.dylib" "$HOOK" >/dev/null || die "could not add the shader hook. Nothing was changed"
codesign -f -s - "$WORK/libAMDIL902.dylib" >/dev/null 2>&1 || die "could not sign the patched shader compiler. Nothing was changed"

echo "STEP placing"
if mount | grep -F " on $SYS (" | grep -q 'read-only'; then
  /sbin/mount -uw "$SYS" 2>/dev/null
  mount | grep -F " on $SYS (" | grep -q 'read-only' && die "the macOS 27 system volume is read-only and could not be made writable"
fi
mkdir -p "$STATE" "$KEPT" || die "could not make $STATE"; touch "$ADDED"
keep() { local r k; [ -e "$1" ] || [ -L "$1" ] || return 0; r=$(rel "$1"); k="$KEPT/${r%%:*}${r#*:}"
  [ -e "$k" ] && return 0; grep -qxF "$r" "$ADDED" && return 0; mkdir -p "${k%/*}" && mv "$1" "$k" || die "could not keep $1"; }
put() {   # $1 = source (file or folder), $2 = destination, $3 = mode for a file
  keep "$2"; mkdir -p "${2%/*}" || die "could not make ${2%/*}"; rm -rf "$2.gg-new"
  if [ -d "$1" ]; then cp -RX "$1" "$2.gg-new" && chown -R root:wheel "$2.gg-new"; chmod -R go-w "$2.gg-new"
  else cp -X "$1" "$2.gg-new" && chown root:wheel "$2.gg-new"; chmod "$3" "$2.gg-new"; fi
  rm -rf "$2" && mv -f "$2.gg-new" "$2" || die "could not place $2"
  grep -qxF "$(rel "$2")" "$ADDED" || rel "$2" >> "$ADDED"; }
for b in $AMDBUNDLES; do put "$(where "$b.bundle")" "$SYS/System/Library/Extensions/$b.bundle" 755; done
put "$WORK/libAMDIL902.dylib" "$SYS/$IL" 755
put "$P$HOOK" "$DATA$HOOK" 755
put "$P/Library/Extensions/NMXAudio.kext" "$DATA/Library/Extensions/NMXAudio.kext" 755
for f in usr/local/libexec/nullmoth/audsel usr/local/libexec/nullmoth/netup.sh; do put "$P/$f" "$DATA/$f" 755; done
for f in "$P"/Library/LaunchDaemons/*.plist "$P"/Library/LaunchAgents/*.plist; do put "$f" "$DATA${f#"$P"}" 644; done
put "$WORK/aux.kc" "$AUX" 644
say "installed; restart into macOS 27 to finish"
