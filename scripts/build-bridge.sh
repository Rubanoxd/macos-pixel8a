#!/bin/bash
# The metal2vulkan bridge for macOS arm64e (UniXNU/build/m2v-target/arm64e-apple-darwin/release/libadreno_metal_bridge.a), as the
# plugin links it. The runtime id stays the pinned+recorded-patches one (it keys the shader cache): local akita changes on top of the
# patched source keep the seeded cache valid. Then: bash mesa-build/plugin/build-plugin.sh and the addon.
set -euo pipefail
source /Volumes/datos2/unixnu-akita/rust/env.sh
B=/Volumes/datos2/unixnu-akita/UniXNU/build/adreno-metal-bridge
cd $B
export NMB_RUNTIME_ID=5571676ad9ebf4228d6e05b2489d8aeb NMB_FIXTURE_AIR=$B/runtime_examples.air NMB_FC_AIR=$B/function_constant.air \
  NMB_SCANOUT_AIR=$B/scanout_convert.air MACOSX_DEPLOYMENT_TARGET=26.0 RUSTC_BOOTSTRAP=1 TMPDIR=/Volumes/datos2/tmp
cargo +1.98.0 -Z build-std=std,panic_unwind build --release --manifest-path Cargo.toml --target arm64e-apple-darwin \
  --target-dir ../m2v-target > $B/build-macos-arm64e.log 2>&1 || { tail -40 $B/build-macos-arm64e.log; exit 1; }
tail -2 $B/build-macos-arm64e.log
ls -la ../m2v-target/arm64e-apple-darwin/release/libadreno_metal_bridge.a
