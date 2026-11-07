#!/bin/bash
# NativeMaliMetal.bundle for macOS arm64e: plugin + UniXNU frontend + metal2vulkan bridge (PAC-fixed) + PanVK inside the bundle.
set -euo pipefail
U=/Volumes/datos2/unixnu-akita/UniXNU
M=/Volumes/datos2/unixnu-akita/mesa-build
W=$M/build/plugin; OUT=$M/plugin/out; B=$OUT/NativeMaliMetal.bundle
rm -rf $OUT; mkdir -p $W $B
BRIDGE=$U/build/m2v-target/arm64e-apple-darwin/release/libadreno_metal_bridge.a
SDK=$(xcrun --show-sdk-path)
/opt/homebrew/opt/llvm/bin/llvm-objcopy --redefine-sym=_pthread_create=_nmb_pac_pthread_create --redefine-sym=__tlv_atexit=_nmb_pac_tlv_atexit \
  --redefine-sym=_atexit=_nmb_pac_atexit --redefine-sym=__dyld_register_func_for_add_image=_nmb_pac_dyld_register_func_for_add_image \
  --redefine-sym=__Unwind_Backtrace=_nmb_pac_Unwind_Backtrace --redefine-sym=_dlsym=_nmb_pac_dlsym $BRIDGE $W/libadreno_metal_bridge_pac.a
clang -arch arm64e -mmacosx-version-min=26.0 -isysroot $SDK -O2 -g -std=c11 -I $M/src/mesa-akita-main/include \
  -c $U/drivers/AdrenoMetal/bridge/RustPacBoundary.c -o $W/RustPacBoundary.o
clang++ -arch arm64e -mmacosx-version-min=26.0 -isysroot $SDK -O2 -g -std=c++17 -fobjc-arc -Wno-deprecated-declarations -bundle \
  -I $U/drivers/AdrenoMetal -I $M/src/mesa-akita-main/include \
  -x objective-c++ $M/plugin/NativeMaliMetalPlugin.mm $U/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm -x none \
  $W/libadreno_metal_bridge_pac.a $W/RustPacBoundary.o -Wl,-map,$W/NativeMaliMetal.map -Wl,-dead_strip \
  -lobjc -framework Foundation -framework Metal -framework IOSurface -o $B/NativeMaliMetal 2>&1 | grep -vE "built for newer|duplicate libraries" || true
python3 $U/scripts/rust-pac-rewrite.py $B/NativeMaliMetal $W/NativeMaliMetal.map libadreno_metal_bridge_pac.a
strip -x $B/NativeMaliMetal; rm -rf $W/NativeMaliMetal.dSYM; mv $B/NativeMaliMetal.dSYM $W/ 2>/dev/null || true
cp $M/libvulkan_panfrost.dylib $B/libvulkan_panfrost.dylib
python3 - $B/Info.plist <<'EOF'
import plistlib, sys
open(sys.argv[1], 'wb').write(plistlib.dumps(dict(
    CFBundleIdentifier='org.akita.NativeMaliMetal', CFBundleExecutable='NativeMaliMetal', CFBundlePackageType='BNDL',
    CFBundleVersion='1', CFBundleShortVersionString='1', NSPrincipalClass='NativeMaliMetalDevice',
    LSMinimumSystemVersion='26.0'), fmt=plistlib.FMT_XML))
EOF
codesign -s - -f $B/libvulkan_panfrost.dylib
codesign -s - -f $B/NativeMaliMetal
codesign -s - -f $B
( cd $OUT && tar -cf NativeMaliMetal.bundle.tar NativeMaliMetal.bundle )
ls -la $B $OUT
