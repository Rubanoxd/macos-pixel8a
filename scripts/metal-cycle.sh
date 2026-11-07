#!/bin/bash
# Reset the phone to fastboot (watchdog sysctl), boot <image>, start Mali level 9 (IOAccelerator), union-mount a tmpfs over
# /System/Library/Extensions, install NativeMaliMetal.bundle, allow <clients>, run metaldiscover. Usage: metal-cycle.sh <image> [clients]
set -u
cd /Volumes/datos2/unixnu-akita
IMG=$1; CLIENTS=${2:-metaldiscover}
pkill -f "akita-live.py log"
if [ -z "${SKIP_BOOT:-}" ]; then
( printf 'sysctl -w kern.akita_fastboot=1\nexit\n'; sleep 5 ) | nc -G 3 -w 10 169.254.7.2 2325 >/dev/null 2>&1
until fastboot devices 2>/dev/null | grep -q fastboot; do sleep 2; done
echo "FASTBOOT $(date +%T)"
fastboot boot "$IMG" 2>&1 | tail -1
sleep 20
fi
until ifconfig | grep -q '02:a5:1a:00:00:01'; do sleep 2; done
echo "GADGET $(date +%T)"
sleep 15
TMPDIR=/Volumes/datos2/tmp python3 -u akita-live.py log scratch/live/metal.log > /dev/null 2>&1 &
until nc -G 2 -z 169.254.7.2 2325 2>/dev/null; do sleep 5; done
echo "SHELL $(date +%T)"
sleep 20
scratch/akx 'sysctl -w kern.akita_mali=9 >/dev/null; ioreg -c IOAccelerator -l | grep -E "Metal|class IOAccelerator"' 150
P=mesa-build/plugin/out/NativeMaliMetal.bundle.tar
ok=0; for i in 1 2; do scratch/akssh "mkdir /System/Volumes/Data/ak/gpu 2>/dev/null; cat > /System/Volumes/Data/ak/gpu/NativeMaliMetal.bundle.tar" < $P && ok=1 && break; sleep 5; done
[ $ok = 1 ] || scratch/akput $P /System/Volumes/Data/ak/gpu/NativeMaliMetal.bundle.tar 60
ok=0; for i in 1 2; do scratch/akssh "cat > /System/Volumes/Data/ak/gpu/metaldiscover" < mesa-build/test/metaldiscover && ok=1 && break; sleep 5; done
[ $ok = 1 ] || scratch/akput mesa-build/test/metaldiscover /System/Volumes/Data/ak/gpu/metaldiscover 30
scratch/akx "mount_tmpfs -o union -s 96m /System/Library/Extensions; cd /System/Library/Extensions && tar -xf /System/Volumes/Data/ak/gpu/NativeMaliMetal.bundle.tar && ls -la NativeMaliMetal.bundle; printf '%s\n' $CLIENTS > /System/Volumes/Data/ak/metal-clients; cat /System/Volumes/Data/ak/metal-clients" 60
scratch/akx 'cd /System/Volumes/Data/ak/gpu && chmod +x metaldiscover && ./metaldiscover 2>&1 | tail -10; cat /tmp/nativemali-metaldiscover.log 2>/dev/null | tail -10' 120
echo "DONE $(date +%T)"
