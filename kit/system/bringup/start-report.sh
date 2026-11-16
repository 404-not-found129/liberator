#!/bin/bash
# With the owner's consent, tells NullMoth once how this PC's start of macOS 27 went. Run once per start by its launch job.
set -u
R=${NM_REPORT_DIR:-/Library/NullMoth/report}
LOG=${NM_REPORT_LOG:-/var/log/nullmoth-start-report.log}
VARLOG=${NM_VARLOG:-/var/log}
GFXPATH=${NM_GFXPATH:-/Library/NullMoth/bringup/graphics-path}
URL=${NM_REPORT_URL:-https://nullmothsystems.com/api/upload}
WAIT_S=600          # chosen: how long a start may take to reach a healthy desktop before it is left as "not reached"
STEADY_S=120        # chosen: a window server that keeps one process id this long counts as up (paths without nvload)
MAX_FAILED=3        # chosen: failed-start reports per install; more would repeat the same evidence
SEND_TRIES=5        # tries per start for each queued file
SEND_BOOTS=3        # starts a queued file is kept for
CONSENT=$R/consent; PENDING=$R/start.pending; SENT=$R/first-desktop.sent; FAILED_N=$R/failed.count; Q=$R/queue
say() { echo "$(date '+%F %T') $*" >> "$LOG"; }

boot_sec() { /usr/sbin/sysctl -n kern.boottime 2>/dev/null | sed -n 's/^{ *sec = \([0-9]*\).*/\1/p'; }
now_sec() { date +%s; }
sysctl_s() { /usr/sbin/sysctl -n "$1" 2>/dev/null; }
ioreg_out() { /usr/sbin/ioreg "$@" 2>/dev/null; }
profiler() { /usr/sbin/system_profiler "$@" 2>/dev/null; }
ws_pid() { /usr/bin/pgrep -x WindowServer 2>/dev/null | head -n 1; }
console_user() { /usr/bin/stat -f %Su /dev/console 2>/dev/null; }
nap() { sleep "$1"; }
post() {  # $1 = file, $2 = name, $3 = notes, $4 = batch; prints the server's answer, exit 0 when it took the file
  local sha meta
  sha=$(/usr/bin/shasum -a 256 "$1" | cut -d' ' -f1)
  meta=$(printf '{"consent": true, "notes": "%s", "batch": "%s"}' "$3" "$4" | /usr/bin/base64 | tr -d '\n=' | tr '+/' '-_')
  /usr/bin/curl -fsS --proto =https -m 90 -X POST -H 'Content-Type: application/octet-stream' -H "X-File-Name: $2" \
    -H "X-Meta: $meta" -H "X-Content-SHA256: $sha" --data-binary "@$1" "$URL"
}

redact() {
  local homes; homes=$(printf '/%s' Users)   # the folder that holds every account's home, spelled so no path stands here
  sed -E -e "s#$homes/[^/ ]+#$homes/USER#g" \
         -e 's/[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}/[uuid]/g' \
         -e 's/([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}/[mac]/g' \
         -e 's/(Serial Number[^:]*: *)[A-Za-z0-9]+/\1[serial]/g' \
         -e 's/([0-9]{1,3}\.){3}[0-9]{1,3}/[ip]/g'
}
json_str() { printf '%s' "$1" | tr -d '\000-\037' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'; }
kit_tag() { head -n 1 "$R/kit-tag" 2>/dev/null; }   # written beside the consent by the installer: the kit's release tag
batch_id() {
  [ -s "$R/batch" ] || printf 'liberator-27-%s\n' "$(/usr/bin/od -An -N8 -tx1 /dev/urandom | tr -d ' \n')" > "$R/batch"
  head -n 1 "$R/batch"
}

lines_since() {  # $1 = file, $2 = line count at the beginning of the start
  [ -f "$1" ] || return 0
  local n; n=$(wc -l < "$1" | tr -d ' ')
  if [ "${2:-0}" -le "$n" ] 2>/dev/null; then tail -n +"$(( ${2:-0} + 1 ))" "$1"; else cat "$1"; fi
}
count_lines() { if [ -f "$1" ]; then wc -l < "$1" | tr -d ' '; else echo 0; fi; }

start_facts() {  # $1 nvload lines-at-start, $2 ws-gate lines-at-start, $3 ws-stderr lines-at-start; sets F_* variables
  local nv gate err
  nv=$(lines_since "$VARLOG/nullmoth-nvload.log" "$1"); gate=$(lines_since "$VARLOG/nullmoth-ws-gate.log" "$2")
  err=$(lines_since "$VARLOG/nullmoth-ws-stderr.log" "$3")
  F_VERDICT=$(printf '%s\n' "$nv" | grep -E 'HEALTHY|STOP|did NOT recover|go\(2\) failed' | tail -n 1 | sed 's/^[0-9-]* [0-9:]* //')
  F_HEALTHY=0; printf '%s\n' "$nv" | grep -q 'HEALTHY' && F_HEALTHY=1
  F_FRAMES=$(printf '%s\n' "$F_VERDICT" | sed -n 's/.*frames through our pipe \([0-9]*\).*/\1/p')
  F_FRAMEFAIL=$(printf '%s\n' "$F_VERDICT" | sed -n 's/.*(fail \([0-9]*\)).*/\1/p')
  F_WS_STARTS=$(printf '%s\n' "$gate" | grep -c 'starting on the\|starting WindowServer on the')
  F_TIMEOUTS=$(printf '%s\n' "$err" | grep -a -c 'wait TIMEOUT')
  F_FAULTS=$(printf '%s\n' "$err" | grep -a -c 'channel FAULTED')
  F_ASSERTS=$(printf '%s\n' "$err" | grep -a -c 'Assertion failed')
}

pc_facts() {  # sets P_* variables: what the PC is, read once
  P_OS="$(/usr/bin/sw_vers -productVersion 2>/dev/null) ($(/usr/bin/sw_vers -buildVersion 2>/dev/null))"
  P_MODEL=$(sysctl_s hw.model); P_CPU=$(sysctl_s machdep.cpu.brand_string); P_VENDOR=$(sysctl_s machdep.cpu.vendor)
  P_FAMILY="family $(sysctl_s machdep.cpu.family) model $(sysctl_s machdep.cpu.model)"
  P_CORES="$(sysctl_s hw.physicalcpu) cores, $(sysctl_s hw.logicalcpu) threads"
  P_MEM_GB=$(( $(sysctl_s hw.memsize || echo 0) / 1073741824 ))
  P_PATH=$(cat "$GFXPATH" 2>/dev/null || echo nvidia)
  P_KIT=$(kit_tag)
  P_GPUS=$(ioreg_out -r -c IOPCIDevice -l -w 0 | awk '
    /\+-o / { v = ""; d = ""; c = "" }
    /"vendor-id" = </ { v = substr($0, index($0, "<") + 1, 4) }
    /"device-id" = </ { d = substr($0, index($0, "<") + 1, 4) }
    /"class-code" = </ { c = substr($0, index($0, "<") + 1, 8); if (substr(c, 5, 2) == "03" && v != "" && d != "") {
        printf "%s%s:%s%s ", substr(v, 3, 2), substr(v, 1, 2), substr(d, 3, 2), substr(d, 1, 2) } }' | tr 'a-f' 'A-F' | sed 's/ $//')
  local sp; sp=$(profiler SPDisplaysDataType)
  P_CHIP=$(printf '%s\n' "$sp" | sed -n 's/^ *Chipset Model: *//p' | paste -sd ';' -)
  P_METAL=$(printf '%s\n' "$sp" | sed -n 's/^ *Metal Support: *//p' | paste -sd ';' -)
  P_MODES=$(printf '%s\n' "$sp" | sed -n 's/^ *Resolution: *//p' | paste -sd ';' -)
  P_KEXTS=$(/usr/bin/kmutil showloaded 2>/dev/null | grep -o 'com\.nullmoth\.[A-Za-z0-9]*' | sort -u | paste -sd ' ' -)
  [ -n "$P_KEXTS" ] || P_KEXTS=$(/usr/sbin/kextstat 2>/dev/null | grep -o 'com\.nullmoth\.[A-Za-z0-9]*' | sort -u | paste -sd ' ' -)
  P_AUDIO=$(profiler SPAudioDataType | grep -c 'Output Channels')
  P_NET=$(/sbin/route -n get default 2>/dev/null | sed -n 's/^ *interface: *//p')
  P_GUARD=$(/usr/sbin/nvram 7C436110-AB2A-4BBB-A880-FE41995C9F82:nullmoth-nv-starts 2>/dev/null | cut -f2-)
  P_STOPS=$(ls /private/var/tmp/a2x-reports 2>/dev/null | grep -c '\.log$')
}

write_text() {  # $1 = out file, $2 = headline, $3 = seconds from start to a healthy desktop ("" when not reached)
  { echo "$2"; echo
    echo "macOS $P_OS"; echo "kit: ${P_KIT:-unknown}"; echo "model: $P_MODEL"
    echo "cpu: $P_CPU"; echo "cpu vendor: $P_VENDOR ($P_FAMILY; $P_CORES)"; echo "memory: $P_MEM_GB GB"
    echo "graphics path: $P_PATH"; echo "display PCI: ${P_GPUS:-none found}"
    echo "chipset: ${P_CHIP:-not reported}"; echo "Metal: ${P_METAL:-not reported}"; echo "display modes: ${P_MODES:-not reported}"
    echo "NullMoth drivers loaded: ${P_KEXTS:-none}"
    echo "start: healthy $F_HEALTHY${3:+ after $3 s}; verdict: ${F_VERDICT:-none written}"
    echo "frames through the display pipe: ${F_FRAMES:-unknown} (failed ${F_FRAMEFAIL:-unknown})"
    echo "window server starts this start: $F_WS_STARTS"
    echo "device loss this start: wait timeouts $F_TIMEOUTS, channel faults $F_FAULTS, window server assertions $F_ASSERTS"
    echo "start guard: ${P_GUARD:-(unset)}"
    echo "audio outputs: $P_AUDIO"; echo "network: ${P_NET:-no default route}"
    echo "interpreter stop records: $P_STOPS"
  } | redact > "$1"
}

write_json() {  # $1 = out file, $2 = kind (first-desktop | failed-start), $3 = seconds to healthy or ""
  { printf '{"schema": "nullmoth-start-report/1", "kind": "%s", "macos": "%s", "kit": "%s", "model": "%s", ' \
      "$2" "$(json_str "$P_OS")" "$(json_str "${P_KIT:-}")" "$(json_str "$P_MODEL")"
    printf '"cpu": "%s", "cpu_vendor": "%s", "cpu_family": "%s", "cores": "%s", "memory_gb": %s, ' \
      "$(json_str "$P_CPU")" "$(json_str "$P_VENDOR")" "$(json_str "$P_FAMILY")" "$(json_str "$P_CORES")" "${P_MEM_GB:-0}"
    printf '"graphics_path": "%s", "display_pci": "%s", "chipset": "%s", "metal": "%s", "display_modes": "%s", ' \
      "$(json_str "$P_PATH")" "$(json_str "$P_GPUS")" "$(json_str "$P_CHIP")" "$(json_str "$P_METAL")" "$(json_str "$P_MODES")"
    printf '"drivers_loaded": "%s", "healthy": %s, "seconds_to_healthy": %s, "verdict": "%s", ' \
      "$(json_str "$P_KEXTS")" "$([ "$F_HEALTHY" = 1 ] && echo true || echo false)" "${3:-null}" "$(json_str "${F_VERDICT:-}")"
    printf '"frames": %s, "frames_failed": %s, "window_server_starts": %s, "wait_timeouts": %s, "channel_faults": %s, ' \
      "${F_FRAMES:-null}" "${F_FRAMEFAIL:-null}" "${F_WS_STARTS:-0}" "${F_TIMEOUTS:-0}" "${F_FAULTS:-0}"
    printf '"window_server_assertions": %s, "audio_outputs": %s, "network": "%s", "interpreter_stops": %s}\n' \
      "${F_ASSERTS:-0}" "${P_AUDIO:-0}" "$(json_str "${P_NET:-}")" "${P_STOPS:-0}"
  } | redact > "$1"
}

queue_add() {  # $1 = file, $2 = name on the server, $3 = notes
  mkdir -p "$Q" || return 1
  local id; id="$(now_sec)-$$-$(ls "$Q" 2>/dev/null | wc -l | tr -d ' ')"
  cp "$1" "$Q/$id.body" && printf '%s\n%s\n0\n' "$2" "$3" > "$Q/$id.meta"
}

send_queue() {
  local m id name notes boots try ok
  [ -d "$Q" ] || return 0
  for m in "$Q"/*.meta; do
    [ -f "$m" ] || continue
    id=${m%.meta}; name=$(sed -n 1p "$m"); notes=$(sed -n 2p "$m"); boots=$(sed -n 3p "$m")
    ok=0; try=1
    while [ "$try" -le "$SEND_TRIES" ]; do
      if post "$id.body" "$name" "$notes" "$(batch_id)" >> "$LOG" 2>&1; then ok=1; break; fi
      nap $((try * 10)); try=$((try + 1))
    done
    if [ "$ok" = 1 ]; then say "sent $name"; rm -f "$id.body" "$m"; [ "$name" = first-desktop.txt ] && : > "$SENT"
    else
      boots=$((${boots:-0} + 1))
      if [ "$boots" -ge "$SEND_BOOTS" ]; then say "dropped $name: not sent in $boots starts"; rm -f "$id.body" "$m"
      else printf '%s\n%s\n%s\n' "$name" "$notes" "$boots" > "$m"; say "kept $name for the next start (start $boots of $SEND_BOOTS)"; fi
    fi
  done
}

report_failed_start() {  # reads the marker of the earlier start
  local psec pnv pgate perr n out
  psec=$(sed -n 1p "$PENDING"); pnv=$(sed -n 2p "$PENDING"); pgate=$(sed -n 3p "$PENDING"); perr=$(sed -n 4p "$PENDING")
  n=$(cat "$FAILED_N" 2>/dev/null || echo 0)
  [ "$n" -lt "$MAX_FAILED" ] || { say "an earlier start did not reach the desktop; $n already reported, not reported again"; return 0; }
  out=$R/work; rm -rf "$out"; mkdir -p "$out" || return 1
  pc_facts; start_facts "${pnv:-0}" "${pgate:-0}" "${perr:-0}"
  write_text "$out/start-failed.txt" "macOS 27 start did not reach a healthy desktop (the start at $(date -r "${psec:-0}" '+%F %T' 2>/dev/null || echo unknown))" ""
  write_json "$out/start-failed.json" failed-start ""
  { echo "== display hand-over (that start's lines)"; lines_since "$VARLOG/nullmoth-nvload.log" "${pnv:-0}" | tail -n 120
    echo "== window server and login window gates"; lines_since "$VARLOG/nullmoth-ws-gate.log" "${pgate:-0}" | tail -n 120
    echo "== window server output: driver and assertion lines"
    lines_since "$VARLOG/nullmoth-ws-stderr.log" "${perr:-0}" | grep -a -E 'nvrm:|nvk|Assertion failed|FATAL|a2x-stop: (signal|program)' | tail -n 80
    echo "== kernel collection loader"; tail -n 40 "$VARLOG/auxkcload.log" 2>/dev/null
  } | tr -d '\000' | redact > "$out/start-failed-logs.txt"
  local notes="macOS 27 start did not reach the desktop; Liberator kit ${P_KIT:-unknown}"
  queue_add "$out/start-failed.txt" start-failed.txt "$notes"; queue_add "$out/start-failed.json" start-failed.json "$notes"
  queue_add "$out/start-failed-logs.txt" start-failed-logs.txt "$notes"
  echo $((n + 1)) > "$FAILED_N"; rm -rf "$out"
  say "an earlier start did not reach the desktop: report queued ($((n + 1)) of $MAX_FAILED)"
}

wait_healthy() {  # $1 = nvload lines at the beginning of this start; prints seconds waited, exit 1 when not reached
  local t0 pid0="" since=0 pid
  t0=$(now_sec)
  while [ $(( $(now_sec) - t0 )) -lt "$WAIT_S" ]; do
    if lines_since "$VARLOG/nullmoth-nvload.log" "$1" | grep -q 'HEALTHY'; then echo $(( $(now_sec) - t0 )); return 0; fi
    if [ "$(cat "$GFXPATH" 2>/dev/null || echo nvidia)" != nvidia ]; then
      pid=$(ws_pid)
      if [ -n "$pid" ] && [ "$pid" = "$pid0" ] && [ "$(console_user)" != root ] && [ -n "$(console_user)" ]; then
        [ $(( $(now_sec) - since )) -ge "$STEADY_S" ] && { echo $(( $(now_sec) - t0 )); return 0; }
      else pid0=$pid; since=$(now_sec); fi
    fi
    nap 5
  done
  return 1
}

main() {
  [ -f "$CONSENT" ] || exit 0
  mkdir -p "$R" || exit 0
  local nv gate err secs
  if [ -f "$PENDING" ] && [ "$(sed -n 1p "$PENDING")" != "$(boot_sec)" ]; then report_failed_start; fi
  nv=$(count_lines "$VARLOG/nullmoth-nvload.log"); gate=$(count_lines "$VARLOG/nullmoth-ws-gate.log")
  err=$(count_lines "$VARLOG/nullmoth-ws-stderr.log")
  printf '%s\n%s\n%s\n%s\n' "$(boot_sec)" "$nv" "$gate" "$err" > "$PENDING.tmp" && mv -f "$PENDING.tmp" "$PENDING"
  send_queue
  if secs=$(wait_healthy "$nv"); then
    rm -f "$PENDING"
    if [ ! -f "$SENT" ] && [ ! -f "$R/first-desktop.queued" ]; then
      local out=$R/work; rm -rf "$out"; mkdir -p "$out" || exit 0
      pc_facts; start_facts "$nv" "$gate" "$err"
      write_text "$out/first-desktop.txt" "macOS 27 reached a healthy desktop" "$secs"
      write_json "$out/first-desktop.json" first-desktop "$secs"
      local notes="reached desktop; Liberator kit ${P_KIT:-unknown} on macOS 27 (first healthy desktop)"
      queue_add "$out/first-desktop.txt" first-desktop.txt "$notes"; queue_add "$out/first-desktop.json" first-desktop.json "$notes"
      : > "$R/first-desktop.queued"; rm -rf "$out"; say "first healthy desktop after $secs s: report queued"
    fi
    send_queue
  else
    say "no healthy desktop in $WAIT_S s: the marker stays for the next start"
  fi
}
if [ "${BASH_SOURCE[0]}" = "$0" ]; then main "$@"; fi

