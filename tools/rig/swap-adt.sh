#!/usr/bin/env bash
# Regenerate the ADT of the DroidVM boot disk on Linux and push the disk to the phone (no Mac rebuild): the same command
# "unixnu build payload" runs, from board.toml ([payload] dram_size, [payload.adt]) and package/crosvm-gunyah.dtb (the VM's own
# FDT), for a payload with a Data seed and a trust cache. Prints how the new ADT differs from the disk's (random seed aside).
#   swap-adt.sh [--dry-run] [extra make-droidvm-adt.py args...]      --dry-run: write out/kernel.macho.adt.new only
set -euo pipefail
. "$(dirname "$0")/rigenv.sh"
RIG=$(cd "$(dirname "$0")" && pwd)
REPO=${REPO:-$RIG/../..}   # the device repository (rig.env names it when the rig lives elsewhere)
DISK=$RIG/out/droidvm-disk.img
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
DRY=0; [ "${1:-}" = --dry-run ] && { DRY=1; shift; }
NEW=$RIG/out/kernel.macho.adt.new
python3 - "$REPO" "$(cat "$RIG/unique-chip-id.txt")" "$NEW" "$@" <<'PY'
import subprocess, sys, tomllib
repo, chip, out, extra = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
spec = tomllib.load(open(f'{repo}/board.toml', 'rb'))['payload']
adt = spec.get('adt', {})
command = ['python3', f'{repo}/{adt["tool"]}', '--fdt', f'{repo}/package/crosvm-gunyah.dtb', '--output', out,
           '--dram-size', str(spec['dram_size'])]
for key in ('owned_memory_size', 'root_match_volume_uuid', 'chip_id', 'cpu_regs', 'psci_method', 'dram_base'):
    if adt.get(key) is not None:
        command += ['--' + key.replace('_', '-'), str(adt[key])]
command += ['--unique-chip-id', chip, '--packed-data-ramdisk' if adt.get('packed_data_ramdisk') else '--data-ramdisk',
            '--trust-cache']
command += ['--ipad-identity'] * bool(adt.get('ipad_identity')) + ['--persistent-data'] * bool(adt.get('persistent_data'))
for fixup in spec.get('text_fixups', []):
    command += ['--text-fixup', fixup]
subprocess.run(command + adt.get('tool_args', []) + extra, check=True)
PY
OLD=$(mktemp --suffix=.adt)
MTOOLS_SKIP_CHECK=1 mcopy -o -n -i "$DISK" ::/EFI/UniXNU/Payloads/kernel.macho.adt "$OLD"
diff <(python3 "$REPO/scripts/dump-adt.py" "$OLD" | grep -v random-seed) \
     <(python3 "$REPO/scripts/dump-adt.py" "$NEW" | grep -v random-seed) && echo "ADT unchanged (random seed aside)" || true
rm -f "$OLD"
[ $DRY = 1 ] && exit 0
MTOOLS_SKIP_CHECK=1 mcopy -o -i "$DISK" "$NEW" ::/EFI/UniXNU/Payloads/kernel.macho.adt
cp "$NEW" "$RIG/out/kernel.macho.adt"
adb -s "$PHONE" push "$DISK" /storage/emulated/0/DroidVM/UniXNU/droidvm-disk.img | tail -1
