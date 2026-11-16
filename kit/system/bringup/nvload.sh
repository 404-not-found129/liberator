#!/bin/bash
# Brings the NVIDIA display up at start-up and puts the window server on it. Run once per start by its launch job.

LOG=/var/log/nullmoth-nvload.log
NVGO=/Library/NullMoth/bringup/nvgo                 # NVRM user client, method 0 = run bring-up pass <n> (root only)
GUARD=7C436110-AB2A-4BBB-A880-FE41995C9F82:nullmoth-nv-starts
HEAD_WAIT_TRIES=300                                 # x 2 s = 600 s for a head
GO2_SETTLE_TRIES=30                                 # x 2 s = 60 s for the retried pass to publish a head
NVGO_SECS=30                                        # nvgo itself is bounded: a pass that never returns must not hold the boot
GO2_RETRIED=0
IDLE_WATCH_TRIES=60                                 # x 3 s = 3 more minutes for an idle desktop's first frame
WSGATE_DONE=/var/run/ws-gate.done                   # written by ws-gate.sh ("armed ...") when it armed the pipe first
PIPE_DONE=/var/run/nvload.pipe                      # "final <time> <outcome>": nvload changes nothing more this boot

say() { echo "$(date '+%F %T') $*"; }

run_bounded() {
  local secs=$1 p w rc; shift
  "$@" & p=$!
  ( /bin/sleep "$secs"; kill -9 "$p" 2>/dev/null ) & w=$!
  wait "$p"; rc=$?
  kill "$w" 2>/dev/null; wait "$w" 2>/dev/null
  [ "$rc" = 137 ] && rc=124
  return $rc
}

heads() { local h; h=$(sysctl -n debug.nvaccel_heads_published 2>/dev/null); case "$h" in ''|*[!0-9]*) echo 0;; *) echo "$h";; esac; }
autogo_state() { ioreg -c NVRM -lw0 2>/dev/null | sed -n 's/.*"nvrm-autogo" = "\([^"]*\)".*/\1/p' | head -n 1; }

go2_retry() {
  local st out rc i h
  [ "$GO2_RETRIED" = 1 ] && return 1
  st=$(autogo_state)
  [ "$st" = "go(2) failed" ] || return 1
  GO2_RETRIED=1
  say "auto-go reports go(2) failed: clearing the start guard, then running go(2) once more"
  nvram -d "$GUARD" 2>/dev/null
  if out=$(nvram "$GUARD" 2>/dev/null) && [ -n "$out" ]; then
    say "go(2) NOT retried: the start guard is still set ($out) - a retry would count as another start"
    return 1
  fi
  say "start guard cleared"
  if [ ! -x "$NVGO" ]; then say "go(2) NOT retried: $NVGO is missing"; return 1; fi
  out=$(run_bounded "$NVGO_SECS" "$NVGO" 2 2>&1); rc=$?
  if [ "$rc" = 124 ]; then say "nvgo 2 did not return within $NVGO_SECS s and was stopped"
  else say "nvgo 2 -> exit $rc: $(printf '%s' "$out" | tr '\n' ' ')"; fi
  h=0
  for i in $(seq 1 "$GO2_SETTLE_TRIES"); do
    h=$(heads); [ "$h" -ge 1 ] && break
    sleep 2
  done
  st=$(autogo_state)
  if [ "$h" -ge 1 ]; then say "go(2) retry RECOVERED: auto-go '$st', $h head(s) published"
  else say "go(2) retry did NOT recover: auto-go '$st', no head after $((GO2_SETTLE_TRIES * 2)) s - the screen stays on the software compositor; a restart usually clears it"; fi
  return 0
}

wait_for_head() {
  local i
  for i in $(seq 1 "$HEAD_WAIT_TRIES"); do
    [ "$(heads)" -ge 1 ] && { say "NVAccel head published after ~$((i * 2)) s"; return 0; }
    go2_retry && [ "$(heads)" -ge 1 ] && return 0
    sleep 2
  done
  say "STOP: NVAccel published no head within $((HEAD_WAIT_TRIES * 2)) s - the screen stays on the software compositor"
  return 1
}

console_user() {
  local u s
  u=$(stat -f %Su /dev/console 2>/dev/null)
  s=$(who 2>/dev/null | awk '$2 == "console" {print $1; exit}')
  case "$u" in root|loginwindow) u="";; esac
  case "$s" in loginwindow) s="";; esac
  if [ -n "$u" ]; then echo "$u"; elif [ -n "$s" ]; then echo "$s"
  elif [ -z "$(stat -f %Su /dev/console 2>/dev/null)" ]; then echo unknown
  elif [ -n "$(defaults read /Library/Preferences/com.apple.loginwindow autoLoginUser 2>/dev/null)" ]; then echo "automatic login"; fi
}

on_pipe_already() {
  [ -f "$WSGATE_DONE" ] || return 1
  [ "$(tail -n 1 "$WSGATE_DONE" 2>/dev/null | cut -d' ' -f1)" = software ] && return 1
  grep -q "^armed" "$WSGATE_DONE" 2>/dev/null && return 0
  [ "$(tail -n 1 "$WSGATE_DONE" 2>/dev/null | cut -d' ' -f1)" = ready ]
}
switch_windowserver() {
  local old who
  old=$(pgrep -x WindowServer)
  if on_pipe_already; then
    say "ws-gate started WindowServer $old on the pipe - no restart"
    return 0
  fi
  who=$(console_user)       # read right before the restart, so a login during the steps above is seen
  if [ -n "$who" ]; then
    say "user session active (console user ${who}) - pipe switch deferred to the next boot; WindowServer $old kept on the software compositor"
    return 1
  fi
  say "display pipe published; restarting WindowServer $old onto the Metal pipe"
  [ -n "$old" ] && kill -9 $old
  return 0
}

arm_display() {
  local caps old ok0 ok1 fail last same p i verdict
  if on_pipe_already; then
    say "ws-gate $(grep -q "^armed" "$WSGATE_DONE" 2>/dev/null && echo "armed the pipe before WindowServer $(pgrep -x WindowServer) started" || echo "found the pipe armed and started WindowServer $(pgrep -x WindowServer) on it") - no re-arm, no restart"
    pipe_verdict
    return
  fi
  if [ "$(sysctl -n debug.nvaccelfb)" = 0 ]; then
    sysctl -w debug.nvaccelfb=1; sleep 1
    if [ "$(sysctl -n debug.nvaccelfb)" != 1 ]; then say "STOP: level 1 did not take - not arming further"; return 0; fi
    sysctl -w debug.nvaccelfb=4
  else
    say "nvaccelfb already $(sysctl -n debug.nvaccelfb) - NVRM armed it itself"
  fi
  sleep 2
  sysctl -w debug.nvrmfb_agdc_k5=1; sysctl -w debug.nvrmfb_agdc=1; sysctl -w debug.nvaccel_iop=1
  caps=$(ioreg -r -c NVAccel -d 1 -l 2>/dev/null | grep -c IOAccelDisplayPipeCapabilities)
  if [ "$caps" -lt 1 ]; then say "STOP: IOAccelDisplayPipeCapabilities not published - WindowServer NOT restarted"; return 0; fi
  defaults write /Library/Preferences/com.apple.CoreDisplay useIOP_onNVIDIA -bool true
  switch_windowserver || return 0
  pipe_verdict
}

iop() { sysctl -n "debug.nvaccel_iop_$1" 2>/dev/null; }   # the pipe's counters: ok (frames through it), fail

RESTARTS=/var/db/nullmoth-display-restarts
RECOVER_MAX=2
WS_STDERR=/var/log/nullmoth-ws-stderr.log
restart_machine() { /sbin/shutdown -r now; }
DEATH_RE='native rip [0-9a-f]* *\(_ZN2CA3OGL2SW|^a2x-stop: frame [0-3] native QuartzCore\+0x[0-9a-f]+ _ZN2CA3OGL2SW'
WS_WATCH_FROM=0
file_bytes() { local n=0; [ -f "$1" ] && n=$(wc -c < "$1" 2>/dev/null | tr -d ' '); case "$n" in ''|*[!0-9]*) n=0;; esac; echo "$n"; }
ws_output_since_watch() {
  local now; now=$(file_bytes "$WS_STDERR")
  if [ "$now" -ge "$WS_WATCH_FROM" ]; then tail -c +$((WS_WATCH_FROM + 1)) "$WS_STDERR" 2>/dev/null | tail -c 400000
  else tail -c 400000 "$WS_STDERR" 2>/dev/null; fi
}
recover_dead_display() {
  local n
  if ! ws_output_since_watch | grep -aqE "$DEATH_RE"; then
    say "no automatic restart: the window server's output does not show the no-display-pipe death"; return 1; fi
  n=$(cat "$RESTARTS" 2>/dev/null); case "$n" in ''|*[!0-9]*) n=0;; esac
  if [ "$n" -ge "$RECOVER_MAX" ]; then
    say "no automatic restart: restarted $n time(s) in a row for this already - it needs a person (read /var/log/nullmoth-ws-gate.log)"; return 1; fi
  echo $((n + 1)) > "$RESTARTS.tmp" && mv -f "$RESTARTS.tmp" "$RESTARTS" || { say "no automatic restart: the count could not be written"; return 1; }
  say "the window server found no display pipe this boot: restarting the machine (automatic restart $((n + 1)) of $RECOVER_MAX)"
  restart_machine
}

pipe_verdict() {
  local ok0 fail0 ok1 fail1 last="" same=0 p i
  ok0=$(iop ok); fail0=$(iop fail); ok0=${ok0:-0}; fail0=${fail0:-0}
  WS_WATCH_FROM=$(file_bytes "$WS_STDERR")
  for i in $(seq 1 20); do
    sleep 3; p=$(pgrep -x WindowServer | head -1)
    if [ -n "$p" ] && [ "$p" = "$last" ]; then same=$((same+1)); else same=0; last="$p"; fi
    [ $same -ge 10 ] && break
  done
  if [ $same -lt 10 ]; then
    say "NOT healthy (WindowServer never held one pid for 30 s in 60 s) - flip left off; read /var/log/nullmoth-ws-stderr.log"
    recover_dead_display
    return 1
  fi
  for i in $(seq 0 "$IDLE_WATCH_TRIES"); do
    [ "$i" = 0 ] || sleep 3
    ok1=$(iop ok); fail1=$(iop fail); ok1=${ok1:-0}; fail1=${fail1:-0}
    p=$(pgrep -x WindowServer | head -1)
    if [ "$p" != "$last" ]; then
      say "NOT healthy: WindowServer $last went away (now ${p:-none}); frames through our pipe $((ok1-ok0)) (fail $((fail1-fail0))) - flip left off; read /var/log/nullmoth-ws-stderr.log"
      return 1
    fi
    if [ "$fail1" -gt "$fail0" ]; then
      say "NOT healthy: $((fail1-fail0)) frame(s) failed through our pipe (ok $((ok1-ok0))) - flip left off; read /var/log/nullmoth-ws-stderr.log"
      return 1
    fi
    if [ "$ok1" -gt "$ok0" ]; then
      sysctl -w debug.nvaccel_iop_flip=1
      pipe_final "flip on"
      rm -f "$RESTARTS"
      say "HEALTHY: WindowServer $last steady, frames through our pipe $((ok1-ok0)) (fail 0)$([ "$i" = 0 ] || echo " after $((i * 3)) s idle"); async flip on"
      return 0
    fi
  done
  say "steady but idle: WindowServer $last drew no frame in $((30 + IDLE_WATCH_TRIES * 3)) s (fail 0) - flip left off"
  return 1
}

pipe_final() {
  [ -e "$PIPE_DONE" ] && return 0
  echo "final $(date '+%T') $*" > "$PIPE_DONE.tmp" && mv -f "$PIPE_DONE.tmp" "$PIPE_DONE"
}

cap_log() {
  local f=$1 n
  n=$(stat -f %z "$f" 2>/dev/null) || return 0
  [ "$n" -gt 1048576 ] || return 0
  tail -c 524288 "$f" > "$f.cap" 2>/dev/null && cat "$f.cap" > "$f"; rm -f "$f.cap"
}
nvload_main() {
  if [ "$(cat /Library/NullMoth/bringup/graphics-path 2>/dev/null)" = apple ]; then
    say "graphics path is apple: nothing to do for the NVIDIA pipe"; return 0
  fi
  cap_log "$LOG"
  exec >> "$LOG" 2>&1
  say "== nvload start, boot $(sysctl -n kern.boottime | cut -c1-40)"
  if ! wait_for_head; then pipe_final "no head"; exit 0; fi
  arm_display
  pipe_final "done"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  case "${1:-}" in
    "") nvload_main;;
    *) echo "usage: nvload.sh" >&2; exit 2;;
  esac
fi

