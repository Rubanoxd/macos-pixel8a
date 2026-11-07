#!/bin/bash
# metal-cycle (boot <image>, Mali level 9, plugin, metaldiscover) with WindowServer allowed, then restart WindowServer on the GPU and
# print what the plugin/frontend/kernel said. Usage: ws-cycle.sh <image>
cd /Volumes/datos2/unixnu-akita
scratch/metal-cycle.sh "$1" "metaldiscover WindowServer" | tail -4
wc -c < scratch/live/metal.log | tr -d ' ' > scratch/live/ws-offset
scratch/akx 'kill -9 $(pgrep -x WindowServer); echo WindowServer killed' 30
sleep 90
tail -c +$(cat scratch/live/ws-offset) scratch/live/metal.log | grep -avE "^launchd\|" | grep -aiE "nativemali|native-metal|mali|metal|WindowServer|panic|platform code|MACF" | grep -v "^SHENG: fd2 pid [0-9]* WindowServer -  $" | cut -c1-260 | head -80
scratch/akx 'ps -o pid,pcpu,rss,command -p $(pgrep -x WindowServer); pgrep -lx Dock; pgrep -lx Finder' 30
echo "DONE $(date +%T)"
