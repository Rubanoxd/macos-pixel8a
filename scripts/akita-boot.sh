#!/bin/bash
# Boots a macOS image for the Pixel 8a from RAM (fastboot boot) and gets everything ready to work without a camera:
#   1. waits for the phone in fastboot and boots the image
#   2. waits for the USB interface (CDC-ECM gadget) and saves the live kernel log (akita-live.py log)
#   3. waits for the input script to finish (Terminal open with  nc -l 2323|bash|nc -l 2324 )
#   4. leaves sshd ready (akita-usb-shell.sh setup)
# Usage: akita-boot.sh <image dir, e.g. out/akita-macos-live3>  [label]
set -u
BASE=/Volumes/datos2/unixnu-akita
FS=$BASE/ref/macOS-ForSheng
IMG=${1:?imagen}
case "$IMG" in /*) ;; *) IMG=$FS/$IMG;; esac
TAG=${2:-$(basename "$IMG")}
LOG=$BASE/scratch/live/kernel-$TAG-$(date +%H%M).log
mkdir -p "$BASE/scratch/live"
until fastboot devices | grep -q fastboot; do sleep 3; done
echo "arranque $(date +%H:%M:%S) $IMG"
fastboot boot "$IMG/akita-macos26-boot.img" 2>&1 | tail -1
IF=""
for _ in $(seq 1 240); do
  IF=$(for i in $(ifconfig -l); do ifconfig "$i" 2>/dev/null | grep -q "ether 02:a5:1a:00:00:01" && echo "$i"; done | head -1)
  [ -n "$IF" ] && ifconfig "$IF" | grep -q "inet " && break
  sleep 5
done
[ -z "$IF" ] && { echo "no USB interface"; exit 1; }
echo "red $IF $(date +%H:%M:%S)"
pkill -f "akita-live.py log" 2>/dev/null
nohup python3 "$BASE/akita-live.py" log "$LOG" > /dev/null 2>&1 &
sleep 2; python3 "$BASE/akita-live.py" replay
echo "log en $LOG"
for _ in $(seq 1 180); do grep -aq "script done" "$LOG" && break; sleep 10; done
echo "terminal $(date +%H:%M:%S)"
sleep 5
"$BASE/akita-usb-shell.sh" setup 2>&1 | tail -1
