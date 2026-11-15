#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# macOS 26.5 on the Pixel 8a (akita) - port by @1421rubano
# akita (Generated-by: LLM): download the Google/AOSP sources the DisplayPort and DECON/DPP code is built from (GPL-2.0, not
# kept in this repo), lay them out as build-dp-cal.sh expects and apply our patches (google-patches/).
#   usage: fetch-google-sources.sh [work-dir]     (default: ./build-dp)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${1:-$PWD/build-dp}
GITILES=https://android.googlesource.com
# pinned revisions (branch android-gs-akita-5.15-android15-qpr1 for the Pixel modules, android14-5.15 for the common kernel)
DISPLAY_REV=13ee4944a0a8db6b2c3997db515089eee84f161c   # kernel/google-modules/display
GS_REV=30a388f76a31747f24ef6caa21d72733a918723a        # kernel/google-modules/soc/gs
COMMON_REV=8adccf9a3b0e2fbca627b652f98cc069e1a30875    # kernel/common

get() {   # get <repo> <rev> <path> <destination>
  mkdir -p "$(dirname "$4")"
  curl -fsS -m 120 "$GITILES/$1/+/$2/$3?format=TEXT" | base64 --decode > "$4"
}

rm -rf "$WORK"
mkdir -p "$WORK/cal" "$WORK/dpu/drm" "$WORK/shim"
cp -R "$HERE/shim/." "$WORK/shim/"

D=kernel/google-modules/display; S=samsung
# DP link (cal) + Synopsys USB/DP combo PHY (gs)
get $D $DISPLAY_REV $S/cal_9865/dp_reg.c                  "$WORK/cal/cal_9865_dp_reg.c"
get $D $DISPLAY_REV $S/cal_9865/regs-dp.h                 "$WORK/cal/regs-dp.h"
get $D $DISPLAY_REV $S/cal_9865/regs-usbdpphy_ctrl.h      "$WORK/cal/regs-usbdpphy_ctrl.h"
get $D $DISPLAY_REV $S/cal_9865/regs-usbdpphy_tca_ctrl.h  "$WORK/cal/regs-usbdpphy_tca_ctrl.h"
get $D $DISPLAY_REV $S/cal_common/dp_cal.h                "$WORK/cal/dp_cal.h"
G=kernel/google-modules/soc/gs; P=drivers/phy/samsung
for f in phy-exynos-snps-usbdp.c snps-usbdp-con-reg.h snps-usbdp-tca-reg.h phy-samsung-usb-cal.h snps-usbdp-ram-code.h; do
  get $G $GS_REV $P/$f "$WORK/cal/$f"
done
# DECON + DPP
get $D $DISPLAY_REV $S/cal_9865/decon_reg.c "$WORK/dpu/cal_9865_decon_reg.c"
get $D $DISPLAY_REV $S/cal_9865/dpp_reg.c   "$WORK/dpu/cal_9865_dpp_reg.c"
for f in regs-decon.h regs-dpp.h regs-dqe.h regs-hdr.h; do get $D $DISPLAY_REV $S/cal_9865/$f "$WORK/dpu/$f"; done
for f in decon_cal.h dpp_cal.h dqe_cal.h hdr_cal.h exynos_dpp_coef.h exynos_hdr_lut.h exynos_panel.h; do
  get $D $DISPLAY_REV $S/cal_common/$f "$WORK/dpu/$f"
done
for f in exynos_drm_format.h exynos_drm_plane.h; do get $D $DISPLAY_REV $S/$f "$WORK/$f"; done
get $D $DISPLAY_REV $S/include/uapi/drm/samsung_drm.h      "$WORK/dpu/drm/samsung_drm.h"
get $D $DISPLAY_REV $S/include/uapi/drm/drm_fourcc_gs101.h "$WORK/dpu/drm/drm_fourcc_gs101.h"
get kernel/common $COMMON_REV include/uapi/drm/drm_fourcc.h "$WORK/dpu/drm/drm_fourcc.h"
sed -i.bak 's|#include "drm.h"|#include <drm/drm.h>|' "$WORK/dpu/drm/drm_fourcc.h" && rm "$WORK/dpu/drm/drm_fourcc.h.bak"
# drm_dsc.h: the real Linux header (DECON's DSC structs), wrapped for the shim
get kernel/common $COMMON_REV include/drm/drm_dsc.h "$WORK/drm_dsc.linux.h"
{
  echo '#ifndef AKITA_DRM_DSC_H'; echo '#define AKITA_DRM_DSC_H'; echo '#include "../akita_linux_shim.h"'
  echo 'struct drm_printer { int unused; };'
  echo 'struct dp_sdp_header { u8 HB0, HB1, HB2, HB3; } __attribute__((packed));'
  echo '#define drm_printf(p, ...) printf(__VA_ARGS__)'
  sed 's|#include <drm/drm_dp_helper.h>||' "$WORK/drm_dsc.linux.h"
  echo '#endif'
} > "$WORK/shim/drm/drm_dsc.h"
rm "$WORK/drm_dsc.linux.h"

# our glue and changes
cp "$HERE/glue/akita_dp_glue.c" "$WORK/cal/"
cp "$HERE/glue/akita_dpu_glue.c" "$WORK/dpu/"
cp -R "$HERE/glue/trace" "$WORK/dpu/"
cp "$HERE/glue/drm/drm.h" "$WORK/dpu/drm/"
(cd "$WORK/cal" && patch -p1 --batch --fuzz=0 < "$HERE/google-patches/cal_9865_dp_reg.c.patch" \
                && patch -p1 --batch --fuzz=0 < "$HERE/google-patches/phy-exynos-snps-usbdp.c.patch")
(cd "$WORK/dpu" && patch -p1 --batch --fuzz=0 < "$HERE/google-patches/cal_9865_dpp_reg.c.patch")
echo "Google sources ready in $WORK (build with build-dp-cal.sh $WORK)"
