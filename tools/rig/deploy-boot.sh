#!/usr/bin/env bash
# The Mac's new iPadOS 27 boot disk onto this rig's VM: stop it, fetch the disk and the native kernel, put the working
# DarwinBoot back on the disk (the Mac's make-droidvm-disk.sh puts an old one there and mac-build27.sh's replacement may be
# gone; out/DarwinBoot.working.efi is the one this VM boots with), refresh the fileset symbols, push, boot and print health.
#   deploy-boot.sh [WAIT]
set -euo pipefail
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/rigenv.sh"
O=$RIG/out
export MTOOLS_SKIP_CHECK=1
[ -f "$O/DarwinBoot.working.efi" ] || { echo "no $O/DarwinBoot.working.efi"; exit 1; }
timeout 120 "$RIG/vm.sh" stop 2>&1 | tail -1
scp -q "$BUILD_HOST:$BUILD_OUT/payload/droidvm-disk.img" "$O/droidvm-disk.img"
scp -q "$BUILD_HOST:$BUILD_OUT/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf" \
  "$O/kernel.native.macho"
llvm-nm -n --defined-only "$O/kernel.native.macho" > "$O/kernel.native.nm"
mcopy -o -i "$O/droidvm-disk.img" "$O/DarwinBoot.working.efi" ::/EFI/UniXNU/Payloads/DarwinBoot.efi
mcopy -o -i "$O/droidvm-disk.img" ::/EFI/UniXNU/Payloads/kernel.macho.adt "$O/kernel.macho.adt"
mcopy -o -i "$O/droidvm-disk.img" ::/EFI/UniXNU/Payloads/kernel.macho "$O/kernel.macho"
python3 "$RIG/fileset-syms.py" "$O/kernel.macho" "$O/kernel.native.macho" "$O/kernel.native.nm" "$O/kernel.nm" >/dev/null
adb -s "$PHONE" push "$O/droidvm-disk.img" /storage/emulated/0/DroidVM/UniXNU27/droidvm-disk.img | tail -1
timeout 900 "$RIG/vm.sh" start "${1:-300}" 2>&1 | grep -v -E 'Traceback|File |~~~|\^\^\^|raise|main\(\)|auth:|m = recv|EOFError'
