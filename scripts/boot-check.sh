#!/bin/bash
N=${1:?usage: boot-check.sh N}
cd /Volumes/datos2/unixnu-akita
I=ref/macOS-ForSheng/out/akita-macos-fullos$N/akita-macos26-boot.img
pkill -f "akita-live.py log"
python3 akita-live.py fastboot >/dev/null 2>&1; ( printf 'sysctl -w kern.akita_fastboot=1\nexit\n'; sleep 5 ) | nc -G 3 -w 10 169.254.7.2 2325 >/dev/null 2>&1
for i in $(seq 90); do fastboot devices 2>/dev/null | grep -q fastboot && break; sleep 2; done
echo "FASTBOOT $(date +%T)"; fastboot boot $I 2>&1 | tail -1
sleep 20; ok=0
for i in $(seq 150); do ifconfig | grep -q '02:a5:1a:00:00:01' && { ok=1; break; }; sleep 2; done
[ $ok = 1 ] || { echo "NO GADGET in 5 min (UEFI window?)"; exit 1; }
echo "GADGET $(date +%T)"
# the phone's clock from the Mac's, then into the PMIC RTC so later boots have it without the Mac (kern.akita_acpm=3000000)
( sleep 90; scratch/akx "date -u $(date -u +%m%d%H%M%Y.%S) >/dev/null; sysctl -w kern.akita_acpm=3000000 >/dev/null" 20 >/dev/null 2>&1 ) &
TMPDIR=/Volumes/datos2/tmp nohup python3 -u akita-live.py log scratch/live/fullos$N.log > /dev/null 2>&1 &
sleep 360
scratch/akx 'sysctl hw.memsize hw.ncpu; vm_stat | head -4; who; pgrep -lf Dock.app | cut -c1-60; pgrep -lf Finder.app | cut -c1-60; ps -o %cpu= -p $(pgrep WindowServer); uptime' 25
