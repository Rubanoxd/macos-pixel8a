#!/bin/bash
# One bisection step on the Pixel 8a: reset after CALL in FUNCTION (occurrence N), boot, report.
#   akita-bisect.sh CALL [FUNCTION] [OCCURRENCE]
# "passed" = the probe's PSCI reset fired (quick return to fastboot); "hung" = the akita
# firmware's watchdog fired (back in fastboot after ~136 s).
set -u
ROOT=/Volumes/datos2/unixnu-akita
CALL=${1:?call}
IN=${2:-_arm_init}
OCC=${3:-1}
BOARD=$ROOT/UniXNU-akita/board.toml

python3 - "$BOARD" "$CALL" "$IN" "$OCC" <<'EOF'
import re, sys
path, call, fn, occ = sys.argv[1:]
s = open(path).read()
s = re.sub(r'^entry_probe_reset_(after|in|occurrence) = .*\n', '', s, flags=re.M)
s = s.replace('entry_probe_reset_delay = 6\n',
              f'entry_probe_reset_delay = 6\nentry_probe_reset_after = "{call}"\n'
              f'entry_probe_reset_in = "{fn}"\nentry_probe_reset_occurrence = {occ}\n', 1)
open(path, 'w').write(s)
EOF

"$ROOT/akita-payload.sh" > /dev/null || exit 1
grep patched "$ROOT/UniXNU/out/akita/ipados-17.7.11-21H461/akita/logs/payload-patch.log" | sed 's/.*stage 4/stage 4/'
result=$("$ROOT/akita-test.sh" "$ROOT/Mu-akita-boot-xnu.img" 240)
echo "$result" | tail -1
# A probe reset writes the bootloader reboot mode first, so a pass is a quick return to fastboot;
# a hang is the firmware watchdog (~136 s). Android means the reboot mode was lost (still a pass).
secs=$(echo "$result" | sed -n 's/.*back in fastboot after \([0-9]*\) s.*/\1/p')
case "$result" in
  *"android booted"*) echo "RESULT $CALL in $IN #$OCC: passed (via android)" ;;
  *"back in fastboot"*)
    if [ "$secs" -lt 110 ]; then echo "RESULT $CALL in $IN #$OCC: passed ($secs s)"
    else echo "RESULT $CALL in $IN #$OCC: hung ($secs s)"; fi ;;
  *) echo "RESULT $CALL in $IN #$OCC: unknown" ;;
esac
timeout 60 fastboot oem dmesg 2>&1 | grep -A3 'Reboot Info' | head -8
