#!/bin/bash
# metal-cycle with WindowServer allowed and the plugin's forced Metal compositor, then restart WindowServer and print what the
# plugin saw (SkyLight statics, device calls) and the compositor line. Usage: ws-metal-cycle.sh <image>
cd /Volumes/datos2/unixnu-akita
scratch/metal-cycle.sh "$1" "metaldiscover WindowServer" | tail -3
scratch/akx 'touch /System/Volumes/Data/ak/ws-force-metal; : > /tmp/nativemali-WindowServer.log; kill -9 $(pgrep -x WindowServer); echo WindowServer killed' 20
sleep 60
scratch/akx 'grep -E "ws |dev |error" /tmp/nativemali-WindowServer.log | head -60; P=$(pgrep -x WindowServer); log show --last 2m --style compact --predicate "processID == $P AND eventMessage CONTAINS[c] \"compositor\"" | tail -2; ps -o pcpu=,rss= -p $P' 90
echo "DONE $(date +%T)"
