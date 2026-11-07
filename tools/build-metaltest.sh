#!/bin/bash
# Link a macOS arm64e program against the UniXNU native Metal frontend + metal2vulkan bridge (pin 624bab39), with the arm64e PAC fixes
# UniXNU's plugin build applies (renamed callback imports -> RustPacBoundary.c wrappers, post-link indirect-branch rewrite).
# Usage: build-metaltest.sh <source.mm> <output>
set -euo pipefail
SRC=$1; OUT=$2
U=/Volumes/datos2/unixnu-akita/UniXNU
M=/Volumes/datos2/unixnu-akita/mesa-build
W=$M/build/metaltest; mkdir -p $W
BRIDGE=$U/build/m2v-target/arm64e-apple-darwin/release/libadreno_metal_bridge.a
SDK=$(xcrun --show-sdk-path)
BASE=(clang -arch arm64e -mmacosx-version-min=26.0 -isysroot $SDK -O2 -g)
/opt/homebrew/opt/llvm/bin/llvm-objcopy --redefine-sym=_pthread_create=_nmb_pac_pthread_create --redefine-sym=__tlv_atexit=_nmb_pac_tlv_atexit \
  --redefine-sym=_atexit=_nmb_pac_atexit --redefine-sym=__dyld_register_func_for_add_image=_nmb_pac_dyld_register_func_for_add_image \
  --redefine-sym=__Unwind_Backtrace=_nmb_pac_Unwind_Backtrace --redefine-sym=_dlsym=_nmb_pac_dlsym $BRIDGE $W/libadreno_metal_bridge_pac.a
"${BASE[@]}" -std=c11 -I $M/src/mesa-akita-main/include -c $U/drivers/AdrenoMetal/bridge/RustPacBoundary.c -o $W/RustPacBoundary.o
clang++ -arch arm64e -mmacosx-version-min=26.0 -isysroot $SDK -O2 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations \
  -I $U/drivers/AdrenoMetal -I $M/src/mesa-akita-main/include \
  -x objective-c++ $SRC $U/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm -x none $W/libadreno_metal_bridge_pac.a $W/RustPacBoundary.o \
  -Wl,-map,$W/$(basename $OUT).map -Wl,-dead_strip -lobjc -framework Foundation -framework Metal -framework IOSurface -o $OUT
python3 $U/scripts/rust-pac-rewrite.py $OUT $W/$(basename $OUT).map libadreno_metal_bridge_pac.a
strip -x $OUT
codesign -s - -f $OUT
ls -la $OUT
