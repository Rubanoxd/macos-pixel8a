#!/bin/bash
# Full-OS akita image: compile changed kernel files, link, seal, dSYM, relink (NativeUfs + 14 roots), package.
# Usage: build-fullos.sh <N> [file.c ...]   (files are compile-<name>.sh scripts in scratch/fullos17)
set -euo pipefail
N=$1; shift
export TMPDIR=/Volumes/datos2/tmp
R=/Volumes/datos2/unixnu-akita/ref/macOS-ForSheng; X=$R/out/macos26-xnu-arm64e; C=/Volumes/datos2/unixnu-akita/scratch/fullos17
for f in "$@"; do bash $C/compile-$f.sh 2>&1 | grep -E " error:" && exit 1 || true; done
cd $X
# Mali.o comes in through LD_KERNEL_ARCHIVES, which make does not track: drop the linked kernel so it is always relinked.
rm -f obj/DEVELOPMENT_ARM64_SHENG/kernel.development.sheng.unstripped.noctf
make -C source SDKROOT=macosx KDKROOT=/Volumes/datos2/macos-25F71/kdk/KDK.pkg/Payload "TARGET_CONFIGS=DEVELOPMENT ARM64 SHENG" \
  ARCH_STRING_FOR_CURRENT_MACHINE_CONFIG=arm64e ARM64_LARGE_MEMORY=1 ARM64_LINK_ADDR=0xfffffe0007004000 RC_DARWIN_KERNEL_VERSION=25.5.0 \
  RC_ProjectSourceVersion=12377.121.6 "DEPLOYMENT_TARGET_FLAGS=-mmacosx-version-min=26.5 -DXNU_TARGET_OS_OSX" BUILD_LTO=0 BOUND_CHECKS=0 \
  BUILD_WERROR=1 MAKEJOBS=-j4 KERNEL_SDKROOT=$X/kernel-sdk OBJROOT=$X/obj SYMROOT=$X/sym DSTROOT=$X/kernel-sdk build BUILD_STATIC_LINK=1 \
  BUILD_XNU_LIBRARY=0 CFLAGS_KERNEL=-mkernel CXXFLAGS_KERNEL=-fapple-kext \
  "CFLAGS_EXTRA=-DXNU_NATIVE_ARCH_TIMER=1 -DXNU_DT_PANIC_REGION=1 -Wno-error=unused-but-set-variable -Wno-error=gnu-folding-constant -Wno-error=implicit-int-float-conversion -Wno-error=pointer-to-int-cast -Wno-error=void-pointer-to-int-cast -Wno-error=void-pointer-to-enum-cast" \
  "LD_KERNEL_LIBS=/Volumes/SATA-SSD/xcode/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/lib/clang/21/lib/darwin/libclang_rt.cc_kext.a" \
  "LD_KERNEL_ARCHIVES=-Wl,-force_load,$X/corecrypto/libcorecrypto-native.a $X/ArmDTPlatform.o $X/ArmCpu.o $X/Mali.o $X/kernel-sdk/usr/local/lib/kernel/libfirehose_kernel.a" \
  "NATIVE_KERNEL_LINK_FLAGS=-Wl,-headerpad,0x2000 -Wl,-map,$X/kernel.development.sheng.map" DO_CTFMERGE=0 > $C/link$N.log 2>&1
cp obj/DEVELOPMENT_ARM64_SHENG/kernel.development.sheng.unstripped.noctf kernel.development.sheng
python3 -c "import sys; sys.path.insert(0,'$R/scripts'); from seal_macos26_crypto import seal; from pathlib import Path; seal(Path('kernel.development.sheng'),'arm64e')"
rm -rf kernel.development.sheng.dSYM; xcrun dsymutil obj/DEVELOPMENT_ARM64_SHENG/kernel.development.sheng.unstripped.noctf -o kernel.development.sheng.dSYM
python3 - <<'EOF'
import json,hashlib,subprocess
sha=hashlib.sha256(open('kernel.development.sheng','rb').read()).hexdigest()
uuid=subprocess.check_output(['dwarfdump','--uuid','kernel.development.sheng'],text=True).split()[1]
for p in ['build.json','storage-driver/build.json','ufs-driver/build.json']:
    d=json.load(open(p))
    if p=='build.json' or 'executable_kernel_sha256' in d:
        d['executable_kernel_sha256']=sha; d['kernel_uuid']=uuid
        if p=='build.json': d['dsym_uuid']=uuid
        json.dump(d,open(p,'w'),indent=2)
print('kernel', uuid)
EOF
cd $R
python3 scripts/emit_macos26_security.py --kernel-build $X --storage-driver $X/ufs-driver --output out/akita-security-fullos$N --qemu-only \
  --include-module com.apple.iokit.IOMobileGraphicsFamily --include-module com.apple.iokit.IOSurface --include-module com.apple.iokit.IOAVFamily \
  --include-module com.apple.filesystems.lifs --include-module com.apple.filesystems.tmpfs --recovery-components --hid-components > /dev/null
BA=$(python3 -c "import json;print(json.load(open('out/akita-macos-fullos34/build.json'))['boot_args'])"); BA="${BA/akita-fullos-union-mb=512/akita-fullos-union-mb=${AKITA_UNION_MB:-1024}} akita-mali-platform=1 akita-csr=${AKITA_CSR:-0x24} akita-tfp=1 akita-epoch=$(date +%s)"
python3 platform/akita/package-akita-macos.py --kernel-build out/akita-security-fullos$N --root-hash private/macos26-media/root-hash.bin \
  --output out/akita-macos-fullos$N --cpus 8 --fullos-root-uuid 47719F50-5527-44A3-9F59-8170528D8C14 --nvram "prev-lang:kbd=es:87" --boot-args "$BA" | grep "left in"
python3 -c "import json;d=json.load(open('out/akita-security-fullos$N/security-relink.json'));assert d['storage_driver']['bundle']=='org.blu-sharky.NativeUfs' and len(d['roots'])==14;print('relink ok')"
strings out/akita-macos-fullos$N/kernel.macho.adt | grep -q 47719F50-5527-44A3-9F59-8170528D8C14 && echo "adt uuid ok"
echo "IMAGE out/akita-macos-fullos$N/akita-macos26-boot.img"
