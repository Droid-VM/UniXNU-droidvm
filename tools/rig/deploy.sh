#!/usr/bin/env bash
# Fetch the boot disk and kernel built on the Mac ("unixnu build payload" + make-droidvm-disk.sh, see
# README.md), put a copy of the kernel with the early exception catcher on
# the disk and push it to the phone.   deploy.sh [patch-early-probe.py args, e.g. --ubsan nop]
set -euo pipefail
. "$(dirname "$0")/rigenv.sh"
RIG=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$RIG/../..}   # the device repository (rig.env names it when the rig lives elsewhere)
O=$RIG/out
MAC=${BUILD_HOST:?set BUILD_HOST to the build Mac, user@host}
M=${BUILD_OUT:?set BUILD_OUT to out/droidvm of the UniXNU checkout on the build Mac}
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
scp -q $MAC:$M/payload/droidvm-disk.img $O/droidvm-disk.img
scp -q $MAC:$M/kernel/kernel.macho $O/kernel.macho
llvm-nm -n --defined-only $O/kernel.macho > $O/kernel.nm
python3 $REPO/scripts/patch-early-probe.py $O/kernel.macho $O/kernel.probe.macho "$@"
export MTOOLS_SKIP_CHECK=1
mcopy -o -i $O/droidvm-disk.img $O/kernel.probe.macho ::/EFI/UniXNU/Payloads/kernel.macho
mcopy -o -i $O/droidvm-disk.img ::/EFI/UniXNU/Payloads/kernel.macho.adt $O/kernel.macho.adt
mcopy -o -i $O/droidvm-disk.img ::/EFI/UniXNU/unixnu.toml $O/unixnu.toml
adb -s $PHONE push $O/droidvm-disk.img /storage/emulated/0/DroidVM/UniXNU/droidvm-disk.img | tail -1
