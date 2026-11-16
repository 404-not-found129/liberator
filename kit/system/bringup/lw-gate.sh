#!/bin/bash
# Starts the login window once the window server answers. It is the login window's launch entry.

LW=/System/Library/CoreServices/loginwindow.app/Contents/MacOS/loginwindow
LOG=/var/log/nullmoth-ws-gate.log
GATE_DONE=/var/run/lw-gate.done                       # this gate's per-boot record (/var/run is emptied at boot)
GATE_LOCK=/var/run/lw-gate.owner                      # mkdir'd by the one start that owns the boot's first exec
RELEASED=/var/run/lw-gate.released                    # "released <epoch>": the owner exec'd loginwindow
DUP_WINDOW_SECS=120                                   # starts this soon after the release, with one running, are duplicates
RELEASE_WAIT_TRIES=200                                # x 2 s: longer than the owner's own bound (360 s + settle)
WSGATE_DONE=/var/run/ws-gate.done                     # ws-gate.sh's per-boot marker
LW_FIRST_RUN=/var/run/com.apple.loginwindow.didRunThisBoot
LW_PREFS=/Library/Preferences/com.apple.loginwindow
JOB=${XPC_SERVICE_NAME:-com.apple.loginwindow}        # launchd sets XPC_SERVICE_NAME to this job's label
WAIT_TRIES=180                                        # x 2 s = 360 s for WindowServer
WSREADY=/Library/NullMoth/bringup/wsready             # exit 0 once com.apple.windowserver.active answers a lookup
SETTLE_SECS=5                                         # let WindowServer finish its display setup first

say() { echo "$(date '+%F %T') lw-gate: $*" >> "$LOG"; }
start_loginwindow() { exec "$LW" "$@"; }

wait_for_windowserver() {
  local i pid=""
  if [ ! -x "$WSREADY" ]; then
    for i in $(seq 1 "$WAIT_TRIES"); do
      if [ -e "$WSGATE_DONE" ] && pgrep -x WindowServer >/dev/null; then
        echo "WindowServer process up after ~$((i * 2)) s ($WSREADY is missing: not checked that it serves - loginwindow may exit)"; return 0; fi
      sleep 2
    done
    echo "timed out after $((WAIT_TRIES * 2)) s"; return 1
  fi
  for i in $(seq 1 "$WAIT_TRIES"); do
    if [ -e "$WSGATE_DONE" ] && pid=$(pgrep -x WindowServer | head -1) && [ -n "$pid" ]; then
      if "$WSREADY" 0 >/dev/null 2>&1; then echo "WindowServer $pid serving com.apple.windowserver.active after ~$((i * 2)) s"; return 0; fi
    fi
    sleep 2
  done
  echo "timed out after $((WAIT_TRIES * 2)) s (WindowServer ${pid:-not running}; com.apple.windowserver.active never answered)"
  return 1
}

boot_secs() { sysctl -n kern.boottime 2>/dev/null | sed -n 's/^{ *sec = \([0-9]*\).*/\1/p'; }   # not usec

console_login_since_boot() {
  local boot out rc now year line t
  boot=$(boot_secs)
  out=$(last -t console 2>/dev/null); rc=$?
  [ -n "$boot" ] && [ "$rc" = 0 ] || { echo unknown; return; }
  now=$(date +%s); year=$(date +%Y)
  while IFS= read -r line; do
    set -- $line
    [ "$2" = console ] && [ $# -ge 6 ] || continue
    t=$(date -j -f "%Y %a %b %e %H:%M" "$year $3 $4 $5 $6" +%s 2>/dev/null) || continue
    [ "$t" -gt "$now" ] && t=$(date -j -f "%Y %a %b %e %H:%M" "$((year - 1)) $3 $4 $5 $6" +%s 2>/dev/null)
    [ -n "$t" ] && [ "$t" -ge $((boot - 60)) ] && { echo yes; return; }
  done <<< "$out"
  echo no
}

previous_exit() {
  launchctl print "system/$JOB" 2>/dev/null | sed -En 's/^[[:space:]]*(last exit code|last exit reason) = (.*)/\1 \2/p' | tr '\n' ';' | sed 's/;$//'
}

restore_first_run() {
  local when auto owner since
  [ -e "$LW_FIRST_RUN" ] || { say "relaunch: loginwindow's first-run marker is absent, nothing to restore"; return 0; }
  when=$(stat -f '%Sm' -t '%T' "$LW_FIRST_RUN" 2>/dev/null)
  if pgrep -x loginwindow >/dev/null; then
    say "relaunch: a loginwindow is running - first-run marker (written ${when:-?}) kept"; return 0
  fi
  if grep -q '^restored' "$GATE_DONE" 2>/dev/null; then
    say "relaunch: first-run marker (written ${when:-?}) already restored once this boot - kept"; return 0
  fi
  auto=$(defaults read "$LW_PREFS" autoLoginUser 2>/dev/null)
  [ -n "$auto" ] || { say "relaunch: automatic login is not set up - marker kept"; return 0; }
  owner=$(stat -f %Su /dev/console 2>/dev/null)
  if [ -n "$owner" ] && [ "$owner" != root ]; then say "relaunch: $owner is on the console - marker kept"; return 0; fi
  since=$(console_login_since_boot)
  case "$since" in
    yes) say "relaunch after a logout (a console login is recorded this boot) - marker kept"; return 0;;
    unknown) say "relaunch: the login records cannot be read - marker kept"; return 0;;
  esac
  if rm -f "$LW_FIRST_RUN" && [ ! -e "$LW_FIRST_RUN" ]; then
    echo "restored $(date '+%T')" >> "$GATE_DONE"
    say "relaunch with nobody logged in yet: the earlier loginwindow used this boot's first run (marker written ${when:-?}); marker removed so automatic login for $auto runs"
  else
    say "relaunch: could not remove the first-run marker written ${when:-?} - automatic login will not happen this boot"
  fi
}

released_at() { sed -n 's/^released \([0-9][0-9]*\).*/\1/p' "$RELEASED" 2>/dev/null; }

lwgate_main() {
  local why prev rel i since
  if mkdir "$GATE_LOCK" 2>/dev/null; then
    echo "start $(date '+%T')" > "$GATE_DONE"
    why=$(wait_for_windowserver)
    sleep "$SETTLE_SECS"
    echo "released $(date +%s)" > "$RELEASED.tmp" && mv -f "$RELEASED.tmp" "$RELEASED"
    say "starting loginwindow: $why"
    start_loginwindow "$@"
    return
  fi
  if [ ! -e "$RELEASED" ]; then
    say "another start is waiting for WindowServer - this one waits for it"
    for i in $(seq 1 "$RELEASE_WAIT_TRIES"); do [ -e "$RELEASED" ] && break; sleep 2; done
    [ -e "$RELEASED" ] || say "the first start never released (waited $((RELEASE_WAIT_TRIES * 2)) s) - going on"
  fi
  rel=$(released_at)
  if [ -n "$rel" ] && [ $(( $(date +%s) - rel )) -lt "$DUP_WINDOW_SECS" ] && pgrep -x loginwindow >/dev/null; then
    since=$(console_login_since_boot)
    if [ "$since" = no ]; then
      say "duplicate start $(( $(date +%s) - rel )) s after the release while loginwindow $(pgrep -x loginwindow | head -1) runs - not starting a second one"
      return 0
    fi
  fi
  prev=$(previous_exit)
  say "loginwindow relaunched (previous one ended: ${prev:-exit status not recorded by launchd})"
  restore_first_run
  start_loginwindow "$@"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  case "${1:-}" in
    *) lwgate_main "$@";;
  esac
fi

