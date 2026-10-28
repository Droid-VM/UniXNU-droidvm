#!/usr/bin/env bash
# Replace the phone's UniXNU VM with a new one from a release that scripts/build-droidvm-all.py made on the build Mac: the old
# VM is stopped and deleted with everything in its directory (images, Data store and their copies), and a new VM with a new id
# gets the release's boot disk, OS and SystemOS disks, the firmware, an empty Data store and the VM config of
# unixnu-vm.template.json, then boots.
#   fresh-deploy.sh [--build] [--yes] [--keep-files]
#     --build        run build-droidvm-all.py on the Mac first (BUILD_REPO; IPSW_CACHE passes --ipsw-cache)
#     --yes          do not ask before deleting the old VM
#     --keep-files   keep the VM directory as an earlier run left it: only the VM is made again (and booted)
# Needs PHONE, DVMIPC, BUILD_HOST, BUILD_OUT (out/droidvm of the release's checkout), BUILD_REPO (the device repository there),
# EDK2_FD (the edk2-gunyah firmware with the virtio-gpu GOP: the meta repo's edk2-gunyah/edk2-gunyah.fd) and unique-chip-id.txt
# beside this script; CROSVM_BIN, when set, replaces the app's crosvm (the VM's sound, battery and media devices need the
# Droid-VM crosvm of wip/hackintosh). Writes the new VM id into rig.env and the config into unixnu-vm.config.json. Follow
# hugepage-harness.md (meta repo): run hostwatch.py while it boots.
set -euo pipefail
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/rigenv.sh"
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
IPC=${DVMIPC:?set DVMIPC to the path of the DroidVM daemon IPC client dvmipc.py}
MAC=${BUILD_HOST:?set BUILD_HOST to the build Mac, user@host}
M=${BUILD_OUT:?set BUILD_OUT to out/droidvm of the UniXNU checkout on the build Mac}
FD=${EDK2_FD:?set EDK2_FD to the edk2-gunyah firmware (the meta repo edk2-gunyah/edk2-gunyah.fd)}
A="adb -s $PHONE"
NAME="UniXNU (DroidVM)"
DIR=/storage/emulated/0/DroidVM/UniXNU
MEDIA=/data/media/0/DroidVM/UniXNU
APP=cn.classfun.droidvm
FILES=/data/user/0/$APP/files
RUN=/data/data/$APP/run
O=$RIG/out
BUILD=0; YES=0; KEEP=0
for arg in "$@"; do
  case "$arg" in --build) BUILD=1 ;; --yes) YES=1 ;; --keep-files) KEEP=1 ;; *) sed -n '2,16p' "$0"; exit 2 ;; esac
done
mkdir -p "$O/release"
[ -f "$FD" ] || { echo "no firmware $FD"; exit 1; }

if [ $BUILD = 1 ]; then
  MREPO=${BUILD_REPO:?set BUILD_REPO to the device repository checkout on the build Mac}
  ssh "$MAC" "export PATH=/opt/homebrew/bin:\$PATH; cd $MREPO && python3 -u scripts/build-droidvm-all.py \
    --unique-chip-id $(cat "$RIG/unique-chip-id.txt") ${IPSW_CACHE:+--ipsw-cache $IPSW_CACHE}" || { echo "build failed"; exit 1; }
fi

echo "== release"
scp -q "$MAC:$M/release/release.json" "$O/release/release.json" || { echo "no release in $MAC:$M/release (build-droidvm-all.py)"; exit 1; }
for f in os.img systemos.img; do
  want=$(python3 -c "import json,sys;print(json.load(open(sys.argv[1]))['files'][sys.argv[2]]['sha256'])" "$O/release/release.json" "$f")
  if [ -f "$O/release/$f" ] && [ "$(cat "$O/release/$f.sha256" 2>/dev/null)" = "$want" ]; then
    echo "$f: have it"
  else
    scp -q "$MAC:$M/release/$f" "$O/release/$f"
    got=$(sha256sum "$O/release/$f" | cut -c1-64)
    [ "$got" = "$want" ] || { echo "$f: sha256 $got, the release says $want"; exit 1; }
    echo "$got" > "$O/release/$f.sha256"
  fi
done

ipc() {
  local port token
  port=$($A shell "su -c 'cat $RUN/droidvmd-port.txt'" | tr -dc 0-9)
  token=$($A shell "su -c 'cat $RUN/droidvmd-token.txt'" | tr -dc 0-9a-f)
  $A forward tcp:$port tcp:$port >/dev/null
  python3 "$IPC" "$port" "$token" "$@"
}

echo "== the old VM"
$A shell "su -c 'cat $FILES/vms.json'" > "$O/vms.json.before"
"$RIG/daemon-recover.sh" >/dev/null 2>&1 || true
# The app's vms.json and the daemon's store can disagree (a VM made over IPC, an app that wrote its own copy back): both count.
ipc vm_list > "$O/vm_list.before" 2>/dev/null || true
OLD=$(python3 -c "
import json, sys
ids = [vm['id'] for vm in json.load(open(sys.argv[1]))['vms'] if vm.get('name') == sys.argv[3]]
text = open(sys.argv[2]).read()
if '\n' in text:
    ids += [vm['id'] for vm in json.loads(text[text.index('\n') + 1:]).get('data', []) if vm.get('name') == sys.argv[3]]
print(' '.join(dict.fromkeys(ids)))" "$O/vms.json.before" "$O/vm_list.before" "$NAME")
echo "VMs named \"$NAME\": ${OLD:-none}"
$A shell "su -c 'ls -la $DIR 2>/dev/null'" || true
if [ $YES = 0 ]; then
  [ $KEEP = 1 ] && what="delete them" || what="delete them and everything in $DIR"
  read -r -p "$what? [y/N] " answer
  [ "$answer" = y ] || exit 1
fi
if [ -n "$(adb -s "$PHONE" shell "su -c 'for p in \$(pidof crosvm); do tr \"\\0\" \" \" < /proc/\$p/cmdline | grep -q UniXNU && echo \$p; done'" | tr -dc 0-9)" ]; then
  VM=${VM:-${OLD%% *}} "$RIG/vm.sh" stop       # through its control socket, then the hugepage pool's settle time
fi
for id in $OLD; do ipc vm_delete "{\"vm_id\":\"$id\"}" | grep -E '"(success|existed)"' | tr -d ' \n'; echo " deleted $id from the daemon"; done
# The app keeps its own copy of vms.json in memory and writes it back: stop it before the file changes.
$A shell "am force-stop $APP"
$A shell "su -c 'cp $FILES/vms.json $FILES/vms.json.bak-fresh-$(date +%Y%m%d-%H%M%S)'"

if [ $KEEP = 0 ]; then
echo "== the new VM's files"
$A shell "su -c 'rm -rf $MEDIA'"
$A shell "mkdir -p $DIR"               # through the shared storage, which gives it the owner the app's other VM directories have
for f in os.img systemos.img; do $A push "$O/release/$f" "$DIR/$f" | tail -1; done
$A push "$FD" "$DIR/edk2-virtio-gpu.fd" | tail -1
"$RIG/deploy.sh"                       # the release's boot disk (payload/droidvm-disk.img), with the early probes, and kernel.nm
"$RIG/make-data-store.sh" >/dev/null
$A shell "su -c 'sync; ls -la $DIR'"
if [ -n "${CROSVM_BIN:-}" ]; then
  if [ "$(md5sum "$CROSVM_BIN" | cut -c1-32)" != "$($A shell "su -c 'md5sum /data/data/$APP/usr/bin/crosvm'" | cut -c1-32)" ]; then
    "$RIG/crosvm-deploy.sh"
  else
    echo "crosvm: the app has CROSVM_BIN already"
  fi
fi
fi

echo "== the new VM"
NET=$(adb -s "$PHONE" shell "su -c 'cat $FILES/networks.json'" | python3 -c "
import json, sys
nets = [n for n in json.load(sys.stdin)['networks'] if n.get('bridge_name') == 'br-wifi' or n.get('name') == 'br-wifi']
sys.exit('no br-wifi network in the app') if not nets else print(nets[0]['id'])")
UID_=$($A shell "su -c 'stat -c %u /data/data/$APP'" | tr -dc 0-9)     # the app's uid: the sound and media helpers run as it
ID=$(python3 -c 'import uuid; print(uuid.uuid4())')
python3 - "$RIG/unixnu-vm.template.json" "$RIG/unixnu-vm.config.json" "$ID" "$NET" "$UID_" "$DIR" "$MEDIA" <<'PY'
import json, sys
template, output, vm_id, net, uid, directory, media = sys.argv[1:]
text = open(template).read()
for key, value in (('@VM_ID@', vm_id), ('@NETWORK_ID@', net), ('@APP_UID@', uid), ('@MEDIA_DIR@', media), ('@DIR@', directory)):
    text = text.replace(key, value)
if '@' in text:
    sys.exit('the template has a placeholder left: ' + text[text.index('@'):][:40])
open(output, 'w').write(json.dumps(json.loads(text), indent=1) + '\n')
PY
python3 - "$O/vms.json.before" "$RIG/unixnu-vm.config.json" "$O/vms.json.new" "$NAME" <<'PY'
import json, sys, time
before, config, output, name = sys.argv[1:]
store = json.load(open(before))
vm = json.load(open(config))['config']
vm['created_at'] = int(time.time() * 1000)
store['vms'] = [v for v in store['vms'] if v.get('name') != name] + [vm]
open(output, 'w').write(json.dumps(store, indent=4) + '\n')
PY
$A push "$O/vms.json.new" /data/local/tmp/vms.json.new >/dev/null
# cat over the file keeps its owner, mode and SELinux label
$A shell "su -c 'cat /data/local/tmp/vms.json.new > $FILES/vms.json && rm /data/local/tmp/vms.json.new'"
if grep -q '^VM=' "$RIG/rig.env" 2>/dev/null; then
  sed -i "s/^VM=.*/VM=$ID/" "$RIG/rig.env"
else
  echo "VM=$ID" >> "$RIG/rig.env"
fi
export VM=$ID
echo "VM $ID: network $NET, app uid $UID_"

echo "== boot"
"$RIG/vm.sh" start 300
python3 -c "import time; time.sleep(30)"
timeout 60 "$RIG/guest-ssh.sh" --key >/dev/null 2>&1 && echo "guest ssh key put" || echo "guest ssh key NOT put (export.py)"
