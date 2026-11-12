#!/bin/bash
# darwin-drm for macOS arm64e -> mesa-build/build/darwin-drm-arm64e/libdrm_darwin.a, then PanVK relinked (ninja does not see the .a).
set -e
B=/Volumes/datos2/unixnu-akita/mesa-build; SHIM=/Volumes/datos2/unixnu-akita/ref/mali/MaliMetal/darwin-drm; MALI=/Volumes/datos2/unixnu-akita/ref/mali/Mali
O=$B/build/darwin-drm-arm64e; SDK=$(xcrun --show-sdk-path)
clang -arch arm64e -mmacosx-version-min=26.0 -isysroot $SDK -O2 -fPIC -Wall -I$SHIM/include -I$B/src/mesa-akita/include -I$MALI -c $SHIM/drm_darwin.c -o $O/drm_darwin.o
rm -f $O/libdrm_darwin.a; ar rcs $O/libdrm_darwin.a $O/drm_darwin.o
echo "drm ok"
