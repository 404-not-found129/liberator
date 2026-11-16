#!/bin/bash
# Moves Apple's OS and App cryptex content, left beside its place when macOS 27 was laid down, into place.
# Run by ws-gate.sh before the first window server start of each boot. It never replaces a folder that holds anything.
CRYPTEXES=/System/Volumes/Preboot/Cryptexes
STAGE_NAME=.golden-gate-stage
VERSION_PLIST=/System/Library/CoreServices/SystemVersion.plist
say() { echo "$(date '+%F %T') cryptex-place: $*"; }

running_build() { /usr/bin/plutil -extract ProductBuildVersion raw -o - "$VERSION_PLIST" 2>/dev/null; }

entries() { ls -A "$1" >/dev/null 2>&1 || { echo unknown; return; }; ls -A "$1" 2>/dev/null | wc -l | tr -d ' '; }

place_one() {
  local name=$1 src="$CRYPTEXES/$STAGE_NAME/$1" dst="$CRYPTEXES/$1" n
  [ -d "$src" ] && [ ! -L "$src" ] || return 0
  if [ -L "$dst" ]; then say "$name: $dst is a link, not a folder; the staged content is left where it is"; return 1; fi
  if [ -d "$dst" ]; then
    n=$(entries "$dst")
    if [ "$n" != 0 ]; then
      say "$name: $dst already holds $n entries and is not replaced; the staged content is left where it is"
      return 1
    fi
    if ! rmdir "$dst" 2>/dev/null; then say "$name: the empty $dst could not be replaced (read-only?); the staged content is left where it is"; return 1; fi
  elif [ -e "$dst" ]; then
    say "$name: $dst is not a folder; the staged content is left where it is"; return 1
  fi
  if mv "$src" "$dst" 2>/dev/null && [ -d "$dst" ] && [ ! -e "$src" ]; then
    say "$name: in place ($(entries "$dst") entries at its top)"
    return 0
  fi
  [ -e "$dst" ] || mkdir "$dst" 2>/dev/null
  say "$name: the staged content could not be renamed to $dst; it is left where it is"
  return 1
}

cryptex_place_main() {
  local stage="$CRYPTEXES/$STAGE_NAME" want have bad=0 name
  [ -d "$stage" ] && [ ! -L "$stage" ] || return 0          # nothing staged: the usual case after the first start
  if [ ! -f "$stage/ready" ]; then say "content is staged but not marked complete (no ready file); not touched"; return 0; fi
  want=$(head -n 1 "$stage/ready" 2>/dev/null | tr -d ' \n')
  have=$(running_build)
  if [ -z "$have" ] || [ "$want" != "$have" ]; then
    say "staged content is for build ${want:-unknown} and this system is ${have:-unreadable}; not touched"
    return 0
  fi
  for name in OS App; do place_one "$name" || bad=$((bad + 1)); done
  if [ "$bad" = 0 ]; then
    rm -f "$stage/ready"; rmdir "$stage" 2>/dev/null || say "the stage folder holds something else and stays: $stage"
  fi
  return 0
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  case "${1:-}" in
    *) cryptex_place_main "$@";;
  esac
fi

