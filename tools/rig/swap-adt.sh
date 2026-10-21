#!/usr/bin/env bash
# Regenerate the ADT of the DroidVM boot disk on Linux and push the disk to the phone (no Mac rebuild).
#   swap-adt.sh [extra make-droidvm-adt.py args...]
set -euo pipefail
. "$(dirname "$0")/rigenv.sh"
RIG=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$RIG/../..}   # the device repository (rig.env names it when the rig lives elsewhere)
DISK=$RIG/out/droidvm-disk.img
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
ADT=$(mktemp --suffix=.adt)
python3 $REPO/scripts/make-droidvm-adt.py --fdt $REPO/package/crosvm-gunyah.dtb \
  --dram-size 0xff600000 --dram-base 0x80600000 --cpu-regs 0 --output "$ADT" "$@"
MTOOLS_SKIP_CHECK=1 mcopy -o -i "$DISK" "$ADT" ::/EFI/UniXNU/Payloads/kernel.macho.adt
rm -f "$ADT"
adb -s "$PHONE" push "$DISK" /storage/emulated/0/DroidVM/UniXNU/droidvm-disk.img | tail -1
