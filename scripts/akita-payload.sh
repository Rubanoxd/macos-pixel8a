#!/bin/bash
# Rebuild the akita payload and pack it into Mu-akita-boot-xnu.img.
set -euo pipefail
ROOT=/Volumes/datos2/unixnu-akita
cd "$ROOT/UniXNU"
export SSL_CERT_FILE="$ROOT/.venv/lib/python3.13/site-packages/certifi/cacert.pem"
# Probe resets set the Tensor reboot mode to bootloader first, so they land in fastboot.
export UNIXNU_PROBE_RESET_PREP_SMC=0x82000504,0x15460810,1,0xfc
export UNIXNU_PROBE_RESET_WARM=1
# The build tree keeps its own copy of the scripts; keep the probe patcher current.
for t in out/akita/ipados-17.7.11-21H461/akita/tree out/akita/tree; do
  [ -d "$t/scripts" ] && cp scripts/patch-xnu-entry-probe.py "$t/scripts/"
done
UNIXNU_UEFI="$ROOT/Mu-Silicium" ./unixnu build payload -D ../UniXNU-akita \
  --kernelcache out/ipsw/21H461/kernelcache.decompressed --skip-prelink \
  --trust-cache ../payload-inputs/original-static-system.bundle --unique-chip-id 0ab78e30a848 --data-ramdisk "$ROOT/data-seed/data.dbr" "$@" \
  > "$ROOT/payload-akita.log" 2>&1 || { tail -20 "$ROOT/payload-akita.log"; exit 1; }
grep -E 'payload\.img|kernel args' "$ROOT/payload-akita.log" | cut -c1-200
cd "$ROOT"
python3 UniXNU-akita/scripts/akita-bootimg.py --mu Mu-Silicium/Mu-akita-0.img --stock factory/stock/boot.img \
  --kernel-payload UniXNU/out/akita/ipados-17.7.11-21H461/akita/payload/payload.img -o Mu-akita-boot-xnu.img
