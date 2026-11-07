#!/bin/bash
# metal-cycle with WindowServer allowed, then record WindowServer's own os_log while it restarts on the GPU, and print the
# compositor decision lines. Usage: ws-log-cycle.sh <image>
cd /Volumes/datos2/unixnu-akita
scratch/metal-cycle.sh "$1" "metaldiscover WindowServer" | tail -4
scratch/akx 'nohup log stream --level debug --style compact --predicate "process == \"WindowServer\"" > /tmp/wslog3.txt 2>&1 < /dev/null & sleep 3; kill -9 $(pgrep -x WindowServer); echo WindowServer killed' 30
sleep 75
scratch/akx 'pkill -f "log stream"; wc -l /tmp/wslog3.txt; grep -naiE "compositor|absent|suitable|Metal|display manager|BootSet|NativeMali|accelerat|CAWS|safe" /tmp/wslog3.txt | cut -c1-240 | head -80' 60
scratch/akx 'ps -o pid,pcpu,rss,command -p $(pgrep -x WindowServer)' 20
echo "DONE $(date +%T)"
