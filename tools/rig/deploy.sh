#!/usr/bin/env bash
# Fetch the kernel built on the Mac, add the early probes to a copy, regenerate the ADT, refresh the boot
# disk and push it to the phone.   deploy.sh [patch-early-probe.py args, e.g. --ubsan nop]
set -euo pipefail
RIG=$(cd "$(dirname "$0")" && pwd)
O=$RIG/out
MAC=${BUILD_HOST:?set BUILD_HOST to the build Mac, user@host}
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
scp -q $MAC:${BUILD_OUT:?set BUILD_OUT to out/droidvm of the UniXNU checkout on the build Mac}/kernel/kernel.macho $O/kernel.macho
llvm-nm -n --defined-only $O/kernel.macho > $O/kernel.nm
python3 $RIG/../../scripts/patch-early-probe.py $O/kernel.macho $O/kernel.probe.macho "$@"
ADT=$(mktemp --suffix=.adt)
python3 $RIG/../../scripts/make-droidvm-adt.py --fdt $RIG/../../package/crosvm-gunyah.dtb \
  --dram-size 0xff600000 --dram-base 0x80600000 --cpu-regs 0 --owned-memory-size 0x60000000 --output "$ADT"
export MTOOLS_SKIP_CHECK=1
mcopy -o -i $O/droidvm-disk.img $O/kernel.probe.macho ::/EFI/UniXNU/Payloads/kernel.macho
mcopy -o -i $O/droidvm-disk.img "$ADT" ::/EFI/UniXNU/Payloads/kernel.macho.adt
rm -f "$ADT"
adb -s $PHONE push $O/droidvm-disk.img /storage/emulated/0/DroidVM/UniXNU/droidvm-disk.img | tail -1
