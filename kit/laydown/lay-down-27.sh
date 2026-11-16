#!/bin/bash
# lay-down-27.sh - put macOS 27.2 (build 26B123) on its own APFS volume group, from macOS 15 or 26, on an Intel machine.
#
# It ends with code 0 (done, or a clean dry run), 1 (refused or failed), 2 (unknown option) or 3: the running macOS
# lacks something this needs. Code 3 comes with one line starting "SYSTEM:" and only before anything is written.
#
# Why this exists: Apple's macOS 27 installer cannot be used on Intel. Its device list holds only Apple-silicon
# boards, its payload carries no Intel install environment, and its front-end programs are arm64-only. What Apple
# does publish for this exact build is the restore image (a zip on Apple's download server). Inside it the OS is
# one disk image holding one APFS volume, and the Intel dyld cache is in a second, unencrypted image. This script
# downloads that file from Apple, opens it with the tools macOS already has (curl, unzip, openssl, aea,
# hdiutil, ditto, diskutil) and builds the volume group the way a macOS install is laid out. The one program it
# brings is cache4k (next to this script, source cache4k.c), which rewrites the dyld cache's signature for 4 KB
# pages. Nothing of Apple's is carried with this script: every Apple file comes from Apple's server at run time.
#
#   sudo bash lay-down-27.sh                 dry run: checks everything it can, changes nothing
#   sudo bash lay-down-27.sh --apply         do it
#
# options
#   --name NAME                volume name (default "macOS 27"); the data volume is "NAME - Data"
#   --work DIR                 where the download and the unpacked images go (default /Library/Application Support/1401/golden-gate/laydown)
#   --ipsw FILE                a restore image that is already downloaded (it is verified, never trusted)
#   --container diskN          APFS container to add the volumes to (default: the one the running macOS is on)
#   --kernel-collections DIR   folder with BootKernelExtensions.kc and SystemKernelExtensions.kc for Intel, as
#                              make-kernel-collections.sh leaves them. Apple ships none for macOS 27; without
#                              them the volume is laid down but cannot start
# The work folder and the kernel-collections folder are used by a root script, so each must belong to root, with
# no folder in its path writable by anyone else and no symbolic link in it.
#   --user NAME                account to carry over from this macOS (default: the user who ran sudo);
#                              --no-user leaves the new system at Setup Assistant
#   --enable-ssh               turn Remote Login on in the new system (for a machine without a screen)
#   --keep-work                keep the unpacked images after a successful run
#
# What it changes, and nothing else:
#   - adds two new volumes (data, then system grouped with it) to the container. Existing volumes are never
#     erased, renamed, resized or written.
#   - adds one new folder, named by the new group's identifier, to the container's Preboot volume. Every macOS
#     install does this. It also adds links to the dyld cache under Preboot's Cryptexes/Rosetta, and only where
#     no entry of that name exists. Nothing else on Preboot is opened.
# It does not touch the startup partition, the firmware boot choice, or the running system's volumes.
#
# What is still needed before the new volume starts (this script says so again at the end):
#   - the boot loader configuration for macOS 27 (the Golden Gate stage/activate steps)
#   - Intel kernel collections, when --kernel-collections was not given
#   - the kit's system and driver installers (the Golden Gate finish step)
set -u
PATH=/usr/bin:/bin:/usr/sbin:/sbin; export PATH

BUILD=26B123
VERSION=27.2
IPSW_URL="https://updates.cdn-apple.com/2026FallFCS/c9671975-52db-4fb5-9af2-00217169ba8d/UniversalMac_27.2_26B123_Restore.ipsw"
IPSW_SIZE=25595669503
IPSW_SHA256=6613b34e8589db2202a01ec37a2648afb141ece13428175183f8f3e38438534f
# Measured: 25 GB right after an install (system 23.1, data 2.0) and 32 GB on the test PC after days of use
# (22.9 + 9.1). 45 was asked for before either was measured.
NEED_CONTAINER_GB=35      # system 16 + dyld cache 7 + data template 1 + the kit, a first login and its caches
NEED_OSDMG_GB=16; NEED_ROS_GB=7; NEED_IPSW_GB=26
# for the progress lines: the sizes of the two unpacked images, and how long each phase takes (seconds; downloads
# at 30 MB a second; verify, unpack and copy as measured on a whole run on an Intel Mac with a solid-state disk,
# the last three estimated)
OSDMG_BYTES=14289993728; ROS_BYTES=6484393984
T_VERIFY=90; T_UNPACK=60; T_COPY=390; T_CACHE=40; T_DATA=15; T_PREBOOT=15

MODE=dry; NAME="macOS 27"; WORK="/Library/Application Support/1401/golden-gate/laydown"; IPSW=""; CONT=""; KCDIR=""
HERE=$(cd "$(dirname "$0")" && pwd)
SEEDUSER=${SUDO_USER:-}; NOUSER=0; SSH=0; KEEP=0
while [ $# -gt 0 ]; do
  case "$1" in
    --apply) MODE=apply;;
    --dry-run) MODE=dry;;
    --name) NAME=${2:?}; shift;;
    --work) WORK=${2:?}; shift;;
    --ipsw) IPSW=${2:?}; shift;;
    --container) CONT=${2:?}; shift;;
    --kernel-collections) KCDIR=${2:?}; shift;;
    --data-quota-gb) QUOTA_GB=${2:?}; shift;;
    --user) SEEDUSER=${2:?}; shift;;
    --no-user) NOUSER=1;;
    --enable-ssh) SSH=1;;
    --keep-work) KEEP=1;;
    *) echo "STOP unknown option $1"; echo "RESULT stop"; exit 2;;
  esac
  shift
done
case "${QUOTA_GB:-20}" in *[!0-9]*|'') echo "STOP --data-quota-gb takes a whole number of GB"; echo "RESULT stop"; exit 2;; esac
[ "${QUOTA_GB:-20}" -ge 20 ] && [ "${QUOTA_GB:-20}" -le 4096 ] || { echo "STOP --data-quota-gb takes 20 to 4096"; echo "RESULT stop"; exit 2; }

step() { echo "STEP $*"; }
ok()   { echo "OK   $*"; }
note() { echo "NOTE $*"; }
# Progress for the program that shows this run, one line each, never in a dry run:
#   PLAN <phase>:<weight> ...                          once, the phases this run will go through; weights sum to 100
#   PROGRESS <phase> <percent> <done bytes> <total bytes>   percent 0-100 within the phase; bytes 0 when they say nothing
# While a long step runs, a watcher looks every 2 seconds at what the step has produced so far and prints a line when
# the percent has moved. It never says 100: pg_end does, when the step is over and was checked.
PG_WATCH=""
pg() { echo "PROGRESS $1 $2 $3 $4"; }
pg_now() {  # <kind> <what>: bytes so far. file: a growing file; tree: a folder; used: a mounted volume; dd: a dd's own count
  case "$1" in
    file) stat -f %z "$2" 2>/dev/null || echo 0;;
    tree) du -sk "$2" 2>/dev/null | awk '{print $1 * 1024}';;
    used) df -k "$2" 2>/dev/null | awk 'NR==2 {print $3 * 1024}';;
    dd)   kill -INFO "$(cat "$2.pid" 2>/dev/null)" 2>/dev/null; sleep 0.3; awk '/bytes transferred/ {v=$1} END {print v + 0}' "$2" 2>/dev/null;;
  esac
}
pg_watch() {  # <phase> <total bytes> <kind> <what> [<bytes to take off> [<percent already said>]]
  local phase=$1 total=$2 kind=$3 what=$4 base=${5:-0} from=${6:-}
  [ -n "$from" ] || { pg "$phase" 0 0 "$total"; from=0; }
  ( last=$from
    while sleep 2; do
      now=$(pg_now "$kind" "$what"); now=$(( ${now:-0} - base )); [ "$now" -ge 0 ] || now=0
      pct=$(( total > 0 ? now * 100 / total : 0 )); [ "$pct" -le 99 ] || pct=99
      [ "$pct" -le "$last" ] || { echo "PROGRESS $phase $pct $now $total"; last=$pct; }
    done ) &
  PG_WATCH=$!
}
# The watcher is stopped with KILL, which it cannot ignore or outlive: a plain kill could leave it running, and
# the wait below would then wait for ever.
pg_stop() { [ -z "$PG_WATCH" ] || { kill -KILL "$PG_WATCH" 2>/dev/null; wait "$PG_WATCH" 2>/dev/null; PG_WATCH=""; }; }
pg_end() { pg_stop; pg "$1" 100 "$2" "$2"; }
plan() {  # <phase>:<seconds> ...: the PLAN line; each phase's share of the time, at least 1, summing to 100
  printf '%s\n' "$@" | awk -F: '{n[NR] = $1; s[NR] = $2; t += $2; if (!big || $2 > s[big]) big = NR}
    END { for (i = 1; i <= NR; i++) { w[i] = int(s[i] * 100 / t); if (w[i] < 1) w[i] = 1; sum += w[i] }
          w[big] += 100 - sum; printf "PLAN"; for (i = 1; i <= NR; i++) printf " %s:%d", n[i], w[i]; printf "\n" }'
}
ATTACHED=""; MOUNTED=""
cleanup() {
  local m d
  pg_stop
  # one mount point per line: the work folder's path may hold spaces
  while IFS= read -r m; do [ -z "$m" ] || umount "$m" 2>/dev/null || diskutil unmount force "$m" >/dev/null 2>&1; done <<MOUNTS
$MOUNTED
MOUNTS
  for d in $ATTACHED; do hdiutil detach "$d" -quiet 2>/dev/null || hdiutil detach "$d" -force -quiet 2>/dev/null; done
}
CREATED=""
REC=""; DONE_MARK=""; LEFT=""; LEFT_USED_GB=0; LEFT_HOW=""; PREV=""
stop() {
  echo "STOP $*"
  cleanup
  # the volumes this run made are taken away again, so that the next run starts clean; only the ones in its record
  if [ -n "$CREATED" ]; then
    if remove_recorded; then echo "NOTE the volumes this run had made were removed again:$CREATED"
    else
      echo "NOTE volumes this run created and could not remove:$CREATED"
      echo "NOTE to remove them: for each one, sudo diskutil apfs deleteVolume <device>   (check the name first with diskutil info)"
    fi
  fi
  echo "RESULT stop"; exit 1
}
trap 'cleanup' EXIT
free_gb() { df -g "$1" 2>/dev/null | awk 'NR==2 {print $4}'; }
# A folder a root script reads from or writes to: it and every folder above it belong to root, none is writable by
# group or others (the sticky temporary folders are not accepted), and no part of the path is a symbolic link.
safe_dir() {
  local p=$1 q
  case "$p" in /*) :;; *) echo "$p is not a full path"; return 1;; esac
  q=$p
  while [ "$q" != "/" ] && [ -n "$q" ]; do
    [ ! -L "$q" ] || { echo "$q is a symbolic link"; return 1; }
    if [ -e "$q" ]; then
      [ -d "$q" ] || { echo "$q is not a folder"; return 1; }
      [ "$(stat -f %u "$q")" = 0 ] || { echo "$q does not belong to root"; return 1; }
      case "$(stat -f %Lp "$q")" in *[2367][0-7]|*[2367]) echo "$q is writable by group or others"; return 1;; esac
    fi
    q=$(dirname "$q")
  done
}
vols_of() { diskutil apfs list "$1" 2>/dev/null | awk '/APFS Volume Disk \(Role\)|\+-> Volume disk/ {for (i=1;i<=NF;i++) if ($i ~ /^disk[0-9]+s[0-9]+$/) print $i}' | sort -u; }
in_list() { echo "$1" | grep -qxF -- "$2"; }     # $1 = one device per line, $2 = a device
dinfo() { diskutil info "$1" 2>/dev/null | awk -F': *' -v k="$2" '$1 ~ "^ *"k"$" {print $2; exit}'; }

# The two volumes a run makes are written down in the work folder as it makes them (volumes.created: their devices
# and volume UUIDs), and volumes.complete is written when the run has finished. A run that stops takes its two
# volumes away again; a run that was cut off leaves the record, and the next run removes exactly those two first.
# Nothing is ever removed that is not in the record with a matching UUID - with one exception for a run of this
# script from before it kept a record, described at find_unfinished.
vol_by_uuid() {  # <volume UUID>: its device, when it is a volume of the target container with exactly that UUID
  local d; d=$(dinfo "$1" "Device Identifier")
  [ -n "$d" ] && in_list "$(vols_of "$CONT")" "$d" && [ "$(dinfo "$d" "Volume UUID")" = "$1" ] && echo "$d"
}
remove_one() {  # <volume UUID> <name it must have>: 0 when it is gone afterwards (or was not there)
  local d; d=$(vol_by_uuid "$1") || return 0
  [ "$(dinfo "$d" "Volume Name")" = "$2" ] || return 1
  diskutil unmount force "$d" >/dev/null 2>&1
  diskutil apfs deleteVolume "$d" >/dev/null 2>&1
  [ -z "$(vol_by_uuid "$1")" ]
}
remove_recorded() {  # removes the volumes the record names (system first), then the record and their Preboot folder
  local du su pm rc=0
  [ -f "$REC" ] && [ ! -L "$REC" ] || return 1
  du=$(awk '$1 == "data" {print $3}' "$REC"); su=$(awk '$1 == "system" {print $3}' "$REC")
  [ -z "$su" ] || remove_one "$su" "$NAME" || rc=1
  [ -z "$du" ] || remove_one "$du" "$NAME - Data" || rc=1
  if [ "$rc" = 0 ]; then
    pm=$(dinfo "${PREV:-none}" "Mount Point")
    case "$du" in ????????-????-????-????-????????????) [ -z "$pm" ] || [ ! -d "$pm/$du" ] || [ -L "$pm/$du" ] || rm -rf "$pm/$du";; esac
    rm -f "$REC" "$DONE_MARK"
  fi
  return $rc
}
# find_unfinished: sets LEFT to the devices of the two volumes an unfinished run of this script left in the target
# container, and LEFT_USED_GB to the space they hold. Two ways to know them:
#   by the record: volumes.created is there, volumes.complete is not, and a volume with a recorded UUID exists;
#   without a record (a run of this script from before it kept one): one volume named '<name>' and one named
#   '<name> - Data', the two in one volume group, no boot.efi on the system one (this script places it last), and
#   this script's own os.dmg.ok in the work folder. Anything short of all of that is not ours and is left alone.
find_unfinished() {
  local why du su dd sd n nd v mp
  LEFT=""; LEFT_USED_GB=0; LEFT_HOW=""
  [ -d "$WORK" ] && why=$(safe_dir "$WORK") || return 0
  if [ -e "$REC" ]; then
    [ -f "$REC" ] && [ ! -L "$REC" ] && [ ! -e "$DONE_MARK" ] || return 0
    [ "$(sed -n 's/^name //p' "$REC")" = "$NAME" ] || return 0
    du=$(awk '$1 == "data" {print $3}' "$REC"); su=$(awk '$1 == "system" {print $3}' "$REC")
    dd=""; sd=""
    [ -z "$du" ] || dd=$(vol_by_uuid "$du"); [ -z "$su" ] || sd=$(vol_by_uuid "$su")
    [ -z "$dd" ] || [ "$(dinfo "$dd" "Volume Name")" = "$NAME - Data" ] || return 0
    [ -z "$sd" ] || [ "$(dinfo "$sd" "Volume Name")" = "$NAME" ] || return 0
    LEFT_HOW=record
  else
    [ -f "$WORK/os.dmg.ok" ] && [ ! -L "$WORK/os.dmg.ok" ] || return 0
    n=0; nd=0; dd=""; sd=""
    for v in $(vols_of "$CONT"); do
      case "$(dinfo "$v" "Volume Name")" in "$NAME") n=$((n + 1)); sd=$v;; "$NAME - Data") nd=$((nd + 1)); dd=$v;; esac
    done
    [ "$n" = 1 ] && [ "$nd" = 1 ] || return 0
    [ "$(dinfo "$sd" "APFS Volume Group")" = "$(dinfo "$dd" "Volume UUID")" ] || return 0
    [ "$(id -u)" = 0 ] || return 0
    mp=$(dinfo "$sd" "Mount Point"); v=""
    if [ -z "$mp" ]; then mp=$WORK/mnt-check; mkdir -p "$mp" && mount -t apfs -o rdonly,nobrowse "/dev/$sd" "$mp" 2>/dev/null || return 0; v=$mp; fi
    if [ -e "$mp/System/Library/CoreServices/boot.efi" ]; then n=0; fi
    [ -z "$v" ] || { umount "$v" 2>/dev/null || diskutil unmount force "$v" >/dev/null 2>&1; rmdir "$v" 2>/dev/null; }
    [ "$n" = 1 ] || return 0
    LEFT_HOW=shape
  fi
  LEFT="$dd $sd"
  for v in $dd $sd; do
    n=$(diskutil info "$v" 2>/dev/null | awk -F'[()]' '/Volume Used Space/ {print int($2 / 1073741824); exit}')
    LEFT_USED_GB=$((LEFT_USED_GB + ${n:-0}))
  done
  [ -n "$dd$sd" ] || LEFT=""
}

# ---------------------------------------------------------------------------------------------------------------
step "this machine"
[ "$(sysctl -n hw.cputype 2>/dev/null)" = 7 ] && [ "$(sysctl -n hw.optional.arm64 2>/dev/null)" != 1 ] \
  && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" != 1 ] || stop "this is not an Intel machine"
OSV=$(sw_vers -productVersion 2>/dev/null)
# A refusal that is the running macOS's doing (a system tool is missing or cannot do what is needed) ends with code 3
# and one line starting "SYSTEM:". Every such check runs here, before anything is downloaded or written.
system_stop() { echo "SYSTEM: $*"; echo "RESULT system"; exit 3; }
for t in curl unzip openssl aea hdiutil diskutil shasum plutil xxd ditto file base64 md5 stat df awk sed grep; do
  command -v "$t" >/dev/null 2>&1 || system_stop "the system tool '$t' is missing"
done
aea decrypt -h 2>&1 | grep -q -- '-key-value' || system_stop "aea on this macOS cannot decrypt with a given key (-key-value)"
diskutil apfs addVolume 2>&1 | grep -qE -- '-group(With|\[With\])' || system_stop "diskutil on this macOS cannot add a volume to a volume group (-groupWith)"
diskutil apfs 2>&1 | grep -q updatePreboot || system_stop "diskutil on this macOS has no updatePreboot"
[ "$(printf '<plist><dict><key>k</key><string>v</string></dict></plist>' | plutil -extract k raw -o - - 2>/dev/null)" = v ] || system_stop "plutil on this macOS cannot print a plain value (-extract ... raw)"
[ "$(printf abc | shasum -a 256 2>/dev/null | awk '{print $1}')" = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad ] || system_stop "shasum on this macOS does not compute sha256"
[ "$(printf 'YWJj' | base64 -D 2>/dev/null | xxd -p 2>/dev/null)" = 616263 ] && [ "$(printf 616263 | xxd -r -p 2>/dev/null)" = abc ] || system_stop "base64 or xxd on this macOS does not convert as expected"
# the four openssl operations the image key needs, each on a known value or a throwaway key held in memory
[ "$(printf abc | openssl dgst -sha256 -mac HMAC -macopt hexkey:00 -binary 2>/dev/null | xxd -p | tr -d '\n')" = fd7adb152c05ef80dccf50a1fa4c05d5a3ec6da95575fc312ae7c5d091836351 ] \
  || system_stop "openssl on this macOS cannot compute HMAC-SHA256"
[ "$(printf 00000000000000000000000000000000 | xxd -r -p | openssl enc -aes-256-ctr -K 0000000000000000000000000000000000000000000000000000000000000000 -iv 00000000000000000000000000000000 -nopad 2>/dev/null | xxd -p)" = dc95c078a2408989ad48a21492842087 ] \
  || system_stop "openssl on this macOS cannot do AES-256-CTR"
tk=$(openssl ecparam -name prime256v1 -genkey -noout 2>/dev/null); tp=$(printf '%s\n' "$tk" | openssl ec -pubout 2>/dev/null)
[ "$(printf '%s\n' "$tp" | openssl ec -pubin -outform DER 2>/dev/null | openssl ec -pubin -inform DER -outform DER 2>/dev/null | wc -c | tr -d ' ')" = 91 ] \
  && [ "$(openssl pkeyutl -derive -inkey <(printf '%s\n' "$tk") -peerkey <(printf '%s\n' "$tp") 2>/dev/null | wc -c | tr -d ' ')" = 32 ] \
  || system_stop "openssl on this macOS cannot do the P-256 key agreement"
unset tk tp
[ -x "$HERE/cache4k" ] || stop "cache4k is missing next to this script"
[ -x "$HERE/apfsgroup" ] || stop "apfsgroup is missing next to this script"
"$HERE/apfsgroup" >/dev/null 2>&1; [ $? = 2 ] || system_stop "apfsgroup next to this script does not run on this macOS"
"$HERE/cache4k" --info /dev/null >/dev/null 2>&1; [ $? = 1 ] || system_stop "cache4k next to this script does not run on this macOS"
if [ "$MODE" = apply ]; then [ "$(id -u)" = 0 ] || stop "run with sudo"; fi
case "$NAME" in *[!A-Za-z0-9\ ._-]*|"") stop "the volume name may hold letters, digits, spaces, dots, dashes and underscores";; esac
ok "macOS ${OSV:-?}, tools present"

# ---------------------------------------------------------------------------------------------------------------
step "where the volumes go"
ROOTCONT=$(dinfo / "APFS Container")
[ -n "$CONT" ] || CONT=$ROOTCONT
case "$CONT" in disk[0-9]*) :;; *) stop "could not tell which APFS container to use; pass --container diskN";; esac
diskutil apfs list "$CONT" >/dev/null 2>&1 || stop "$CONT is not an APFS container"
REC=$WORK/volumes.created; DONE_MARK=$WORK/volumes.complete
PREV=$(diskutil apfs list "$CONT" | awk '/APFS Volume Disk \(Role\)/ && /\(Preboot\)/ {for (i=1;i<=NF;i++) if ($i ~ /^disk[0-9]+s[0-9]+$/) {print $i; exit}}')
find_unfinished
if [ -n "$LEFT" ]; then
  if [ "$MODE" = apply ]; then
    note "removing the two unfinished volumes of the last run ($LEFT; known $([ "$LEFT_HOW" = record ] && echo "from that run's record" || echo "by their names, their group, the missing boot.efi and this work folder"))"
    if [ "$LEFT_HOW" = record ]; then remove_recorded || stop "could not remove the unfinished volumes of the last run ($LEFT); remove them yourself, then run again"
    else
      set -- $LEFT; du=$(dinfo "$1" "Volume UUID"); su=$(dinfo "$2" "Volume UUID")
      remove_one "$su" "$NAME" && remove_one "$du" "$NAME - Data" || stop "could not remove the unfinished volumes of the last run ($LEFT); remove them yourself, then run again"
      pm=$(dinfo "${PREV:-none}" "Mount Point")
      case "$du" in ????????-????-????-????-????????????) [ -z "$pm" ] || [ ! -d "$pm/$du" ] || [ -L "$pm/$du" ] || rm -rf "$pm/$du";; esac
      set --
    fi
    LEFT=""; LEFT_USED_GB=0
  else note "the last run did not finish and left two volumes ($LEFT, $LEFT_USED_GB GB): with --apply they are removed first"; fi
fi
for v in $(vols_of "$CONT"); do
  case " $LEFT " in *" $v "*) continue;; esac
  n=$(dinfo "$v" "Volume Name")
  [ "$n" != "$NAME" ] && [ "$n" != "$NAME - Data" ] || stop "$CONT already has a volume named '$n' ($v); choose another --name or remove it yourself"
done
CFREE=$(diskutil apfs list "$CONT" | awk '/Capacity Not Allocated|Capacity Available/ {for (i=1;i<=NF;i++) if ($i=="B") {print int($(i-1)/1073741824); exit}}')
[ -n "$CFREE" ] || stop "could not read the free space of $CONT"
CFREE=$((CFREE + LEFT_USED_GB))      # what the unfinished volumes hold comes back when they are removed
[ "$CFREE" -ge "$NEED_CONTAINER_GB" ] || stop "$CONT has $CFREE GB free; $NEED_CONTAINER_GB GB are needed for macOS 27"
[ -n "$PREV" ] || stop "$CONT has no Preboot volume: it is not a container macOS starts from"
ok "$CONT, $CFREE GB free, no volume named '$NAME', Preboot is $PREV"

# ---------------------------------------------------------------------------------------------------------------
step "the restore image from Apple"
HAVE_IPSW=0; GIVEN=0
if [ -n "$IPSW" ]; then GIVEN=1; [ -f "$IPSW" ] || stop "--ipsw: $IPSW is not a file"
else IPSW=$WORK/UniversalMac_${VERSION}_${BUILD}_Restore.ipsw; fi
if [ -f "$IPSW" ] && [ "$(stat -f %z "$IPSW")" = "$IPSW_SIZE" ]; then HAVE_IPSW=1; ok "found $IPSW (size matches; its checksum is verified before use)"
elif [ "$GIVEN" = 1 ]; then stop "$IPSW is $(stat -f %z "$IPSW") bytes; the macOS $VERSION ($BUILD) restore image is $IPSW_SIZE bytes. It is not that image, or it is incomplete"
elif [ -f "$IPSW" ] && [ "$(stat -f %z "$IPSW")" -gt "$IPSW_SIZE" ]; then stop "$IPSW is larger than the restore image: remove it and run again"
elif [ -f "$IPSW" ]; then note "$IPSW is $(stat -f %z "$IPSW") of $IPSW_SIZE bytes: the download will be continued"
else
  # Only this build fits the kit. When Apple stops serving it at the known address, say so instead of taking another.
  len=$(curl -fsSIL --proto =https --proto-redir =https -m 60 "$IPSW_URL" 2>/dev/null | tr -d '\r' | awk 'tolower($1)=="content-length:" {v=$2} END {print v}')
  [ "$len" = "$IPSW_SIZE" ] || stop "Apple's server does not offer the macOS $VERSION ($BUILD) restore image at the known address (answer: ${len:-none}). A later build does not fit this kit. If you have UniversalMac_${VERSION}_${BUILD}_Restore.ipsw, pass it with --ipsw"
  note "not downloaded yet; Apple's server offers it: $IPSW_URL (25.6 GB)"
fi
why=$(safe_dir "$WORK") || stop "--work: $why"
WPARENT=$WORK; while [ ! -d "$WPARENT" ]; do WPARENT=$(dirname "$WPARENT"); done
WFREE=$(free_gb "$WPARENT"); NEEDW=$((NEED_OSDMG_GB + NEED_ROS_GB)); [ "$HAVE_IPSW" = 1 ] || NEEDW=$((NEEDW + NEED_IPSW_GB))
[ -f "$WORK/os.dmg.ok" ] && NEEDW=$((NEEDW - NEED_OSDMG_GB))
[ "${WFREE:-0}" -ge "$NEEDW" ] || stop "$WPARENT has ${WFREE:-0} GB free; the work folder needs $NEEDW GB (use --work on a disk with room)"
ok "work folder $WORK, ${WFREE} GB free, $NEEDW GB needed"

if [ "$NOUSER" = 0 ]; then
  [ -n "$SEEDUSER" ] || stop "no account to carry over: pass --user NAME, or --no-user to start at Setup Assistant"
  UPL=/private/var/db/dslocal/nodes/Default/users/$SEEDUSER.plist
  if [ "$(id -u)" = 0 ]; then [ -f "$UPL" ] || stop "there is no local account '$SEEDUSER' on this macOS"; fi
  ok "account '$SEEDUSER' will be carried over (name, password, administrator rights; not the home folder's contents)"
else note "no account is carried over: the new system starts at Setup Assistant"; fi
if [ -n "$KCDIR" ]; then
  why=$(safe_dir "$KCDIR") || stop "--kernel-collections: $why"
  for k in BootKernelExtensions.kc SystemKernelExtensions.kc; do
    [ ! -L "$KCDIR/$k" ] || stop "--kernel-collections: $KCDIR/$k is a symbolic link"
    [ -f "$KCDIR/$k" ] || stop "--kernel-collections: $KCDIR/$k is missing"
    file "$KCDIR/$k" 2>/dev/null | grep -q x86_64 || stop "$KCDIR/$k is not an Intel kernel collection"
  done
  ok "Intel kernel collections given"
else note "no --kernel-collections: the volume will be complete except for them and will not start until they are placed"; fi

if [ "$MODE" != apply ]; then
  echo "NOTE dry run. With --apply this would: download and verify the restore image if needed; unpack the OS image"
  echo "NOTE (key from Apple's key address) and the Intel dyld cache; add the volumes '$NAME - Data' and '$NAME';"
  echo "NOTE copy the OS onto '$NAME'; write the dyld cache in 4 KB form; lay Apple's data template, the firmlink"
  echo "NOTE folders and the account on the data volume; add the new group's folder to Preboot and run Apple's"
  echo "NOTE Preboot update; verify each step."
  echo "RESULT dry"; exit 0
fi

# =============================================================================================================
( umask 077; mkdir -p "$WORK" ) || stop "cannot make $WORK"; chmod 700 "$WORK"
umask 022
why=$(safe_dir "$WORK") || stop "--work: $why"

# the phases of this run: one that is already done (the file is there and verified, the image is unpacked) is left out
PH=""
[ "$HAVE_IPSW" = 1 ] || PH="image-download:$((IPSW_SIZE / 30000000))"
[ "$HAVE_IPSW" = 1 ] && [ "$(cat "$WORK/ipsw.verified" 2>/dev/null)" = "$IPSW_SHA256 $(stat -f '%z %m' "$IPSW" 2>/dev/null)" ] || PH="$PH image-verify:$T_VERIFY"
[ -f "$WORK/os.dmg" ] && [ -f "$WORK/os.dmg.ok" ] && [ -f "$WORK/ros.ok" ] || PH="$PH image-unpack:$T_UNPACK"
plan $PH copy-system:$T_COPY data-volume:$T_DATA dyld-cache:$T_CACHE preboot:$T_PREBOOT

step "download and verify"
if [ "$HAVE_IPSW" != 1 ]; then
  pg_watch image-download "$IPSW_SIZE" file "$IPSW"
  curl -sS -f -L -C - --proto =https --proto-redir =https -o "$IPSW" --connect-timeout 30 --retry 100 --retry-delay 5 --retry-all-errors \
       --speed-limit 1024 --speed-time 120 "$IPSW_URL"; rc=$?
  pg_stop
  [ "$rc" != 22 ] || stop "Apple's server refused the macOS $VERSION ($BUILD) restore image at the known address. A later build does not fit this kit; pass the file with --ipsw if you have it"
  [ "$rc" = 0 ] || stop "the download did not finish (curl $rc); run again to continue it"
  [ "$(stat -f %z "$IPSW")" = "$IPSW_SIZE" ] || stop "the downloaded file has the wrong size"
  pg image-download 100 "$IPSW_SIZE" "$IPSW_SIZE"
fi
STAMP="$IPSW_SHA256 $(stat -f '%z %m' "$IPSW")"
if [ "$(cat "$WORK/ipsw.verified" 2>/dev/null)" = "$STAMP" ]; then ok "checksum already verified for this file"
else
  # read through dd, which says how far it is when asked: the same bytes reach shasum
  pg_watch image-verify "$IPSW_SIZE" dd "$WORK/verify.dd"
  got=$( { dd if="$IPSW" bs=1048576 2>"$WORK/verify.dd" & echo $! > "$WORK/verify.dd.pid"; wait $!; } | shasum -a 256 | awk '{print $1}')
  pg_stop; rm -f "$WORK/verify.dd" "$WORK/verify.dd.pid"
  [ "$got" = "$IPSW_SHA256" ] || stop "the restore image's checksum is $got, not Apple's $IPSW_SHA256: delete it and run again"
  echo "$STAMP" > "$WORK/ipsw.verified"; pg image-verify 100 "$IPSW_SIZE" "$IPSW_SIZE"; ok "sha256 matches"
fi

step "read the image's own manifest"
unzip -p "$IPSW" BuildManifest.plist > "$WORK/BuildManifest.plist" 2>/dev/null; [ -s "$WORK/BuildManifest.plist" ] || stop "no BuildManifest.plist in the restore image"
mb=$(plutil -extract ProductBuildVersion raw -o - "$WORK/BuildManifest.plist" 2>/dev/null)
[ "$mb" = "$BUILD" ] || stop "the restore image is build ${mb:-unknown}, not $BUILD"
OSAEA=$(plutil -extract "BuildIdentities.0.Manifest.OS.Info.Path" raw -o - "$WORK/BuildManifest.plist" 2>/dev/null)
ROSDMG=$(plutil -extract "BuildIdentities.0.Manifest.Cryptex1,RosettaOS.Info.Path" raw -o - "$WORK/BuildManifest.plist" 2>/dev/null)
case "$OSAEA" in *.dmg.aea) :;; *) stop "the manifest names no encrypted OS image (${OSAEA:-nothing})";; esac
case "$ROSDMG" in *.dmg) :;; *) stop "the manifest names no Rosetta image (${ROSDMG:-nothing})";; esac
ok "build $mb; OS image $OSAEA; Intel dyld cache image $ROSDMG"

# The OS image's header names a key address on Apple's server and carries the key wrapped for it (HPKE, RFC 9180:
# P-256, HKDF-SHA256, AES-256-GCM). The unwrapping key is fetched from that address and the image key is unwrapped
# with the system's openssl. A wrong key cannot pass: aea checks the image against it.
aea_key() {  # $1 = file holding the first 64 KB of the image; prints the base64 key
  local T=$WORK/keytmp url resp enc wk enc_hex wk_hex dh pkr prk ss pih ih ctx sec key nonce out
  rm -rf "$T"; mkdir -p "$T" || return 1
  url=$(LC_ALL=C grep -a -o 'https://wkms-public\.apple\.com/fcs-keys/[A-Za-z0-9_=-]*' "$1" | head -1)
  resp=$(LC_ALL=C grep -a -o '{"enc-request": "[A-Za-z0-9+/=]*", "wrapped-key": "[A-Za-z0-9+/=]*"}' "$1" | head -1)
  [ -n "$url" ] && [ -n "$resp" ] || { echo "no key address in the image header" >&2; return 1; }
  enc=$(printf '%s' "$resp" | sed 's/.*"enc-request": "\([^"]*\)".*/\1/'); wk=$(printf '%s' "$resp" | sed 's/.*"wrapped-key": "\([^"]*\)".*/\1/')
  # A reset connection here must not end the lay-down after the image is already down and verified: the request
  # is small and has no side effect, so it is tried up to five times, a little further apart each time.
  local try=1
  while ! curl -fsSL --proto =https --proto-redir =https -m 60 -o "$T/priv.pem" "$url"; do
    [ "$try" -lt 5 ] || { echo "could not reach Apple's key address in $try tries" >&2; return 1; }
    echo "the key address did not answer (try $try of 5); trying again in $((try * 5)) s" >&2
    sleep $((try * 5)); try=$((try + 1))
  done
  grep -q BEGIN "$T/priv.pem" || { echo "Apple's key address did not return a key" >&2; return 1; }
  hex() { xxd -p | tr -d '\n'; }; unhex() { xxd -r -p; }
  enc_hex=$(printf '%s' "$enc" | base64 -D | hex); wk_hex=$(printf '%s' "$wk" | base64 -D | hex)
  [ ${#enc_hex} = 130 ] && [ ${#wk_hex} = 96 ] || { echo "unexpected wrapped-key sizes" >&2; return 1; }
  # the sender's public point as a key file: the fixed P-256 SubjectPublicKeyInfo prefix, then the 65-byte point
  printf '3059301306072a8648ce3d020106082a8648ce3d030107034200%s' "$enc_hex" | unhex > "$T/peer.der"
  openssl ec -pubin -inform DER -in "$T/peer.der" -out "$T/peer.pem" 2>/dev/null || { echo "bad sender point" >&2; return 1; }
  dh=$(openssl pkeyutl -derive -inkey "$T/priv.pem" -peerkey "$T/peer.pem" 2>/dev/null | hex)
  [ ${#dh} = 64 ] || { echo "key agreement failed" >&2; return 1; }
  pkr=$(openssl ec -in "$T/priv.pem" -pubout -outform DER 2>/dev/null | tail -c 65 | hex)
  hmac() { unhex | openssl dgst -sha256 -mac HMAC -macopt hexkey:"$1" -binary | hex; }
  local ZERO=0000000000000000000000000000000000000000000000000000000000000000 V1 KEM SUITE
  s2h() { printf '%s' "$1" | hex; }
  V1=$(s2h "HPKE-v1"); KEM=$(s2h KEM)0010; SUITE=$(s2h HPKE)001000010002
  lextract() { printf '%s%s%s%s' "$V1" "$1" "$(s2h "$3")" "$4" | hmac "${2:-$ZERO}"; }
  lexpand() { printf '%04x%s%s%s%s01' "$5" "$V1" "$1" "$(s2h "$3")" "$4" | hmac "$2" | cut -c1-$(( $5 * 2 )); }
  prk=$(lextract "$KEM" "" eae_prk "$dh"); ss=$(lexpand "$KEM" "$prk" shared_secret "$enc_hex$pkr" 32)
  pih=$(lextract "$SUITE" "" psk_id_hash ""); ih=$(lextract "$SUITE" "" info_hash ""); ctx=00$pih$ih
  sec=$(lextract "$SUITE" "$ss" secret "")
  key=$(lexpand "$SUITE" "$sec" key "$ctx" 32); nonce=$(lexpand "$SUITE" "$sec" base_nonce "$ctx" 12)
  out=$(printf '%s' "${wk_hex:0:64}" | unhex | openssl enc -aes-256-ctr -K "$key" -iv "${nonce}00000002" -nopad 2>/dev/null | hex)
  rm -rf "$T"
  [ ${#out} = 64 ] || { echo "unwrap failed" >&2; return 1; }
  printf '%s' "$out" | unhex | base64
}

step "unpack the OS image"
OSDMG=$WORK/os.dmg
UNPACKING=0; UNPACK_BYTES=$((OSDMG_BYTES + ROS_BYTES))
if [ -f "$OSDMG" ] && [ "$(cat "$WORK/os.dmg.ok" 2>/dev/null)" = "$OSAEA $(stat -f %z "$OSDMG")" ]; then ok "already unpacked"
else
  rm -f "$OSDMG" "$WORK/os.dmg.ok"
  UNPACKING=1; pg_watch image-unpack "$UNPACK_BYTES" file "$OSDMG"
  unzip -p "$IPSW" "$OSAEA" 2>/dev/null | head -c 65536 > "$WORK/os.hdr"
  K=$(aea_key "$WORK/os.hdr") || stop "could not get the OS image's key (see the line above)"
  unzip -p "$IPSW" "$OSAEA" | aea decrypt -o "$OSDMG" -key-value "base64:$K"; rcs="${PIPESTATUS[*]}"
  pg_stop
  [ "$rcs" = "0 0" ] || { rm -f "$OSDMG"; stop "unpacking the OS image failed (unzip/aea: $rcs)"; }
  hdiutil imageinfo "$OSDMG" >/dev/null 2>&1 || { rm -f "$OSDMG"; stop "the unpacked OS image is not a disk image"; }
  echo "$OSAEA $(stat -f %z "$OSDMG")" > "$WORK/os.dmg.ok"; rm -f "$WORK/os.hdr"
  ok "OS image unpacked ($(( $(stat -f %z "$OSDMG") / 1073741824 )) GB)"
fi
step "unpack the Intel dyld cache image"
ROS=$WORK/$ROSDMG
if [ -f "$ROS" ] && hdiutil imageinfo "$ROS" >/dev/null 2>&1 && [ -f "$WORK/ros.ok" ]; then ok "already unpacked"
else
  rm -f "$ROS" "$WORK/ros.ok"
  # the second part of the same phase: it goes on from where the OS image left it (or starts it, when that was done before)
  if [ "$UNPACKING" = 1 ]; then pg_watch image-unpack "$UNPACK_BYTES" file "$ROS" "-$OSDMG_BYTES" "$((OSDMG_BYTES * 100 / UNPACK_BYTES))"
  else UNPACKING=1; pg_watch image-unpack "$ROS_BYTES" file "$ROS"; UNPACK_BYTES=$ROS_BYTES; fi
  unzip -o -q "$IPSW" "$ROSDMG" -d "$WORK"; rc=$?
  pg_stop
  [ "$rc" = 0 ] || stop "could not unpack $ROSDMG (unzip checks its checksum)"
  hdiutil imageinfo "$ROS" >/dev/null 2>&1 || stop "$ROSDMG is not a disk image"
  : > "$WORK/ros.ok"; ok "unpacked"
fi
[ "$UNPACKING" = 0 ] || pg image-unpack 100 "$UNPACK_BYTES" "$UNPACK_BYTES"

# attach <image> : attaches read-only without mounting, sets ATT_VOL to the single APFS volume inside
attach_ro() {
  local out cont
  out=$(hdiutil attach -readonly -nomount -nobrowse "$1" 2>&1) || { echo "$out"; return 1; }
  ATT_DISK=$(echo "$out" | awk '$1 ~ /^\/dev\/disk[0-9]+$/ {print $1; exit}'); ATTACHED="$ATTACHED $ATT_DISK"
  # the line with the APFS container type names the synthesized container, with or without a partition map around it
  cont=$(echo "$out" | awk '/EF57347C-0000-11AA-AA11/ {print $1; exit}'); cont=${cont#/dev/}
  [ -n "$cont" ] || { echo "no APFS container in $1"; return 1; }
  ATT_CONT=$cont
  set -- $(vols_of "$cont")
  [ $# -ge 1 ] || { echo "no APFS volume in the image"; return 1; }
  ATT_VOLS="$*"; ATT_VOL=$1
}
mount_ro() {  # $1 device, $2 mount point
  mkdir -p "$2" && mount -t apfs -o rdonly,nobrowse "/dev/$1" "$2" && MOUNTED="$2
$MOUNTED"
}

step "look inside the OS image"
attach_ro "$OSDMG" || stop "could not attach the OS image"
SRCVOL=""
for v in $ATT_VOLS; do
  mp=$WORK/mnt-os; umount "$mp" 2>/dev/null
  mkdir -p "$mp"; mount -t apfs -o rdonly,nobrowse "/dev/$v" "$mp" 2>/dev/null || continue
  if [ -f "$mp/System/Library/CoreServices/SystemVersion.plist" ]; then SRCVOL=$v; MOUNTED="$mp"$'\n'"$MOUNTED"; break; fi
  umount "$mp"
done
[ -n "$SRCVOL" ] || stop "no volume in the OS image holds a macOS system"
SRCMP=$WORK/mnt-os
sb=$(plutil -extract ProductBuildVersion raw -o - "$SRCMP/System/Library/CoreServices/SystemVersion.plist")
[ "$sb" = "$BUILD" ] || stop "the OS image holds build $sb, not $BUILD"
file "$SRCMP/System/Library/Kernels/kernel" 2>/dev/null | grep -q x86_64 || stop "the OS image has no Intel kernel at System/Library/Kernels/kernel"
[ -f "$SRCMP/usr/share/firmlinks" ] || stop "the OS image has no firmlink table"
[ -d "$SRCMP/System/Library/Templates/Data" ] || stop "the OS image has no data-volume template"
SRCUSED=$(df -g "$SRCMP" | awk 'NR==2 {print $3}')
ok "macOS $VERSION ($sb), Intel kernel present, $(wc -l < "$SRCMP/usr/share/firmlinks" | tr -d ' ') firmlinks, ${SRCUSED} GB"

# ---------------------------------------------------------------------------------------------------------------
step "add the two volumes"
[ "$CONT" != "${ATT_CONT:-}" ] || stop "internal: the target container is the OS image"
B0=$(vols_of "$CONT")
# An upper limit for the new system's own data, when one was asked for: APFS volumes share their container's free
# space, so without it the new system can grow until the disk is full. The limit can be changed or removed later.
if [ -n "${QUOTA_GB:-}" ]; then
  diskutil apfs addVolume "$CONT" APFS "$NAME - Data" -role D -quota "${QUOTA_GB}g" -nomount >/dev/null || stop "could not add the data volume with a limit of $QUOTA_GB GB"
else
diskutil apfs addVolume "$CONT" APFS "$NAME - Data" -role D -nomount >/dev/null || stop "could not add the data volume"
fi
B1=$(vols_of "$CONT"); DATAV=$(comm -13 <(echo "$B0") <(echo "$B1"))
[ "$(echo "$DATAV" | wc -w | tr -d ' ')" = 1 ] || stop "could not tell which volume was just added (got: $DATAV)"
CREATED="$CREATED $DATAV"
rm -f "$DONE_MARK"; printf 'name %s\ncontainer %s\ndata %s %s\n' "$NAME" "$CONT" "$DATAV" "$(dinfo "$DATAV" "Volume UUID")" > "$REC" || stop "could not write the record of the new volumes"
# The system volume is added on its own. The block restore below replaces it, and the two are grouped after that.
diskutil apfs addVolume "$CONT" APFS "$NAME" -role S -nomount >/dev/null || stop "could not add the system volume"
B2=$(vols_of "$CONT"); SYSV=$(comm -13 <(echo "$B1") <(echo "$B2"))
[ "$(echo "$SYSV" | wc -w | tr -d ' ')" = 1 ] || stop "could not tell which volume was just added (got: $SYSV)"
CREATED="$CREATED $SYSV"
printf 'system %s %s\n' "$SYSV" "$(dinfo "$SYSV" "Volume UUID")" >> "$REC" || stop "could not write the record of the new volumes"
GROUP=$(dinfo "$DATAV" "Volume UUID")
ok "data $DATAV, system $SYSV"

step "copy the OS onto $SYSV"
# The copy is a block restore, not a file copy. In Apple's image the folders that show the data volume (/private,
# /Library, /Users and the rest of /usr/share/firmlinks) carry an attribute, com.apple.fs.firmlink, that the file
# system lets nothing but itself write: a file copy is refused it and leaves plain empty folders, and macOS does not
# add it at the first start. A system laid down that way starts its kernel and then stops without writing one file,
# because /private (and with it /etc, /var and /tmp) is an empty read-only folder. Measured 11-10 on a real upgrade.
# A block restore carries the attribute. It also replaces the volume it restores to, so the volume is grouped with
# the data volume afterwards, with the call Apple's restore tool uses (apfsgroup, next to this script).
case " $CREATED " in *" $SYSV "*) :;; *) stop "internal: $SYSV was not created by this run";; esac
! in_list "$B0" "$SYSV" || stop "internal: $SYSV existed before this run"
SYSMP=$WORK/mnt-sys; DATAMP=$WORK/mnt-data; mkdir -p "$SYSMP" "$DATAMP"
FIRM=$(awk -F'\t' '$1 ~ /^\// {print substr($1, 2)}' "$SRCMP/usr/share/firmlinks")
COPY_BYTES=$(pg_now used "$SRCMP"); COPY_BYTES=${COPY_BYTES:-0}
count() { find -x "$1" -not -path "$1/.fseventsd*" -not -path "$1/.Spotlight-V100*" -not -path "$1/.Trashes*" 2>/dev/null | wc -l | tr -d ' '; }
sn=$(count "$SRCMP")
diskutil unmount "$SYSV" >/dev/null 2>&1; diskutil unmount "$DATAV" >/dev/null 2>&1
umount "$SRCMP" 2>/dev/null || diskutil unmount force "$SRCMP" >/dev/null 2>&1
pg copy-system 1 0 "$COPY_BYTES"
asr restore --source "/dev/$SRCVOL" --target "/dev/$SYSV" --erase --noprompt > "$WORK/copy.log" 2>&1; rc=$?
[ "$rc" = 0 ] && grep -q "Restore completed successfully" "$WORK/copy.log" || { tail -3 "$WORK/copy.log"; stop "the block restore of the OS failed (asr $rc; its lines are in $WORK/copy.log)"; }
# the restore names the device it wrote; the volume has the image's name and a new identifier now
rv=$(sed -n 's|.*Restored target device is /dev/\(disk[0-9]*s[0-9]*\)\..*|\1|p' "$WORK/copy.log" | tail -1)
[ -n "$rv" ] || rv=$SYSV
if [ "$rv" != "$SYSV" ]; then CREATED="$CREATED $rv"; SYSV=$rv; fi
in_list "$(vols_of "$CONT")" "$SYSV" || stop "the restored volume $SYSV is not in $CONT"
! in_list "$B0" "$SYSV" || stop "internal: $SYSV existed before this run"
# diskutil renames a mounted volume; right after a restore the volume can be in either state, so both are tried
rn=$(diskutil rename "$SYSV" "$NAME" 2>&1) || { diskutil mount "$SYSV" >/dev/null 2>&1; rn=$(diskutil rename "$SYSV" "$NAME" 2>&1); } \
  || stop "could not name the restored volume '$NAME' ($(echo "$rn" | tail -1))"
diskutil unmount "$SYSV" >/dev/null 2>&1
printf 'name %s\ncontainer %s\ndata %s %s\nsystem %s %s\n' "$NAME" "$CONT" "$DATAV" "$(dinfo "$DATAV" "Volume UUID")" "$SYSV" "$(dinfo "$SYSV" "Volume UUID")" > "$REC.new" && mv -f "$REC.new" "$REC" || stop "could not write the record of the new volumes"
[ "$(dinfo "$SYSV" "Volume Name")" = "$NAME" ] && [ "$(dinfo "$DATAV" "Volume Name")" = "$NAME - Data" ] || stop "the new volumes do not carry the names just given"
"$HERE/apfsgroup" "${CONT#/dev/}" "$GROUP" "${SYSV##*s}" "${DATAV##*s}" || stop "could not group $SYSV with the data volume"
[ "$(dinfo "$SYSV" "APFS Volume Group")" = "$GROUP" ] && [ "$(dinfo "$DATAV" "APFS Volume Group")" = "$GROUP" ] || stop "the two new volumes are not one group"
diskutil enableOwnership "$SYSV" >/dev/null 2>&1; diskutil enableOwnership "$DATAV" >/dev/null 2>&1
diskutil unmount "$SYSV" >/dev/null 2>&1; diskutil unmount "$DATAV" >/dev/null 2>&1
mount -t apfs -o nobrowse "/dev/$SYSV" "$SYSMP" || stop "could not mount the new system volume"; MOUNTED="$SYSMP"$'\n'"$MOUNTED"
mount -t apfs -o nobrowse "/dev/$DATAV" "$DATAMP" || stop "could not mount the new data volume"; MOUNTED="$DATAMP"$'\n'"$MOUNTED"
mount | grep -q " on $SYSMP (.*noowners" && stop "the system volume is mounted without ownership"
mount | grep -q " on $DATAMP (.*noowners" && stop "the data volume is mounted without ownership"
mount -t apfs -o rdonly,nobrowse "/dev/$SRCVOL" "$SRCMP" || stop "could not mount the OS image again to compare"
touch "$SYSMP/.laydown-write-test" 2>/dev/null && rm -f "$SYSMP/.laydown-write-test" || stop "the restored system volume cannot be written"
tn=$(count "$SYSMP")
[ "$sn" = "$tn" ] || stop "the copy holds $tn entries, the image $sn"
# every firmlink folder of the image is, on the new volume, an empty folder that carries the firmlink attribute
nfirm=0
while IFS= read -r rel; do
  [ -n "$rel" ] || continue
  [ -e "$SRCMP/$rel" ] || [ -L "$SRCMP/$rel" ] || continue      # in the table, not in the image (AppleInternal)
  [ -d "$SYSMP/$rel" ] && [ ! -L "$SYSMP/$rel" ] || stop "the firmlink folder /$rel is not a real folder on the new volume"
  [ -z "$(ls -A "$SYSMP/$rel" 2>/dev/null)" ] || stop "the firmlink folder /$rel is not empty on the new volume"
  xattr -p com.apple.fs.firmlink "$SYSMP/$rel" >/dev/null 2>&1 || stop "the firmlink folder /$rel did not keep its firmlink attribute: macOS 27 cannot start from this volume"
  nfirm=$((nfirm + 1))
done <<LIST
$FIRM
LIST
[ "$nfirm" -ge 10 ] || stop "only $nfirm firmlink folders were found on the new volume"
tb=$(plutil -extract ProductBuildVersion raw -o - "$SYSMP/System/Library/CoreServices/SystemVersion.plist" 2>/dev/null)
[ "$tb" = "$BUILD" ] || stop "the copied volume does not hold build $BUILD"
for f in System/Library/Kernels/kernel usr/share/firmlinks usr/lib/dyld sbin/launchd; do
  cmp -s "$SRCMP/$f" "$SYSMP/$f" || stop "the copied $f differs from the image's"
done
[ "$(stat -f '%u:%g %Lp %Sf' "$SRCMP/sbin/launchd")" = "$(stat -f '%u:%g %Lp %Sf' "$SYSMP/sbin/launchd")" ] || stop "owner, mode or flags of sbin/launchd were not kept by the copy"
pg copy-system 100 "$COPY_BYTES" "$COPY_BYTES"
ok "restored and compared: $tn entries, build $tb, $nfirm firmlink folders with their attribute, group $GROUP"

step "the data volume"
pg data-volume 0 0 0
# 1. every firmlink target exists before first start, so the system links them instead of falling back
n=0
while IFS=$'\t' read -r from to; do
  [ -n "$to" ] || continue
  mkdir -p "$DATAMP/$to" || stop "could not make $to on the data volume"; n=$((n + 1))
done < "$SYSMP/usr/share/firmlinks"
# 2. Apple's own data template, as an install lays it (the volume is new: only the folders made above exist)
ditto "$SYSMP/System/Library/Templates/Data" "$DATAMP" || stop "could not lay the data template"
miss=0
while IFS=$'\t' read -r from to; do [ -z "$to" ] || [ -d "$DATAMP/$to" ] || miss=$((miss + 1)); done < "$SYSMP/usr/share/firmlinks"
[ "$miss" = 0 ] || stop "$miss firmlink targets are missing on the data volume"
tn=$(find "$SYSMP/System/Library/Templates/Data" | wc -l | tr -d ' '); dn=$(find "$DATAMP" -not -path "$DATAMP/.*" | wc -l | tr -d ' ')
[ "$dn" -ge "$tn" ] || stop "the data volume holds $dn entries, fewer than the template's $tn"
ok "$n firmlink targets present; data template laid ($tn entries)"

PB=/usr/libexec/PlistBuddy
if [ "$NOUSER" = 0 ]; then
  step "carry the account '$SEEDUSER' over"
  DSL=$DATAMP/private/var/db/dslocal/nodes/Default
  [ -d "$DSL/users" ] && [ -f "$DSL/groups/admin.plist" ] || stop "the data template has no account database to add the account to"
  [ ! -e "$DSL/users/$SEEDUSER.plist" ] || stop "the template already has an account named $SEEDUSER"
  cp -p "$UPL" "$DSL/users/$SEEDUSER.plist" && chown root:wheel "$DSL/users/$SEEDUSER.plist" && chmod 600 "$DSL/users/$SEEDUSER.plist" || stop "could not copy the account record"
  uidn=$($PB -c "Print :uid:0" "$UPL" 2>/dev/null); gidn=$($PB -c "Print :gid:0" "$UPL" 2>/dev/null); guid=$($PB -c "Print :generateduid:0" "$UPL" 2>/dev/null)
  case "$uidn" in ''|*[!0-9]*) stop "the account record has no numeric user id";; esac
  [ "$uidn" -ge 500 ] || stop "'$SEEDUSER' is a system account (id $uidn)"
  $PB -c "Print :users" "$DSL/groups/admin.plist" >/dev/null 2>&1 || $PB -c "Add :users array" "$DSL/groups/admin.plist"
  $PB -c "Add :users: string $SEEDUSER" "$DSL/groups/admin.plist" || stop "could not add the account to the administrators"
  if [ -n "$guid" ]; then $PB -c "Print :groupmembers" "$DSL/groups/admin.plist" >/dev/null 2>&1 || $PB -c "Add :groupmembers array" "$DSL/groups/admin.plist"
    $PB -c "Add :groupmembers: string $guid" "$DSL/groups/admin.plist"; fi
  mkdir -p "$DATAMP/Users/$SEEDUSER" || stop "could not make the home folder"
  # The home folder starts as the system's own template, as it does for an account made in Setup Assistant. An empty
  # one has no Pictures, Movies, Music or Public: Photos could not make its library and asked for one at every start.
  UT="$SYSMP/System/Library/User Template"
  [ -d "$UT/Non_localized" ] || stop "the system has no home folder template"
  ditto "$UT/Non_localized" "$DATAMP/Users/$SEEDUSER" || stop "could not lay the home folder template"
  [ ! -d "$UT/English.lproj" ] || ditto "$UT/English.lproj" "$DATAMP/Users/$SEEDUSER" || stop "could not lay the localized home folder template"
  chown -R "$uidn:${gidn:-20}" "$DATAMP/Users/$SEEDUSER" && chmod 750 "$DATAMP/Users/$SEEDUSER" || stop "could not give the home folder to the account"
  for f in Movies Music Pictures Public; do [ -d "$DATAMP/Users/$SEEDUSER/$f" ] || stop "the home folder has no $f after the template was laid"; done
  for f in Desktop Documents Downloads Library Movies Music Pictures; do [ ! -d "$DATAMP/Users/$SEEDUSER/$f" ] || chmod 700 "$DATAMP/Users/$SEEDUSER/$f"; done
  chmod 755 "$DATAMP/Users/$SEEDUSER/Public"; [ ! -d "$DATAMP/Users/$SEEDUSER/Public/Drop Box" ] || chmod 733 "$DATAMP/Users/$SEEDUSER/Public/Drop Box"
  : > "$DATAMP/private/var/db/.AppleSetupDone"
  $PB -c "Print :users" "$DSL/groups/admin.plist" | grep -qx " *$SEEDUSER" || stop "the account is not in the administrators group after the edit"
  ok "account record, administrator membership, home folder from the system template, Setup Assistant marked done"
fi
if [ "$SSH" = 1 ]; then
  step "Remote Login"
  DP=$DATAMP/private/var/db/com.apple.xpc.launchd/disabled.plist; mkdir -p "$(dirname "$DP")"
  [ -f "$DP" ] || $PB -c "Save" "$DP" >/dev/null 2>&1
  $PB -c "Delete :com.openssh.sshd" "$DP" 2>/dev/null; $PB -c "Add :com.openssh.sshd bool false" "$DP" || stop "could not enable Remote Login"
  ok "enabled"
fi

# The log daemon reads this folder and never makes it: without it it exits at once, again and again, and the new
# system has no log at all. An installed macOS has it; Apple's data template does not.
if [ ! -d "$DATAMP/private/var/db/uuidtext" ]; then
  mkdir -p "$DATAMP/private/var/db/uuidtext" && chown root:wheel "$DATAMP/private/var/db/uuidtext" && chmod 755 "$DATAMP/private/var/db/uuidtext" || stop "could not make the log daemon's folder on the data volume"
fi

# Services that cannot run on an Intel PC and take the system down when they fail: the watchdog restarts the machine
# after its own repeated crashes, the others are for Apple-silicon hardware. A working Intel install has the same set off.
step "services that are switched off"
DP=$DATAMP/private/var/db/com.apple.xpc.launchd/disabled.plist; mkdir -p "$(dirname "$DP")"
[ -f "$DP" ] || $PB -c "Save" "$DP" >/dev/null 2>&1
for svc in com.apple.watchdogd com.apple.centaurid com.apple.corerepaird com.apple.CSCSupportd com.apple.iomfb_bics_daemon com.apple.peakpowermanagerd com.apple.mdmclient.daemon.runatboot; do
  $PB -c "Delete :$svc" "$DP" 2>/dev/null; $PB -c "Add :$svc bool true" "$DP" || stop "could not switch off $svc"
done
[ "$($PB -c "Print :com.apple.watchdogd" "$DP" 2>/dev/null)" = true ] || stop "the watchdog service is not switched off in $DP"
chown root:wheel "$DP"; chmod 644 "$DP"
ok "7 services off"

pg data-volume 100 0 0
step "the Intel dyld cache"
attach_ro "$ROS" || stop "could not attach the Rosetta image"
ROSMP=$WORK/mnt-ros; mount_ro "$ATT_VOL" "$ROSMP" || stop "could not mount the Rosetta image"
ls "$ROSMP/System/Library/dyld/dyld_shared_cache_x86_64" >/dev/null 2>&1 || stop "the Rosetta image has no x86_64 dyld cache"
mkdir -p "$SYSMP/System/Library/dyld"
# Apple signs these files for 16 KB pages; the Intel kernel maps 4 KB pages. cache4k writes each file with the
# same bytes and a signature that hashes 4 KB pages (see cache4k.c). The .map and .atlas files carry no signature.
c=0; CACHEFILES=""
CACHE_BYTES=0; CACHE_DONE=0
for f in "$ROSMP"/System/Library/dyld/dyld_shared_cache_x86_64*; do CACHE_BYTES=$((CACHE_BYTES + $(stat -f %z "$f" 2>/dev/null || echo 0))); done
pg dyld-cache 0 0 "$CACHE_BYTES"
for f in "$ROSMP"/System/Library/dyld/dyld_shared_cache_x86_64*; do
  b=$(basename "$f"); d=$SYSMP/System/Library/dyld/$b
  case "$b" in
    *.map|*.atlas) cp -p "$f" "$d" && cmp -s "$f" "$d" || stop "could not copy $b";;
    *) "$HERE/cache4k" "$f" "$d" || stop "could not write the 4 KB form of $b"
       i=$("$HERE/cache4k" --info "$d") || stop "$b does not read back"
       case "$i" in "page-shift 12 "*) :;; *) stop "$b is not in 4 KB form after the rewrite ($i)";; esac
       lim=$(echo "$i" | awk '{print $6}')
       # the first 0x38 bytes hold the signature size, which changes; everything after them up to the code limit must not
       cmp -s <(tail -c +57 "$f" | head -c $((lim - 56))) <(tail -c +57 "$d" | head -c $((lim - 56))) || stop "$b: the cache contents changed in the rewrite"
       CACHEFILES="$CACHEFILES $b";;
  esac
  chown root:wheel "$d"; chmod 644 "$d"; c=$((c + 1))
  CACHE_DONE=$((CACHE_DONE + $(stat -f %z "$f" 2>/dev/null || echo 0)))
  [ "$CACHE_DONE" -ge "$CACHE_BYTES" ] || pg dyld-cache $((CACHE_DONE * 100 / CACHE_BYTES)) "$CACHE_DONE" "$CACHE_BYTES"
done
[ -n "$CACHEFILES" ] || stop "no cache file was written"
pg dyld-cache 100 "$CACHE_BYTES" "$CACHE_BYTES"
ok "$c files in System/Library/dyld; the cache files carry 4 KB signatures and unchanged contents"

if [ -n "$KCDIR" ]; then
  step "Intel kernel collections"
  mkdir -p "$SYSMP/System/Library/KernelCollections" || stop "could not make the KernelCollections folder"
  for k in BootKernelExtensions.kc SystemKernelExtensions.kc; do
    cp -p "$KCDIR/$k" "$SYSMP/System/Library/KernelCollections/$k" && cmp -s "$KCDIR/$k" "$SYSMP/System/Library/KernelCollections/$k" || stop "could not place $k"
  done
  ok "placed on the system volume"
fi

step "Preboot"
pg preboot 0 0 0
PREMP=$(dinfo "$PREV" "Mount Point")
if [ -z "$PREMP" ]; then PREMP=$WORK/mnt-preboot; mkdir -p "$PREMP"; mount -t apfs -o nobrowse "/dev/$PREV" "$PREMP" || stop "could not mount Preboot ($PREV)"; MOUNTED="$PREMP"$'\n'"$MOUNTED"; fi
G=$PREMP/$GROUP
[ ! -e "$G" ] || stop "Preboot already has a folder for $GROUP"
CS=$G/System/Library/CoreServices
mkdir -p "$CS" "$G/boot/System/Library/KernelCollections" "$G/Library/Preferences/SystemConfiguration" || stop "could not make the Preboot folder"
# Apple's macOS 27 image carries no Intel boot.efi. The one this machine starts its own macOS with is the same program
# an Intel install of any recent macOS uses, so the new system gets a copy of this machine's own file.
BOOTEFI=$SYSMP/System/Library/CoreServices/boot.efi
if [ ! -f "$BOOTEFI" ]; then
  [ -f /System/Library/CoreServices/boot.efi ] || stop "this macOS has no /System/Library/CoreServices/boot.efi to start the new system with"
  cp -p /System/Library/CoreServices/boot.efi "$BOOTEFI" && cmp -s /System/Library/CoreServices/boot.efi "$BOOTEFI" || stop "could not place boot.efi on the system volume"
  chown root:wheel "$BOOTEFI"
fi
cp -p "$BOOTEFI" "$CS/boot.efi" || stop "could not copy boot.efi"
# which boot.efi this is, for the log: its own version text and its checksum
note "boot.efi $(LC_ALL=C grep -a -m1 -oE 'boot\.efi [0-9]+~[0-9]+' "$BOOTEFI" 2>/dev/null | head -1 | awk '{print $2}') md5 $(md5 -q "$BOOTEFI" 2>/dev/null)"
for f in SystemVersion.plist PlatformSupport.plist BridgeVersion.plist BridgeVersion.bin; do
  [ ! -f "$SYSMP/System/Library/CoreServices/$f" ] || cp -p "$SYSMP/System/Library/CoreServices/$f" "$CS/$f"
done
printf '%s' "$GROUP" > "$CS/.root_uuid"            # 36 bytes, no newline, as an install writes it
printf '%s' "$NAME" > "$CS/.disk_label.contentDetails"
cat > "$G/Library/Preferences/SystemConfiguration/com.apple.Boot.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Kernel Flags</key>
	<string>ignition_force_dylib_root=rootfs</string>
</dict>
</plist>
PL
# The flag makes macOS 27 take its Intel system libraries from the system volume: Apple's own cryptex holds no Intel
# copy, so without it no program can start (the kernel runs, nothing after it does, and no log is written). It belongs
# to this volume group only. A working Intel install carries the same file in both places, so both are written.
cp -p "$G/Library/Preferences/SystemConfiguration/com.apple.Boot.plist" "$CS/com.apple.Boot.plist" || stop "could not write the boot settings beside boot.efi"
for f in "$G/Library/Preferences/SystemConfiguration/com.apple.Boot.plist" "$CS/com.apple.Boot.plist"; do
  [ "$(plutil -extract "Kernel Flags" raw -o - "$f" 2>/dev/null)" = "ignition_force_dylib_root=rootfs" ] || stop "the boot settings file $f did not keep the kernel flag"
done
if [ -f "$SYSMP/System/Library/KernelCollections/BootKernelExtensions.kc" ]; then
  cp -p "$SYSMP/System/Library/KernelCollections/BootKernelExtensions.kc" "$G/boot/System/Library/KernelCollections/" || stop "could not copy the boot kernel collection to Preboot"
  # As on the install this was measured against (11-10): the system collection beside the boot one, and the
  # loader with its two property lists under boot/ as well as in the group's own CoreServices.
  if [ -f "$SYSMP/System/Library/KernelCollections/SystemKernelExtensions.kc" ]; then
    cp -p "$SYSMP/System/Library/KernelCollections/SystemKernelExtensions.kc" "$G/boot/System/Library/KernelCollections/" || stop "could not copy the system kernel collection to Preboot"
  fi
  mkdir -p "$G/boot/System/Library/CoreServices" || stop "could not make the Preboot boot folder"
  cp -p "$BOOTEFI" "$G/boot/System/Library/CoreServices/boot.efi" || stop "could not copy boot.efi to the Preboot boot folder"
  for f in SystemVersion.plist PlatformSupport.plist; do
    [ ! -f "$SYSMP/System/Library/CoreServices/$f" ] || cp -p "$SYSMP/System/Library/CoreServices/$f" "$G/boot/System/Library/CoreServices/$f" || stop "could not copy $f to the Preboot boot folder"
  done
  HAVEKC=1
else HAVEKC=0; fi
chown -R root:wheel "$G"
out=$(diskutil apfs updatePreboot "$SYSV" 2>&1); rc=$?
[ "$rc" = 0 ] || { echo "$out" | tail -5; stop "Apple's Preboot update failed (rc $rc)"; }
[ "$(wc -c < "$CS/.root_uuid" | tr -d ' ')" = 36 ] && cmp -s "$BOOTEFI" "$CS/boot.efi" || stop "the Preboot folder did not keep boot.efi and the group identifier"
ok "folder $GROUP added to Preboot ($PREV); Apple's Preboot update rc 0"
# The Rosetta cryptex is not mounted on an Intel start, so the place it would appear holds links to the cache on the
# system volume. Existing entries are left alone.
RL=$PREMP/Cryptexes/Rosetta/System/Library/dyld; made=0
mkdir -p "$RL" || stop "could not make the Rosetta cache folder on Preboot"
for b in $CACHEFILES; do
  if [ -e "$RL/$b" ] || [ -L "$RL/$b" ]; then note "Preboot already has Cryptexes/Rosetta/.../$b: left as it is"; continue; fi
  ln -s "/System/Library/dyld/$b" "$RL/$b" || stop "could not link $b"; made=$((made + 1))
done
ok "$made cache links under Preboot Cryptexes/Rosetta"

# Apple keeps part of macOS 27 outside the system volume, in two more images of the same download: the OS cryptex and
# the App cryptex. A Mac attaches them at Preboot's Cryptexes/OS and Cryptexes/App; an Intel start attaches nothing,
# so the two folders must hold the content themselves. Without it the plug-in daemon cannot load its own framework
# (PlugInKitDaemon, which exists only there), it exits at every launch, the first login never finishes, and Safari,
# which lives in the App cryptex, is missing (measured on the first install made with this script).
# The content cannot be written into the two folders from here: Preboot is shared with the macOS this runs on, and
# while that macOS runs its own images cover both folders read-only. It is left beside them, complete and marked,
# and the start-up script of the new system (bringup/cryptex-place.sh) renames it into place before anyone logs in.
# The content must be Apple's general build from this download; an installed Mac's copy is built for its own chip.
step "the OS and App cryptex content"
CXOS=$(plutil -extract "BuildIdentities.0.Manifest.Cryptex1,SystemOS.Info.Path" raw -o - "$WORK/BuildManifest.plist" 2>/dev/null)
CXAPP=$(plutil -extract "BuildIdentities.0.Manifest.Cryptex1,AppOS.Info.Path" raw -o - "$WORK/BuildManifest.plist" 2>/dev/null)
case "$CXOS" in *.dmg.aea|*.dmg) :;; *) stop "the manifest names no OS cryptex image (${CXOS:-nothing})";; esac
case "$CXAPP" in *.dmg.aea|*.dmg) :;; *) stop "the manifest names no App cryptex image (${CXAPP:-nothing})";; esac
cx_unpack() {  # $1 the image's name in the download, $2 the disk image to write
  local k rcs
  rm -f "$2"
  case "$1" in
    *.aea) unzip -p "$IPSW" "$1" 2>/dev/null | head -c 65536 > "$WORK/cx.hdr"
           k=$(aea_key "$WORK/cx.hdr") || { rm -f "$WORK/cx.hdr"; return 1; }
           unzip -p "$IPSW" "$1" | aea decrypt -o "$2" -key-value "base64:$k"; rcs="${PIPESTATUS[*]}"; rm -f "$WORK/cx.hdr"
           [ "$rcs" = "0 0" ] || return 1;;
    *)     unzip -p "$IPSW" "$1" > "$2" || return 1;;
  esac
  hdiutil imageinfo "$2" >/dev/null 2>&1
}
CXSTAGE=$PREMP/Cryptexes/.golden-gate-stage
rm -rf "$CXSTAGE"; mkdir -p "$CXSTAGE" || stop "could not make the cryptex staging folder on Preboot"
for pair in "OS:$CXOS" "App:$CXAPP"; do
  cxn=${pair%%:*}; cxp=${pair#*:}; cximg=$WORK/cryptex-$cxn.dmg; cxmp=$WORK/mnt-cx-$cxn
  cx_unpack "$cxp" "$cximg" || stop "could not unpack the $cxn cryptex image ($cxp)"
  attach_ro "$cximg" || stop "could not attach the $cxn cryptex image"
  mount_ro "$ATT_VOL" "$cxmp" || stop "could not mount the $cxn cryptex image"
  cxwant=$(find "$cxmp" -type f 2>/dev/null | wc -l | tr -d ' ')
  [ "$cxwant" -gt 0 ] || stop "the $cxn cryptex image holds no files"
  ditto "$cxmp" "$CXSTAGE/$cxn" || stop "could not copy the $cxn cryptex content to Preboot"
  cxgot=$(find "$CXSTAGE/$cxn" -type f 2>/dev/null | wc -l | tr -d ' ')
  [ "$cxgot" = "$cxwant" ] || stop "the $cxn cryptex content on Preboot has $cxgot files, the image has $cxwant"
  ok "$cxn cryptex: $cxgot files beside their place on Preboot"
done
# the one file whose absence stopped the first login, and the one application that was missing
[ -f "$CXSTAGE/OS/System/Library/PrivateFrameworks/PlugInKitDaemon.framework/Versions/A/PlugInKitDaemon" ] || stop "the OS cryptex content has no PlugInKitDaemon framework"
[ -d "$CXSTAGE/App/System/Applications/Safari.app" ] || stop "the App cryptex content has no Safari"
chown -R root:wheel "$CXSTAGE" 2>/dev/null
# written last: the start-up script touches nothing without it, and it names the build the content is for
printf '%s\n' "$BUILD" > "$CXSTAGE/ready" || stop "could not mark the cryptex content as complete"
[ "$(head -n 1 "$CXSTAGE/ready")" = "$BUILD" ] || stop "the cryptex content's mark did not keep the build"

pg preboot 100 0 0
step "result"
cleanup; MOUNTED=""; ATTACHED=""
: > "$DONE_MARK" || stop "could not mark the run as finished"
CREATED=""        # from here on nothing takes the volumes away again
[ "$KEEP" = 1 ] || rm -f "$OSDMG" "$WORK/os.dmg.ok" "$ROS" "$WORK/ros.ok"
# the two cryptex images are small beside the others and are written again on every run: never kept
rm -f "$WORK/cryptex-OS.dmg" "$WORK/cryptex-App.dmg"
echo "OK   macOS $VERSION ($BUILD) is on '$NAME' ($SYSV) with data volume '$NAME - Data' ($DATAV), group $GROUP"
echo "NOTE not done here, needed before it starts:"
[ "$HAVEKC" = 1 ] || echo "NOTE   - Intel kernel collections (System/Library/KernelCollections on the volume, and the boot one in Preboot/$GROUP/boot/System/Library/KernelCollections)"
echo "NOTE   - the kit's system installer and its driver installer (Golden Gate finish)"
echo "NOTE   - the macOS 27 boot loader configuration (Golden Gate stage and activate)"
echo "RESULT ok"
