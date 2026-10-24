#!/usr/bin/env bash
# Fetch the boot disk and kernel built on the Mac ("unixnu build payload" + make-droidvm-disk.sh, see
# README.md), put a copy of the kernel with the early exception catcher on
# the disk and push it to the phone.   deploy.sh [--kernel-only] [patch-early-probe.py args, e.g. --ubsan nop]
# --kernel-only preserves the rig's current boot disk (DarwinBoot, ADT, trust and Data seed).
set -euo pipefail
. "$(dirname "$0")/rigenv.sh"
RIG=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$RIG/../..}   # the device repository (rig.env names it when the rig lives elsewhere)
O=$RIG/out
MAC=${BUILD_HOST:?set BUILD_HOST to the build Mac, user@host}
M=${BUILD_OUT:?set BUILD_OUT to out/droidvm of the UniXNU checkout on the build Mac}
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
if [ "${1:-}" = --kernel-only ]; then
  shift
  [ -f "$O/droidvm-disk.img" ] || { echo "no current boot disk for --kernel-only" >&2; exit 1; }
else
  scp -q "$MAC:$M/payload/droidvm-disk.img" "$O/droidvm-disk.img"
fi
scp -q $MAC:$M/kernel/kernel.macho $O/kernel.macho
llvm-nm -n --defined-only $O/kernel.macho > $O/kernel.nm
python3 $REPO/scripts/patch-early-probe.py $O/kernel.macho $O/kernel.probe.macho "$@"
export MTOOLS_SKIP_CHECK=1
mcopy -o -i $O/droidvm-disk.img $O/kernel.probe.macho ::/EFI/UniXNU/Payloads/kernel.macho
mcopy -o -i $O/droidvm-disk.img ::/EFI/UniXNU/Payloads/kernel.macho.adt $O/kernel.macho.adt
mcopy -o -i $O/droidvm-disk.img ::/EFI/UniXNU/unixnu.toml $O/unixnu.toml
# BOOT_ARGS_DROP="arg=1 other=2": leave those boot-args out of this disk only, for an experiment without a rebuild.
if [ -n "${BOOT_ARGS_DROP:-}" ]; then
  for a in $BOOT_ARGS_DROP; do sed -i "s/ $a\([ \"]\)/\1/" $O/unixnu.toml; done
  mcopy -o -i $O/droidvm-disk.img $O/unixnu.toml ::/EFI/UniXNU/unixnu.toml
  echo "dropped boot-args: $BOOT_ARGS_DROP"
fi
python3 - "$O/unixnu.toml" <<'PY'
import sys, tomllib
config = tomllib.load(open(sys.argv[1], 'rb'))
for entry in config.get('entry', []):
    length = len(entry.get('args', '').encode())
    if length >= 1024:
        raise SystemExit(f'boot args for {entry.get("id")}: {length} bytes; DarwinBoot accepts at most 1023')
PY
adb -s $PHONE push $O/droidvm-disk.img /storage/emulated/0/DroidVM/UniXNU/droidvm-disk.img | tail -1
