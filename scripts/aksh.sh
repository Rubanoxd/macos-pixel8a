#!/bin/bash
# Run a command on the phone through the nc channel (input 2323); its output goes to /dev/console -> UDP kernel log.
F=/Volumes/datos2/unixnu-akita/scratch/live/${AKLOG:-fullos24}.log
TAG="AKSH$RANDOM"
start=$(wc -c < "$F")
nc 169.254.7.2 2324 > /dev/null 2>&1 & R=$!
sleep 1
( printf '{ echo %s-BEGIN; %s; echo %s-END; } 2>&1 | /usr/bin/sed "s/^/%s: /" > /dev/console\n' "$TAG" "$1" "$TAG" "$TAG"; sleep ${2:-4} ) | nc 169.254.7.2 2323 >/dev/null 2>&1
for i in $(seq 1 ${3:-20}); do tail -c +$((start+1)) "$F" | grep -aq "$TAG: $TAG-END" && break; sleep 1; done
kill $R 2>/dev/null
tail -c +$((start+1)) "$F" | grep -a "^$TAG: " | sed "s/^$TAG: //" | grep -v "^$TAG-"
