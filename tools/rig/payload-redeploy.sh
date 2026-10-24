#!/usr/bin/env bash
# Rebuild the boot payload on the build Mac and boot it: remake the owner trust bundle (a rebuilt plugin or owner tool changes its
# CDHashes) and pin it in board.toml (here and on the Mac), build the payload around the Data seed, make the boot disk, then the
# usual deploy.sh, a vm.sh restart, the guest ssh key, and optionally owner tools staged into the guest.
#   payload-redeploy.sh [--keep-trust] [TOOL...]     TOOL: build/TOOL/TOOL on the Mac -> /private/var/ngpu/bin/TOOL
# DATA_SEED names the seed in the tree's build/droidvm-data (default data-12g-skip-ov.dbr). A different seed than the running one
# starts the guest's Data again: back up /private/var/ngpu first (PROGRESS.md in the meta repo, "Data reset").
# The Mac's checkouts must hold the source here (mac-sync-check.sh; SYNC_CHECK=0 skips it). The new boot disk is compared with the
# running one before the VM stops: a changed DarwinBoot, UEFI loader, device tree (ADT; its random seed aside) or Data seed is
# refused unless ALLOW_CHANGE names it (ALLOW_CHANGE="darwinboot uefi adt seed"); a changed seed starts Data again. The ADT is made
# from board.toml's [payload.adt] and package/crosvm-gunyah.dtb, the VM's own FDT: both must follow the VM's devices. The running disk is kept as out/droidvm-disk.prev.img.
# Needs BUILD_HOST, BUILD_OUT, BUILD_REPO (the device repository's checkout on the Mac), FORK and unique-chip-id.txt beside this script.
set -u
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/rigenv.sh"
MAC=${BUILD_HOST:?set BUILD_HOST to the build Mac, user@host}
M=${BUILD_OUT:?set BUILD_OUT to out/droidvm of the UniXNU checkout on the build Mac}
MREPO=${BUILD_REPO:?set BUILD_REPO to the device repository checkout on the build Mac}
DEV=${REPO:-$RIG/../..}
SEED=${DATA_SEED:-data-12g-skip-ov.dbr}
CHIP=${CHIP_ID:-$(cat "$RIG/unique-chip-id.txt")}
KEEP=0; [ "${1:-}" = --keep-trust ] && { KEEP=1; shift; }
if [ "${SYNC_CHECK:-1}" != 0 ]; then
  "$RIG/mac-sync-check.sh" || { echo "the Mac does not build the source here: move its checkouts first (SYNC_CHECK=0 to build anyway)"; exit 1; }
fi
if [ $KEEP = 0 ]; then
  OLD=$(python3 -c "import tomllib;print(tomllib.load(open('$DEV/board.toml','rb'))['payload']['trust_cache_sha256'][1])")
  [ "$(grep -c "\"$OLD\"" "$DEV/board.toml")" = 1 ] || { echo "trust pin $OLD is not unique in board.toml"; exit 1; }
  NEW=$(ssh "$MAC" "export PATH=/opt/homebrew/bin:\$PATH; cd $M/../.. && python3 $M/tree/scripts/make-owner-trust-bundle.py 2>&1 | tail -1 | grep -oE '[0-9a-f]{64}'")
  [ ${#NEW} = 64 ] || { echo "trust bundle failed"; exit 1; }
  echo "trust $OLD -> $NEW"
  sed -i "s/$OLD/$NEW/" "$DEV/board.toml"
  ssh "$MAC" "sed -i '' 's/$OLD/$NEW/' $MREPO/board.toml" || exit 1
fi
ssh "$MAC" "export PATH=/opt/homebrew/bin:\$PATH; T=$M/tree; mkdir -p $M/rig-logs; cd $M/../.. &&
  ./unixnu build payload --trust-cache \$T/build/adreno-metal-plugin/owner-trust.bundle --data-ramdisk \$T/build/droidvm-data/$SEED \
    --unique-chip-id $CHIP > $M/rig-logs/payload.log 2>&1 || { tail -5 $M/rig-logs/payload.log; exit 1; }
  bash $MREPO/scripts/make-droidvm-disk.sh $M/payload/payload.img $M/package/Build/Out/AARCH64-DEBUG $M/payload/droidvm-disk.img \
    > $M/rig-logs/disk.log 2>&1 || { tail -5 $M/rig-logs/disk.log; exit 1; }" || { echo "payload/disk failed"; exit 1; }
echo "payload built around $SEED"
cd "$RIG"
scp -q "$MAC:$M/payload/droidvm-disk.img" out/droidvm-disk.next.img || { echo "cannot fetch the new boot disk"; exit 1; }
python3 - out/droidvm-disk.img out/droidvm-disk.next.img "${ALLOW_CHANGE:-}" "$DEV/scripts/dump-adt.py" <<'PY' || exit 1
import os, subprocess, sys, tempfile, tomllib
running, new, allowed, dump_adt = sys.argv[1], sys.argv[2], sys.argv[3].split(), sys.argv[4]
parts = {'darwinboot': '::/EFI/UniXNU/Payloads/DarwinBoot.efi', 'uefi': '::/EFI/BOOT/BOOTAA64.EFI',
         'seed': '::/EFI/UniXNU/Payloads/scratch-data.img', 'adt': '::/EFI/UniXNU/Payloads/kernel.macho.adt',
         'trust': '::/EFI/UniXNU/Payloads/kernel.macho.trustcache', 'config': '::/EFI/UniXNU/unixnu.toml'}
guarded = {'darwinboot', 'uefi', 'adt', 'seed'}
def read(disk, path):
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, 'f')
        r = subprocess.run(['mcopy', '-n', '-i', disk, path, out], env=dict(os.environ, MTOOLS_SKIP_CHECK='1'), capture_output=True)
        return open(out, 'rb').read() if r.returncode == 0 else None
def adt(disk):
    """The ADT as dump-adt.py prints it, without the random seed every build draws anew."""
    with tempfile.NamedTemporaryFile() as f:
        f.write(read(disk, parts['adt']) or b''); f.flush()
        text = subprocess.run([sys.executable, '-I', dump_adt, f.name], capture_output=True, text=True).stdout
    return [line for line in text.splitlines() if 'random-seed' not in line]
def boot_args(disk):
    config = tomllib.loads((read(disk, parts['config']) or b'').decode())
    return {entry.get('id'): entry.get('args', '') for entry in config.get('entry', [])}
if not os.path.exists(running):
    sys.exit('no running boot disk in out/ to compare with (deploy.sh fetches one)')
changed = [name for name, path in parts.items()
           if (adt(running) != adt(new) if name == 'adt' else read(running, path) != read(new, path))]
print('boot disk changes:', ' '.join(changed) or 'none (the kernel is replaced by deploy.sh)')
before, after = boot_args(running), boot_args(new)
for entry, args in after.items():
    old, now = before.get(entry, '').split(), args.split()
    if old != now:
        print(f'boot-args {entry}: -{[a for a in old if a not in now]} +{[a for a in now if a not in old]}')
    if len(args.encode()) >= 1024:
        sys.exit(f'boot args for {entry}: {len(args.encode())} bytes; DarwinBoot accepts at most 1023')
refused = [name for name in changed if name in guarded and name not in allowed]
if refused:
    sys.exit(f'refusing a new {" and ".join(refused)}: set ALLOW_CHANGE="{" ".join(refused)}" if that is meant'
             + (' (a new seed starts Data again)' if 'seed' in refused else ''))
PY
timeout 300 ./vm.sh stop 2>&1 | tail -1
cp -p out/droidvm-disk.img out/droidvm-disk.prev.img && mv out/droidvm-disk.next.img out/droidvm-disk.img
timeout 600 ./deploy.sh --kernel-only 2>&1 | tail -1
adb -s "$PHONE" shell "su -c sync"
L=$(md5sum out/droidvm-disk.img | cut -c1-32)
R=$(adb -s "$PHONE" shell "su -c 'md5sum /storage/emulated/0/DroidVM/UniXNU/droidvm-disk.img'" | cut -c1-32)
[ "$L" = "$R" ] && echo "boot disk verified" || { echo "boot disk MISMATCH"; exit 1; }
timeout 900 ./vm.sh start 2>&1 | grep -E "^kernel|^userland|^network|^crashes"
python3 -c "import time; time.sleep(30)"
timeout 60 ./guest-ssh.sh --key >/dev/null 2>&1 && echo "guest ssh key put"
for t in "$@"; do
  mkdir -p out/tools
  scp -q "$MAC:$M/tree/build/$t/$t" "out/tools/$t" &&
    timeout 60 python3 export.py put "out/tools/$t" "/private/var/ngpu/bin/$t" 0755 >/dev/null 2>&1 && echo "$t staged"
done
