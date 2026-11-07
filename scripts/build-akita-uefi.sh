#!/bin/bash
# Builds the Mu-Silicium UEFI image for the Pixel 8a (akita) on macOS.
# Output: Mu-Silicium/Mu-akita.img (Android boot image v4 for `fastboot boot`).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
MODE="${1:-DEBUG}"

# CLANGPDB expects clang and lld-link side by side in CLANG_BIN. Homebrew ships
# lld separately from llvm, so link both into one toolchain directory.
TOOLCHAIN="$ROOT/.toolchain"
mkdir -p "$TOOLCHAIN"
for tool in /opt/homebrew/opt/llvm/bin/* /opt/homebrew/opt/lld/bin/*; do
  ln -sf "$tool" "$TOOLCHAIN/$(basename "$tool")"
done
export CLANG_BIN="$TOOLCHAIN/"
export PATH="$TOOLCHAIN:$ROOT/.venv/bin:$PATH"

# BaseTools: C utilities built natively from Mu_Basecore (make -C BaseTools/Source/C)
# and the Python front ends (build, GenFds, ...) through the PosixLike wrappers.
BASETOOLS="$ROOT/Mu-Silicium/Mu_Basecore/BaseTools"
export EDK_TOOLS_PATH="$BASETOOLS"
export EDK_TOOLS_BIN="$BASETOOLS/Source/C/bin"
export PYTHONPATH="$BASETOOLS/Source/Python${PYTHONPATH:+:$PYTHONPATH}"
export PATH="$BASETOOLS/BinWrappers/PosixLike:$EDK_TOOLS_BIN:$PATH"

cd "$ROOT/Mu-Silicium"
python3 build_uefi.py -d akita -r "$MODE" "${@:2}"

# The akita ABL only boots images that fill the boot partition and carry an AVB footer
# ("Boot image location and length mismatch" otherwise). akita-bootimg.py adds the stock
# boot.img vbmeta and a footer; set PAYLOAD to a UniXNU payload volume to pass it as the initrd.
STOCK_BOOT="$ROOT/factory/stock/boot.img"
python3 "$ROOT/UniXNU-akita/scripts/akita-bootimg.py" --mu "$ROOT/Mu-Silicium/Mu-akita-0.img" \
  --stock "$STOCK_BOOT" ${PAYLOAD:+--ramdisk "$PAYLOAD"} -o "$ROOT/Mu-akita-boot.img"
