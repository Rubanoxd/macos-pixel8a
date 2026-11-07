#!/bin/bash
# Unattended binary search of the XNU hang on the Pixel 8a.
# Waits for fastboot, checks that a probe pass comes back to fastboot quickly (warm reset), then
# bisects the main-path calls between a known pass and a known hang. Stops on anything that
# would leave the phone in Android (it cannot come back from there without the buttons).
#   akita-autobisect.sh SITES_FILE
set -u
ROOT=/Volumes/datos2/unixnu-akita
LOG=$ROOT/autobisect.log
# Main-path call sites in boot order, one "CALL FUNCTION OCCURRENCE" per line, from the file
# given as $1. The first site is a known pass. Conditional sites must be left out, since a call
# that never runs would look like a hang.
SITES_FILE=${1:?sites file}
SITES=()
while read -r line; do [ -n "$line" ] && SITES+=("$line"); done < "$SITES_FILE"
LO=0                       # known pass
HI=$((${#SITES[@]} - 1))   # tested first

say() { echo "$(date +%T) $*" | tee -a "$LOG"; }

step() {   # step INDEX -> sets R to pass|hang|stop
  local out
  out=$("$ROOT/akita-bisect.sh" ${SITES[$1]} 2>&1)
  echo "$out" >> "$LOG"
  case "$out" in
    *"RESULT"*"passed (via android)"*) R=stop ;;
    *"RESULT"*": passed ("*) R=pass ;;
    *"RESULT"*": hung ("*) R=hang ;;
    *) R=stop ;;
  esac
  say "site $1 (${SITES[$1]}): $R"
}

say "waiting for fastboot"
until fastboot devices | grep -q .; do sleep 5; done

# The last site first: when it passes, this whole stretch boots and there is nothing to bisect.
step $HI
case $R in
  pass) say "DONE: everything up to ${SITES[$HI]} passes"; exit 0 ;;
  hang) ;;
  *) say "STOP at site $HI"; exit 1 ;;
esac

while [ $((HI - LO)) -gt 1 ]; do
  MID=$(((LO + HI) / 2))
  step $MID
  case $R in
    pass) LO=$MID ;;
    hang) HI=$MID ;;
    *) say "STOP at site $MID"; exit 1 ;;
  esac
done
say "DONE: passes after ${SITES[$LO]}, hangs before ${SITES[$HI]} returns"
