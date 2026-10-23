#!/usr/bin/env bash
# Rebuild the boot payload on the build Mac and boot it: remake the owner trust bundle (a rebuilt plugin or owner tool changes its
# CDHashes) and pin it in board.toml (here and on the Mac), build the payload around the Data seed, make the boot disk, then the
# usual deploy.sh, a vm.sh restart, the guest ssh key, and optionally owner tools staged into the guest.
#   payload-redeploy.sh [--keep-trust] [TOOL...]     TOOL: build/TOOL/TOOL on the Mac -> /private/var/ngpu/bin/TOOL
# DATA_SEED names the seed in the tree's build/droidvm-data (default data-12g-skip-ov.dbr). A different seed than the running one
# starts the guest's Data again: back up /private/var/ngpu first (PROGRESS.md in the meta repo, "Data reset").
# Needs BUILD_HOST, BUILD_OUT, BUILD_REPO (the device repository's checkout on the Mac) and unique-chip-id.txt beside this script.
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
timeout 300 ./vm.sh stop 2>&1 | tail -1
timeout 600 ./deploy.sh 2>&1 | tail -1
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
