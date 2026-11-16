#!/bin/bash
# install-igpu.sh - macOS 27.2 on a PC whose only graphics is a built-in AMD or Intel GPU (the kit's "test" graphics
# path). Puts the AMD/Intel driver built for macOS 27 (NMXGPU, and with --accel its Metal accelerator NMXAccel and the
# Vulkan drivers the Metal plugin runs on it) on the macOS 27 volumes, with the GPU firmware the driver loads, and links
# the auxiliary kernel collection on this machine against the macOS 27.2 boot and system collections that were laid
# down.
#
#   sudo bash install-igpu.sh --payload <this folder> --system "/Volumes/<name>" --data "/Volumes/<name> - Data"
#        [--esp <partition>] [--accel] [--dry-run | --apply | --uninstall]
#
# Run from macOS 15 or 26 (Golden Gate's finish step does that). No mode word is a dry run. --esp is accepted and not
# used. Code 0 done or clean dry run, 1 refused or failed, 2 unknown option.
#
# Staging, and why --accel is off by default: on macOS 27.2 a window server that finds an accelerator without display
# pipe capabilities picks the software compositor with no destination and the session dies (measured with the NVIDIA
# driver). The first start uses NMXGPU alone: its framebuffer, composited in software. --accel is for the step after
# that, and only together with boot-arg nmxfb_pipe=1: NMXAccel then gives NMXGPU's framebuffer a display pipe, keeps the
# Metal plugin name back until the window server's start gate has armed it, and the gate turns CoreDisplay's switch for
# the vendor on (or off, when the arm fails). Without the boot-arg NMXAccel is a Metal device for apps only.
#
# Rules it keeps (the same as the kit's other installers):
#  - Every payload file is checked against MANIFEST before anything is touched.
#  - A file it adds is listed in added.list under <data>/Library/NullMoth/golden-gate/igpu; a file it replaces is kept
#    first under .../igpu/orig/<path>. --uninstall removes the added files and puts the kept ones back.
#  - The collection is linked in a private folder and placed only when kmutil linked it and every wanted extension is
#    in it. The firmware is placed only when every file matches the list. A failure changes nothing on the volumes.
set -u
PATH=/usr/bin:/bin:/usr/sbin:/sbin; export PATH
HERE=$(cd "$(dirname "$0")" && pwd)
PAYLOAD=$HERE; SYS=""; DATA=""; MODE="--dry-run"; ACCEL=0
WANT_BUILD=26B123
# test-hook-begin
KMUTIL=/usr/bin/kmutil
VENDORS=""           # "1002 8086": the vendors of this machine's display devices, in place of reading the registry
FWFROM=""            # a folder laid out like linux-firmware, in place of the running system's copy and the network
NOFETCH=0            # 1: never use the network (tests)
# test-hook-end
while [ $# -gt 0 ]; do case "$1" in
  --payload) PAYLOAD=${2:-}; shift;; --system) SYS=${2:-}; shift;; --data) DATA=${2:-}; shift;; --esp) shift;;
  --accel) ACCEL=1;;
  --dry-run|--apply|--uninstall) MODE=$1;;
  # test-hook-begin
  --kmutil) KMUTIL=${2:-}; shift;; --vendors) VENDORS=${2:-}; shift;; --fw-from) FWFROM=${2:-}; shift;; --no-fetch) NOFETCH=1;;
  # test-hook-end
  *) echo "STOP unknown option $1"; exit 2;; esac; shift; done
say() { echo "  $*"; }
die() { echo "STOP $*"; exit 1; }

[ -n "$SYS" ] && [ -d "$SYS/System/Library" ] || die "--system must name the mounted macOS 27 system volume"
[ -n "$DATA" ] && [ -d "$DATA" ] || die "--data must name the mounted macOS 27 data volume"
STATE="$DATA/Library/NullMoth/golden-gate/igpu"; ADDED="$STATE/added.list"; KEPT="$STATE/orig"
AUX="$DATA/Library/KernelCollections/AuxiliaryKernelExtensions.kc"
PATHFILE="$DATA/Library/NullMoth/bringup/graphics-path"
BOOTKC="$SYS/System/Library/KernelCollections/BootKernelExtensions.kc"
SYSKC="$SYS/System/Library/KernelCollections/SystemKernelExtensions.kc"
KERNEL="$SYS/System/Library/Kernels/kernel"
FW="$DATA/Library/NullMoth/firmware"
P="$PAYLOAD/payload"; MAN="$PAYLOAD/MANIFEST"; FWMAN="$PAYLOAD/gpu-firmware.txt"

# added.list holds paths relative to the volume they are on, prefixed S: or D:, so a list made with the volumes mounted
# at one place still undoes the right files when they are mounted somewhere else
vol() { case "$1" in S:*) printf '%s%s' "$SYS" "${1#S:}";; D:*) printf '%s%s' "$DATA" "${1#D:}";; esac; }
rec() { local r; case "$1" in "$SYS"/*) r="S:${1#"$SYS"}";; "$DATA"/*) r="D:${1#"$DATA"}";; *) return 1;; esac
        mkdir -p "$STATE" && touch "$ADDED" && { grep -qxF "$r" "$ADDED" || echo "$r" >> "$ADDED"; }; }

if [ "$MODE" = "--uninstall" ]; then
  [ -f "$ADDED" ] || { echo "nothing recorded as installed"; echo "RESULT ok"; exit 0; }
  if mount | grep -F " on $SYS (" | grep -q 'read-only'; then /sbin/mount -uw "$SYS" 2>/dev/null; fi
  while IFS= read -r r; do [ -n "$r" ] || continue
    f=$(vol "$r") || continue; [ -n "$f" ] || continue
    rm -rf "$f" && say "removed $f"
    case "$r" in S:*) k="$KEPT/S${r#S:}";; *) k="$KEPT/D${r#D:}";; esac
    if [ -e "$k" ]; then mkdir -p "${f%/*}" && mv "$k" "$f" && say "put back $f"; fi
  done < "$ADDED"
  rm -f "$ADDED"; echo "RESULT ok"; exit 0
fi

echo "STEP the payload"
[ -f "$MAN" ] && [ -d "$P" ] && [ -f "$FWMAN" ] || die "the payload is incomplete (MANIFEST, payload/, gpu-firmware.txt)"
bad=0; n=0
while read -r kind mode sum path; do
  case "$kind" in F) ;; *) continue;; esac
  n=$((n + 1))
  [ -f "$P$path" ] || { echo "  missing $path"; bad=1; continue; }
  [ "$(md5 -q "$P$path")" = "$sum" ] || { echo "  changed $path"; bad=1; }
done < "$MAN"
[ "$bad" = 0 ] && [ "$n" -gt 0 ] || die "the payload does not match MANIFEST ($n files listed)"
say "$n files match MANIFEST"
[ -d "$P/Library/Extensions/NMXGPU.kext" ] || die "the payload has no NMXGPU.kext"
if [ "$ACCEL" = 1 ]; then [ -d "$P/Library/Extensions/NMXAccel.kext" ] || die "--accel, but the payload has no NMXAccel.kext"; fi

echo "STEP the macOS 27 volumes"
b=$(/usr/libexec/PlistBuddy -c 'Print :ProductBuildVersion' "$SYS/System/Library/CoreServices/SystemVersion.plist" 2>/dev/null)
[ "$b" = "$WANT_BUILD" ] || die "the system volume holds build ${b:-?}; this package is for $WANT_BUILD"
for f in "$BOOTKC" "$SYSKC" "$KERNEL"; do [ -f "$f" ] || die "not found: $f (macOS 27 must be laid down first)"; done
LOADER="$PAYLOAD/../driver/payload/usr/libexec/auxkcload"
LJOB="$PAYLOAD/../driver/payload/System/Library/LaunchDaemons/com.apple.kernelmanagement.auxkcload.plist"
[ -f "$LOADER" ] && [ -f "$LJOB" ] || die "the kit's collection loader is not beside this package (driver/payload)"
# NMXAudio rides in the same collection: on an iGPU-only PC it is the sound driver as much as on an NVIDIA one
AUDIO="$PAYLOAD/../driver/payload/Library/Extensions/NMXAudio.kext"
say "boot and system collections, kernel and loader are present"

echo "STEP this machine's graphics"
if [ -z "$VENDORS" ]; then
  VENDORS=$(ioreg -r -c IOPCIDevice -l -w0 2>/dev/null | LC_ALL=C awk -F'<|>' '
    function out() { if (c == "03" && (v == "0210" || v == "8680")) print (v == "0210" ? "1002" : "8086") }
    /\+-o / { out(); c = ""; v = "" }
    /"vendor-id" =/ { v = substr($2, 1, 4) } /"class-code" =/ { c = substr($2, 5, 2) }
    END { out() }' | sort -u | tr '\n' ' ')
fi
VENDORS=$(printf '%s' "$VENDORS" | tr -s ' ')
[ -n "${VENDORS// /}" ] || die "no AMD or Intel display device found; this package is for a PC with built-in AMD or Intel graphics"
say "AMD/Intel display vendors: $VENDORS"
FWDIRS="LICENSES/"
for v in $VENDORS; do case $v in 1002) FWDIRS="$FWDIRS amdgpu/";; 8086) FWDIRS="$FWDIRS i915/ xe/";; esac; done
FWLIST=$(grep -v '^#' "$FWMAN" | awk -v dirs="$FWDIRS" 'BEGIN{n=split(dirs,d," ")} {for(i=1;i<=n;i++) if (index($2,d[i])==1) {print $1 "  " $2; next}}')
[ -n "$FWLIST" ] || die "the firmware list has nothing for $VENDORS"
COMMIT=$(sed -n 1p "$FWMAN" | grep -oE '[0-9a-f]{40}' | head -1)
[ -n "$COMMIT" ] || die "the firmware list names no linux-firmware commit"
say "firmware: $(printf '%s\n' "$FWLIST" | grep -c .) files from linux-firmware $COMMIT"

KEXTS="$P/Library/Extensions/NMXGPU.kext"; IDS="com.nullmoth.NMXGPU"
[ "$ACCEL" = 1 ] && { KEXTS="$KEXTS $P/Library/Extensions/NMXAccel.kext"; IDS="$IDS com.nullmoth.NMXAccel"; }
[ -d "$AUDIO" ] && { KEXTS="$KEXTS $AUDIO"; IDS="$IDS com.nullmoth.NMXAudio"; }

if [ "$MODE" != "--apply" ]; then
  say "dry run: would place the firmware, $(echo $IDS | wc -w | tr -d ' ') extensions ($IDS) linked into $AUX$([ "$ACCEL" = 1 ] && echo ", the Vulkan drivers"), and the graphics path (test)"
  echo "RESULT ok"; exit 0
fi

WORK=$(mktemp -d -t gg-igpu) || die "could not make a work folder"
trap 'rm -rf "$WORK"' EXIT

echo "STEP the GPU firmware"
printf '%s\n' "$FWLIST" > "$WORK/fw.list"
mkdir "$WORK/fw"
fill_from() {   # $1 = a folder laid out like linux-firmware: copy the listed files that are there
  awk '{print $2}' "$WORK/fw.list" | while IFS= read -r p; do
    [ -f "$1/$p" ] && [ ! -f "$WORK/fw/$p" ] && mkdir -p "$(dirname "$WORK/fw/$p")" && cp -X "$1/$p" "$WORK/fw/$p"; done; return 0; }
[ -n "$FWFROM" ] && fill_from "$FWFROM"
# the running macOS's own copy, if the 2.x test driver put one there (it came from the same list)
[ -d /Library/NullMoth/firmware ] && [ -z "$FWFROM" ] && fill_from /Library/NullMoth/firmware
if [ "$NOFETCH" = 0 ]; then
  awk '{print $2}' "$WORK/fw.list" | while IFS= read -r p; do [ -f "$WORK/fw/$p" ] || printf '%s\n' "$p"; done > "$WORK/fw.need"
  if [ -s "$WORK/fw.need" ]; then
    say "fetching $(wc -l < "$WORK/fw.need" | tr -d ' ') files"
    while IFS= read -r p; do mkdir -p "$(dirname "$WORK/fw/$p")"
      curl -fsL --retry 5 --retry-delay 2 -o "$WORK/fw/$p" "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$COMMIT/$p" ||
        curl -fsSL --retry 5 --retry-delay 2 -o "$WORK/fw/$p" "https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/$p?id=$COMMIT" ||
        { rm -f "$WORK/fw/$p"; echo "  could not fetch $p"; }
    done < "$WORK/fw.need"
  fi
fi
(cd "$WORK/fw" && shasum -a 256 --strict -c --quiet < "$WORK/fw.list") >/dev/null 2>&1 \
  || die "the GPU firmware is incomplete or a file does not match its checksum. Nothing was changed"
grep '^#link ' "$FWMAN" | while read -r _ n _ tgt; do
  case "$n/$tgt" in *..*|/*|*//*) continue;; esac; case "$tgt" in */*) continue;; esac
  [ -f "$WORK/fw/$(dirname "$n")/$tgt" ] && ln -sfh "$tgt" "$WORK/fw/$n"; done
say "firmware checked: $(wc -l < "$WORK/fw.list" | tr -d ' ') files"

echo "STEP linking the auxiliary kernel collection"
mkdir "$WORK/repo"; args=""
for k in $KEXTS; do cp -RX "$k" "$WORK/repo/" || die "could not copy ${k##*/}"; done
chown -R root:wheel "$WORK/repo" 2>/dev/null; chmod -R go-w "$WORK/repo"
for id in $IDS; do args="$args -b $id"; done
# shellcheck disable=SC2086
"$KMUTIL" create -n aux --allow-missing-kdk --kernel "$KERNEL" -B "$BOOTKC" -S "$SYSKC" --repository "$WORK/repo" --explicit-only $args -A "$WORK/aux.kc" > "$WORK/kmutil.log" 2>&1 \
  || { grep -v '^$' "$WORK/kmutil.log" | tail -12; die "the extensions did not link against the macOS 27.2 collections. Nothing was changed"; }
# kmutil leaves out an extension it cannot link and still succeeds, so the collection is read back
[ -f "$WORK/aux.kc" ] || die "kmutil wrote no collection. Nothing was changed"
inside=$("$KMUTIL" inspect -A "$WORK/aux.kc" 2>/dev/null | awk '{print $1}')
for id in $IDS; do printf '%s\n' "$inside" | grep -qxF "$id" || die "linked, but $id is not in the collection. Nothing was changed"; done
say "linked: $(stat -f %z "$WORK/aux.kc") bytes, $(echo $IDS | wc -w | tr -d ' ') extensions"

echo "STEP placing"
if mount | grep -F " on $SYS (" | grep -q 'read-only'; then
  /sbin/mount -uw "$SYS" 2>/dev/null
  mount | grep -F " on $SYS (" | grep -q 'read-only' && die "the macOS 27 system volume is read-only and could not be made writable"
fi
mkdir -p "$STATE" "$KEPT" || die "could not make $STATE"; touch "$ADDED"
keep() {  # $1 = a path about to be replaced: kept once, the first time only
  local r k; [ -e "$1" ] || [ -L "$1" ] || return 0
  case "$1" in "$SYS"/*) r="S${1#"$SYS"}";; *) r="D${1#"$DATA"}";; esac; k="$KEPT/$r"
  [ -e "$k" ] && return 0
  # a path an earlier apply placed is this package's own: replaced, never kept as if it were the user's
  grep -qxF "${r%%/*}:/${r#*/}" "$ADDED" 2>/dev/null && return 0
  mkdir -p "${k%/*}" && mv "$1" "$k" || die "could not keep $1"
}
put() {   # $1 = source (file or folder), $2 = destination, $3 = mode for a file
  keep "$2"; mkdir -p "${2%/*}" || die "could not make ${2%/*}"
  rm -rf "$2.gg-new"
  if [ -d "$1" ]; then cp -RX "$1" "$2.gg-new" && chown -R root:wheel "$2.gg-new" 2>/dev/null; chmod -R go-w "$2.gg-new"
  else cp -X "$1" "$2.gg-new" && chown root:wheel "$2.gg-new" 2>/dev/null; chmod "$3" "$2.gg-new"; fi
  rm -rf "$2" && mv -f "$2.gg-new" "$2" || die "could not place $2"
  rec "$2"
}
put "$WORK/fw" "$FW" 755
for k in $KEXTS; do case "$k" in "$P"/*) put "$k" "$DATA/Library/Extensions/${k##*/}" 755;; esac; done
put "$WORK/aux.kc" "$AUX" 744
put "$LOADER" "$SYS/usr/libexec/auxkcload" 755
put "$LJOB" "$SYS/System/Library/LaunchDaemons/com.apple.kernelmanagement.auxkcload.plist" 644
if [ "$ACCEL" = 1 ]; then
  for f in "$P"/Library/GPUBundles/nvmtl/*; do put "$f" "$DATA/Library/GPUBundles/nvmtl/${f##*/}" 755; done
  DB="$PAYLOAD/../driver/payload/Library/GPUBundles/NVMTLDriver.bundle"
  [ -d "$DB" ] && put "$DB" "$DATA/Library/GPUBundles/NVMTLDriver.bundle" 755
fi
SKIP="$DATA/Library/NullMoth/bringup/explicit-load.skip"
[ -e "$SKIP" ] || { mkdir -p "${SKIP%/*}" && : > "$SKIP" && chmod 644 "$SKIP" && rec "$SKIP"; } || die "could not make the loader's skip list"
printf 'test\n' > "$WORK/gp" && put "$WORK/gp" "$PATHFILE" 644
say "placed: firmware, $(echo $KEXTS | wc -w | tr -d ' ') extensions, the collection, its loader and launch job$([ "$ACCEL" = 1 ] && echo ", the Vulkan drivers and the Metal plugin"), and the graphics path (test)"
echo "RESULT ok"
