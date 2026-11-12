#!/bin/bash
N=${1:?usage: test-std.sh N}
cd /Volumes/datos2/unixnu-akita; L=scratch/live/fullos$N.log
bash scratch/boot-check.sh $N 2>&1 | grep -E "FASTBOOT|GADGET|NO GADGET"
python3 -c "import time; time.sleep(120)"
scratch/akx 'sysctl hw.memsize hw.memsize_usable; vm_stat | head -8; memory_pressure | tail -1' 15 | tail -11
grep -a "WINDOW\|window" $L | head -3 | cut -c1-160
python3 akita-live.py click 72 505 >/dev/null; sleep 2; python3 akita-live.py steps 'K11,08' >/dev/null; sleep 6
DRAG_OPEN=0 DRAG_X=450 DRAG_Y=57 python3 scratch/phase6/drag60.py 30
sleep 12
grep -a -o "NativeGOPDisplay: fps [0-9.]* ([0-9]* frames in [0-9]* ms).*pixels changed per copy" $L | tail -4 | cut -c1-120
grep -a -c "fatal 0x\|SHENG-LOG: panic\|exited due to SIGABRT | sent by WindowServer\|UFAULT" $L
