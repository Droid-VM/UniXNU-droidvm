#!/usr/bin/env bash
# Build DarwinBoot.efi on its own. On a Mu-Silicium board it is part of the firmware volume; a DroidVM
# guest boots DroidVM's edk2-gunyah instead, so UniXNU loads DarwinBoot from the payload disk.
#
#   scripts/build-darwinboot-efi.sh TREE EDK2 [DEBUG|RELEASE]
#
# TREE is out/<board>/tree of the unixnu driver, EDK2 the Mu_Basecore checkout "unixnu build package"
# used (out/feeds/mu_basecore-*). The environment follows UniXNU's package/build.sh, so the BaseTools
# and Conf of that step are reused. The result is <TREE>/../package/Build/Out/AARCH64-<TARGET>/DarwinBoot.efi.
set -euo pipefail
TREE=$(cd "$1" && pwd)
EDK2=$(cd "$2" && pwd)
TARGET=${3:-DEBUG}
BUILD_DIR=$(cd "$TREE/.." && pwd)/package
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}

if [ ! -x "$EDK2/BaseTools/Source/C/bin/GenFw" ]; then
  echo "error: no BaseTools in $EDK2; run \"unixnu build package\" first" >&2
  exit 1
fi
export WORKSPACE=$BUILD_DIR
export PACKAGES_PATH=$TREE/drivers/DarwinBoot:$TREE/package:$EDK2
export EDK_TOOLS_PATH=$EDK2/BaseTools
export CONF_PATH=$BUILD_DIR/Build/Conf
export PYTHON_COMMAND=${PYTHON_COMMAND:-python3}
export PATH=$EDK_TOOLS_PATH/BinWrappers/PosixLike:$EDK_TOOLS_PATH/Source/C/bin:$PATH
if [ -z "${CLANG_BIN:-}" ]; then
  CLANG_BIN=$(dirname "$(command -v clang)")/
fi
# CLANGPDB calls $(CLANG_BIN)lld-link. Homebrew ships lld apart from the keg-only llvm, so when the clang
# directory has no lld-link, link both into one toolchain directory.
if [ ! -x "${CLANG_BIN}lld-link" ]; then
  LLD_LINK=$(command -v lld-link) || { echo "error: lld-link not found (brew install lld)" >&2; exit 1; }
  TOOLCHAIN=$BUILD_DIR/toolchain
  mkdir -p "$TOOLCHAIN"
  for tool in "$CLANG_BIN"*; do
    ln -sf "$tool" "$TOOLCHAIN/$(basename "$tool")"
  done
  ln -sf "$LLD_LINK" "$TOOLCHAIN/lld-link"
  CLANG_BIN=$TOOLCHAIN/
fi
export CLANG_BIN
if [ ! -f "$CONF_PATH/tools_def.txt" ]; then
  echo "error: no EDK II Conf in $CONF_PATH; run \"unixnu build package\" first" >&2
  exit 1
fi
build -p DarwinBootPkg/DarwinBoot.dsc -a AARCH64 -b "$TARGET" -t CLANGPDB -n "$JOBS"
OUT=$BUILD_DIR/Build/Out/AARCH64-$TARGET
mkdir -p "$OUT"
# DarwinBoot.dsc sets its own OUTPUT_DIRECTORY (build/DarwinBoot, lower case) under the workspace.
EFI=$(find "$BUILD_DIR" -path "*/DarwinBoot/${TARGET}_CLANGPDB/AARCH64/DarwinBoot.efi" | head -1)
if [ -z "$EFI" ]; then
  echo "error: the build left no DarwinBoot.efi" >&2
  exit 1
fi
cp "$EFI" "$OUT/DarwinBoot.efi"
ls -l "$OUT/DarwinBoot.efi"
