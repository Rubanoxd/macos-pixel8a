#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# macOS 26.5 on the Pixel 8a (akita) - port by @1421rubano
# akita (Generated-by: LLM): build Google's DP CAL + Synopsys USB/DP PHY + DECON/DPP CAL (as laid out by fetch-google-sources.sh)
# and our glue into one relocatable object, AkitaDpCal.o, and optionally merge it into AkitaAcpm.o (the ACPM kernel module that
# includes AkitaDp.inc). Compile AkitaAcpm.cpp first; merging twice is refused.
#   usage: build-dp-cal.sh <work-dir> [AkitaAcpm.o]
#   CLANG=/path/to/clang (default: xcrun clang)
set -euo pipefail
WORK=${1:?work dir from fetch-google-sources.sh}
ACPM_O=${2:-}
CLANG=${CLANG:-$(xcrun -f clang)}
OUT=$WORK/obj; mkdir -p "$OUT"
COMMON=(-xc -std=gnu11 -arch arm64e -mkernel -nostdlibinc -fno-builtin -ffreestanding -O2 -mmacos-version-min=26.5
        -I ../shim -I . -include ../shim/akita_linux_shim.h -Wno-everything)
( cd "$WORK/cal" && for f in cal_9865_dp_reg phy-exynos-snps-usbdp akita_dp_glue; do
    "$CLANG" "${COMMON[@]}" -c $f.c -o "$OUT/$f.o"; done )
( cd "$WORK/dpu" && for f in cal_9865_decon_reg cal_9865_dpp_reg akita_dpu_glue; do
    "$CLANG" "${COMMON[@]}" -include ../shim/drm/drm_dsc.h -include ../shim/akita_dpu_enums.h -c $f.c -o "$OUT/$f.o"; done )
ld -r -arch arm64e "$OUT"/cal_9865_dp_reg.o "$OUT"/phy-exynos-snps-usbdp.o "$OUT"/akita_dp_glue.o \
   "$OUT"/cal_9865_decon_reg.o "$OUT"/cal_9865_dpp_reg.o "$OUT"/akita_dpu_glue.o -o "$OUT/AkitaDpCal.o"
echo "undefined in AkitaDpCal.o (the kernel provides them):"; nm -u "$OUT/AkitaDpCal.o"
if [ -n "$ACPM_O" ]; then
  if nm "$ACPM_O" | grep -q ' T _akita_dp_setup'; then
    echo "$ACPM_O already has the DP CAL: recompile AkitaAcpm.cpp first" >&2; exit 1
  fi
  ld -r -arch arm64e "$ACPM_O" "$OUT/AkitaDpCal.o" -o "$OUT/AkitaAcpm-merged.o" && mv "$OUT/AkitaAcpm-merged.o" "$ACPM_O"
  echo "merged into $ACPM_O"
fi
