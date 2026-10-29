#!/usr/bin/env bash
# The iPadOS 27 VM of this rig (ipados27 board): fetch its boot disk and kernel symbols from the Mac and put the disk on the
# phone, beside the iPadOS 17 VM's own. No early probes: they rewrite __text, which the source corecrypto POST of this kernel
# hashes. kernel.nm comes from the native kernel, whose link addresses the fileset keeps.
#   deploy27.sh            the boot disk and symbols
#   deploy27.sh --images   also the raw OS and SystemOS disks (build-droidvm-root-policies.py)
set -euo pipefail
. "$(dirname "$0")/rigenv.sh"
RIG=$(cd "$(dirname "$0")" && pwd)
O=$RIG/out
MAC=${BUILD_HOST:?}
M=${BUILD_OUT:?}
PHONE=${PHONE:?}
DIR=/storage/emulated/0/DroidVM/UniXNU27
mkdir -p "$O"
scp -q "$MAC:$M/payload/droidvm-disk.img" "$O/droidvm-disk.img"
scp -q "$MAC:$M/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf" "$O/kernel.native.macho"
llvm-nm -n --defined-only "$O/kernel.native.macho" > "$O/kernel.nm"
export MTOOLS_SKIP_CHECK=1
mcopy -o -i "$O/droidvm-disk.img" ::/EFI/UniXNU/Payloads/kernel.macho.adt "$O/kernel.macho.adt"
mcopy -o -i "$O/droidvm-disk.img" ::/EFI/UniXNU/unixnu.toml "$O/unixnu.toml"
adb -s "$PHONE" shell "mkdir -p $DIR"
adb -s "$PHONE" push "$O/droidvm-disk.img" "$DIR/droidvm-disk.img" | tail -1
if [ "${1:-}" = --images ]; then
  for f in os.img systemos.img; do
    scp -q "$MAC:$M/tree/build/droidvm-root-images/$f" "$O/$f"
    adb -s "$PHONE" push "$O/$f" "$DIR/$f" | tail -1
  done
fi
adb -s "$PHONE" shell "su -c 'ls -la $DIR'"
