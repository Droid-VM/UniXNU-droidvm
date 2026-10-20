#!/usr/bin/env bash
# Turn the payload volume into the boot disk of a DroidVM guest.
#
#   scripts/make-droidvm-disk.sh PAYLOAD.img EFI_DIR OUTPUT.img
#
# edk2-gunyah starts \EFI\BOOT\BOOTAA64.EFI of a removable FAT volume, so UniXNU.efi goes there; it
# reads \EFI\UniXNU\unixnu.toml from the same volume, whose [[driver]] (board.toml loader = "driver")
# loads Payloads\DarwinBoot.efi. EFI_DIR is package/Build/Out/AARCH64-<TARGET> of the board output,
# holding UniXNU.efi (unixnu build package) and DarwinBoot.efi (scripts/build-darwinboot-efi.sh).
set -euo pipefail
PAYLOAD=$1
EFI_DIR=$2
OUTPUT=$3
for file in "$EFI_DIR/UniXNU.efi" "$EFI_DIR/DarwinBoot.efi"; do
  [ -f "$file" ] || { echo "error: missing $file" >&2; exit 1; }
done
grep -aq 'Payloads/DarwinBoot.efi' "$PAYLOAD" || {
  echo "error: $PAYLOAD does not load Payloads/DarwinBoot.efi; set loader = \"driver\" in board.toml [payload]" >&2
  exit 1
}
export MTOOLS_SKIP_CHECK=1 TZ=UTC
cp "$PAYLOAD" "$OUTPUT.tmp"
mmd -i "$OUTPUT.tmp" ::/EFI/BOOT
mcopy -i "$OUTPUT.tmp" -m "$EFI_DIR/UniXNU.efi" ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i "$OUTPUT.tmp" -m "$EFI_DIR/DarwinBoot.efi" ::/EFI/UniXNU/Payloads/DarwinBoot.efi
mv "$OUTPUT.tmp" "$OUTPUT"
mdir -i "$OUTPUT" -/ ::/EFI
