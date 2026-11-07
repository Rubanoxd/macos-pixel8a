#!/bin/bash
# Mesa main (27.0-devel) for akita: host tools (arm64, LLVM 23) then PanVK for macOS arm64e. Usage: build-mesa-main.sh [host|target|all]
set -u
B=/Volumes/datos2/unixnu-akita/mesa-build; SRC=$B/src/mesa-akita-main; cd $B
OPTS=(-Dvulkan-drivers=panfrost -Dgallium-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dopengl=false -Dgles1=disabled -Dgles2=disabled
      -Dtools= -Dvideo-codecs= -Dvulkan-layers= -Dxmlconfig=disabled -Dshader-cache=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Dlmsensors=disabled
      -Dzstd=disabled -Dbuild-tests=false -Dbuildtype=release -Dpanfrost-rust=false)
what=${1:-all}
if [ $what = host ] || [ $what = all ]; then
  ( . ./host-env.sh
    [ -f build/main-host/build.ninja ] || meson setup build/main-host $SRC --prefix=$B/prefix/main-host --wrap-mode=nodownload "${OPTS[@]}" \
      -Dllvm=enabled -Dshared-llvm=enabled -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true > main-host-setup.log 2>&1 || { echo host-setup-failed; exit 1; }
    ninja -C build/main-host -k 0 src/compiler/clc/mesa_clc src/panfrost/clc/panfrost_compile src/compiler/spirv/vtn_bindgen2 > main-host.log 2>&1; echo host=$?
    mkdir -p $B/prefix/main-host/bin; for t in src/compiler/clc/mesa_clc src/panfrost/clc/panfrost_compile src/compiler/spirv/vtn_bindgen2; do cp build/main-host/$t $B/prefix/main-host/bin/ 2>/dev/null && echo "tool $(basename $t)"; done )
fi
if [ $what = target ] || [ $what = all ]; then
  export PATH=$B/prefix/main-host/bin:$B/venv/bin:$PATH; unset PKG_CONFIG_PATH LLVM_CONFIG
  [ -f build/main-macos/build.ninja ] || meson setup build/main-macos $SRC --cross-file build/macos-arm64e.cross --wrap-mode=nodownload "${OPTS[@]}" \
      -Dllvm=disabled -Dmesa-clc=system -Dprecomp-compiler=system -Dexpat=disabled -Db_ndebug=true > main-macos-setup.log 2>&1 || { echo target-setup-failed; exit 1; }
  CCC_OVERRIDE_OPTIONS='# +-Wl,-undefined,dynamic_lookup' ninja -C build/main-macos -k 0 src/panfrost/vulkan/libvulkan_panfrost.dylib > main-macos.log 2>&1; echo target=$?
  D=build/main-macos/src/panfrost/vulkan/libvulkan_panfrost.dylib
  [ -f $D ] && cp $D $B/libvulkan_panfrost.dylib && python3 -I /Volumes/datos2/unixnu-akita/ref/mali/MaliMetal/mesa-build/fix-weak-imports.py $B/libvulkan_panfrost.dylib | tail -1
fi
