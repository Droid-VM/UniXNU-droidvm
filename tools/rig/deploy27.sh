#!/usr/bin/env bash
# The build Mac's iPadOS 27 release (ipados27/scripts/build-droidvm-all.py) onto this rig's VM, which keeps its Data.
#   deploy27.sh [--build] [--allow-dirty] [--images] [--keep-plugin] [--new-seed] [--force-stop] [WAIT]
#   deploy27.sh --rollback [WAIT]   the boot disk the last deploy replaced (out/previous27), boot it, and when that deploy
#                                   changed the plugin in the guest's Data, put the replaced files back and boot again
#   deploy27.sh --new-vm FROM_RIG NAME [--images] [--allow-dirty] [WAIT]
#                                   a new VM beside the ones the phone has, from a release with a new Data seed (the build
#                                   script without --generation): see "A new VM" below
#     --build        build the release first, for this rig: its chip id (unique-chip-id.txt) and the Data seed its VM was made
#                    from (BUILD_GENERATION, a directory on the Mac; see the build script)
#     --allow-dirty  take a release built from checkouts with local changes (an experiment); --build passes it on
#     --images       also the OS and SystemOS disks (a VM directory that has none yet)
#     --keep-plugin  leave the Metal plugin in the guest's Data as it is (the release must still trust it)
#     --new-seed     take a release whose Data seed is not the one last deployed here (its store must be new: make-data-store.sh)
#     --force-stop   stop the VM although the hugepage pool would be short afterwards
# What it does, in order:
#   1. release.json from the Mac; refused when the release is not clean, is for another chip id or another Data seed
#   2. the release's files, each checked against the sha256 release.json gives
#   3. the release's plugin into the running guest's Data where a file differs (each as .new, renamed over: running processes
#      keep the old file mapped; the kernel that boots next trusts the new one), over guest-ssh.sh; the guest's own copy of
#      each replaced file is kept first
#   4. vm.sh stop; the phone's boot disk and the rig's symbols for it kept in out/previous27; the new boot disk and
#      release.json into the VM's directory on the phone; kernel symbols for the rig's tools
#   5. vm.sh start, panic.py, health.py, and the plugin's hash as the booted guest holds it. Exits 1 when the guest did not
#      come up; --rollback then puts the previous disk and plugin back
# A new VM (--new-vm), run in a new rig directory (a copy of tools/rig with its own rig.env, VM_DIR a directory the phone
# does not have yet and with UniXNU in its name, unique-chip-id.txt the id the release was built with): the VM's directory
# gets the release's boot disk, release.json and an empty Data store; the VM is made over the daemon's IPC from the
# configuration of an existing iPadOS 27 VM (FROM_RIG/unixnu-vm.config.json) with a new id, NAME, the new directory, its own
# tap and MAC -- nothing of the other VMs changes, and the app's vms.json is not touched (the VM lives in the daemon;
# daemon-recover.sh makes it again from this rig's unixnu-vm.config.json). The OS and SystemOS disks are read-only: with
# --images the release's go into VM_DIR, without it the VM reads FROM_RIG's VM's, checked on the phone against release.json.
# One UniXNU VM runs at a time: stop the other first. The rig's VM and GUEST_IP are written when it is up.
# Needs PHONE, VM, DVMIPC, BUILD_HOST, BUILD_OUT (the board's output on the Mac: <UniXNU>/out/droidvm/<os profile>/
# droidvm_ipados27), GUEST_IP for step 3; with --build also BUILD_REPO (the device repository checkout there) and
# unique-chip-id.txt beside this script. VM_DIR is the VM's directory on the phone (default /storage/emulated/0/DroidVM/UniXNU27).
# Follow hugepage-harness.md (meta repo): run hostwatch.py while it boots.
set -euo pipefail
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/rigenv.sh"
PHONE=${PHONE:?set PHONE to the adb serial of the phone}
MAC=${BUILD_HOST:?set BUILD_HOST to the build Mac, user@host}
M=${BUILD_OUT:?set BUILD_OUT to the board output on the build Mac}
DIR=${VM_DIR:-/storage/emulated/0/DroidVM/UniXNU27}
O=$RIG/out
R=$O/release27
P=$O/previous27
BUILD=0; DIRTY=0; IMAGES=0; PLUGIN=1; NEWSEED=0; FORCE=0; ROLLBACK=0; WAIT=300; NEWVM=0; FROM=; NAME=
while [ $# -gt 0 ]; do
  case "$1" in
    --build) BUILD=1 ;; --allow-dirty) DIRTY=1 ;; --images) IMAGES=1 ;; --keep-plugin) PLUGIN=0 ;; --new-seed) NEWSEED=1 ;;
    --force-stop) FORCE=1 ;; --rollback) ROLLBACK=1 ;; [0-9]*) WAIT=$1 ;;
    --new-vm) NEWVM=1; FROM=${2:?--new-vm FROM_RIG NAME}; NAME=${3:?--new-vm FROM_RIG NAME}; shift 2 ;;
    *) sed -n '2,40p' "$0"; exit 2 ;;
  esac
  shift
done
if [ $NEWVM = 1 ]; then PLUGIN=0; NEWSEED=1; fi
mkdir -p "$R"
A="adb -s $PHONE"
export MTOOLS_SKIP_CHECK=1
SYMBOLS="kernel.macho kernel.native.macho kernel.native.nm kernel.nm kernel.macho.adt unixnu.toml"

stop_vm() {
  if ! "$RIG/vm.sh" pool; then
    [ $FORCE = 1 ] || { echo "the pool would be short after a stop (hugepage-harness.md): not stopping; --force-stop does"; exit 1; }
  fi
  timeout 180 "$RIG/vm.sh" stop 2>&1 | tail -2
}
boot_vm() {          # start, then say how the guest is; 1 when it did not come up
  timeout 900 "$RIG/vm.sh" start "$WAIT" 2>&1 | grep -v -E 'Traceback|File |~~~|\^\^\^|raise|main\(\)|auth:|m = recv|EOFError' || true
  timeout 120 python3 "$RIG/panic.py" 2>&1 | head -2 | cut -c1-250 || true
  timeout 200 python3 "$RIG/health.py" 2>&1
}

# Where the Data seed holds the plugin (add-gpu-plugin-data.py): the bundle under ngpu/addons, the probe daemon in ngpu/bin.
guest_path() { case "$1" in adrenoprobed) echo /private/var/ngpu/bin/adrenoprobed ;; *) echo "/private/var/ngpu/addons/$1" ;; esac; }
TOYBOX=/private/var/ngpu/bin/toybox
# push_plugin FROM [KEEP]: the plugin files of FROM into the running guest's Data where they differ; with KEEP, the guest's
# own file goes there first. 1 when a file did not arrive.
push_plugin() {
  local from=$1 keep=${2:-} ok=1 name dst want have mode
  # the names on descriptor 3: ssh in the loop would read the rest of the list from its standard input
  while read -r name <&3; do
    dst=$(guest_path "$name")
    want=$(sha256sum "$from/$name" | cut -c1-64)
    have=$(GSSH_TIMEOUT=60 "$RIG/guest-ssh.sh" "$TOYBOX sha256sum $dst" 2>/dev/null </dev/null | cut -c1-64 || true)
    if [ "$want" = "$have" ]; then echo "same   $name"; continue; fi
    if [ -n "$keep" ] && [ -n "$have" ]; then
      mkdir -p "$(dirname "$keep/$name")"
      GSSH_TIMEOUT=900 "$RIG/guest-ssh.sh" "$TOYBOX cat $dst" </dev/null > "$keep/$name" 2>/dev/null || true
      [ "$(sha256sum "$keep/$name" | cut -c1-64)" = "$have" ] || { echo "FAILED $name: could not keep the guest's copy"; ok=0; continue; }
      [ -x "$from/$name" ] && chmod 755 "$keep/$name"
    fi
    [ -x "$from/$name" ] && mode=755 || mode=644
    have=$(GSSH_TIMEOUT=900 "$RIG/guest-ssh.sh" "$TOYBOX cat > $dst.new && $TOYBOX chmod $mode $dst.new && $TOYBOX mv -f $dst.new $dst && /private/var/ngpu/bin/gfxprobe sync $dst >/dev/null && $TOYBOX sha256sum $dst" < "$from/$name" | cut -c1-64 || true)
    [ "$want" = "$have" ] && echo "pushed $name" || { echo "FAILED $name: the guest has ${have:-nothing}, want $want"; ok=0; }
  done 3< <(cd "$from" && find . -type f | sed 's|^\./||' | sort)
  [ $ok = 1 ]
}

if [ $ROLLBACK = 1 ]; then
  [ -f "$P/droidvm-disk.img" ] || { echo "no $P/droidvm-disk.img: no deploy to take back"; exit 1; }
  echo "== rollback to the boot disk of $(cat "$P/replaced-at" 2>/dev/null || echo an earlier deploy)"
  stop_vm
  $A push "$P/droidvm-disk.img" "$DIR/droidvm-disk.img" | tail -1
  if [ -f "$P/release.json" ]; then $A push "$P/release.json" "$DIR/release.json" | tail -1; cp "$P/release.json" "$R/deployed.json"
  else $A shell "rm -f $DIR/release.json"; rm -f "$R/deployed.json"; fi
  cp "$P/droidvm-disk.img" "$O/droidvm-disk.img"
  for name in $SYMBOLS; do [ -f "$P/$name" ] && cp "$P/$name" "$O/$name"; done
  echo "== boot"
  boot_vm || { echo "the guest did not come up on the previous boot disk either"; exit 1; }
  if [ -d "$P/plugin" ]; then
    # The previous disk does not trust the plugin the deploy put into Data: the guest is up without Metal until the plugin
    # it replaced is back, and a process maps the plugin once, so the guest boots again.
    echo "== the plugin the deploy replaced, back into the guest's Data"
    push_plugin "$P/plugin" || { echo "the previous plugin did not get back into the guest"; exit 1; }
    python3 -c "import time; time.sleep(15)"
    stop_vm
    boot_vm || exit 1
  fi
  exit 0
fi
CHIP=$(cat "$RIG/unique-chip-id.txt" 2>/dev/null || true)
# A value of a JSON file by its dotted path; a key may itself hold dots (files.kernel.native.macho.sha256).
field() { python3 -c "
import json, sys
value = json.load(open(sys.argv[1]))
keys = sys.argv[2].split('.')
while keys:
    for n in range(len(keys), 0, -1):
        key = '.'.join(keys[:n])
        if isinstance(value, dict) and key in value:
            value, keys = value[key], keys[n:]
            break
    else:
        value, keys = None, []
print('' if value is None else value)" "$1" "$2"; }

if [ $BUILD = 1 ]; then
  MREPO=${BUILD_REPO:?set BUILD_REPO to the device repository checkout on the build Mac}
  [ -n "$CHIP" ] || { echo "no $RIG/unique-chip-id.txt"; exit 1; }
  ssh -o BatchMode=yes "$MAC" "export PATH=/opt/homebrew/bin:\$PATH; cd $MREPO && python3 -u ipados27/scripts/build-droidvm-all.py \
    --unique-chip-id $CHIP ${BUILD_GENERATION:+--generation $BUILD_GENERATION} $([ $DIRTY = 1 ] && echo --allow-dirty)" \
    || { echo "build failed"; exit 1; }
fi

echo "== release"
scp -q "$MAC:$M/release/release.json" "$R/release.json" || { echo "no release in $MAC:$M/release (build-droidvm-all.py)"; exit 1; }
echo "built $(field "$R/release.json" built): UniXNU $(field "$R/release.json" commits.unixnu | cut -c1-12), device $(field "$R/release.json" commits.device | cut -c1-12), seed $(field "$R/release.json" seed.volume_uuid)"
if [ "$(field "$R/release.json" clean)" != True ] && [ $DIRTY = 0 ]; then
  echo "the release was built from checkouts with local changes (release.json \"changed\"); --allow-dirty takes it"; exit 1
fi
if [ -n "$CHIP" ] && [ "$(field "$R/release.json" unique_chip_id)" != "$CHIP" ]; then
  echo "the release is for another chip id than this rig's unique-chip-id.txt (build it with --build)"; exit 1
fi
if [ -f "$R/deployed.json" ] && [ $NEWSEED = 0 ] &&
   [ "$(field "$R/release.json" seed.volume_uuid)" != "$(field "$R/deployed.json" seed.volume_uuid)" ]; then
  echo "the release's Data seed ($(field "$R/release.json" seed.volume_uuid)) is not the one deployed here last ($(field "$R/deployed.json" seed.volume_uuid)):"
  echo "this VM's Data store could not be mounted. Build with the VM's seed (BUILD_GENERATION), or --new-seed with a new store."; exit 1
fi

fetch() {            # NAME in release/, checked against release.json; kept when the local copy already matches
  local name=$1 want got
  want=$(field "$R/release.json" "files.$name.sha256")
  if [ -f "$R/$name" ] && [ "$(sha256sum "$R/$name" | cut -c1-64)" = "$want" ]; then echo "$name: have it"; return; fi
  scp -q "$MAC:$M/release/$name" "$R/$name"
  got=$(sha256sum "$R/$name" | cut -c1-64)
  [ "$got" = "$want" ] || { echo "$name: sha256 $got, the release says $want"; exit 1; }
  echo "$name: fetched"
}
for name in droidvm-disk.img kernel.macho kernel.native.macho; do fetch "$name"; done
[ $IMAGES = 1 ] && for name in os.img systemos.img; do fetch "$name"; done
rm -rf "$R/plugin.new" && mkdir -p "$R/plugin.new"
ssh -o BatchMode=yes "$MAC" "cd $M/release/plugin && COPYFILE_DISABLE=1 tar --no-xattrs -cf - ." | tar xf - -C "$R/plugin.new"
python3 - "$R/release.json" "$R/plugin.new" <<'PY'
import hashlib, json, sys
from pathlib import Path
files, root = json.load(open(sys.argv[1]))['plugin']['files'], Path(sys.argv[2])
for name, want in files.items():
    got = hashlib.sha256((root / name).read_bytes()).hexdigest()
    if got != want:
        sys.exit(f'plugin/{name}: sha256 {got}, the release says {want}')
print(f'plugin: {len(files)} files fetched')
PY
rm -rf "$R/plugin" && mv "$R/plugin.new" "$R/plugin"

symbols() {          # the release's kernel and its symbols where the rig's tools look for them
  cp "$R/droidvm-disk.img" "$O/droidvm-disk.img"
  cp "$R/kernel.macho" "$O/kernel.macho"
  cp "$R/kernel.native.macho" "$O/kernel.native.macho"
  mcopy -o -i "$O/droidvm-disk.img" ::/EFI/UniXNU/Payloads/kernel.macho.adt "$O/kernel.macho.adt"
  mcopy -o -i "$O/droidvm-disk.img" ::/EFI/UniXNU/unixnu.toml "$O/unixnu.toml"
  llvm-nm -n --defined-only "$O/kernel.native.macho" > "$O/kernel.native.nm"
  # kernel.nm for the rig's tools: the native kernel's symbols at the addresses the collection gives them
  python3 "$RIG/fileset-syms.py" "$O/kernel.macho" "$O/kernel.native.macho" "$O/kernel.native.nm" "$O/kernel.nm" >/dev/null
}

if [ $NEWVM = 1 ]; then
  echo "== a new VM: $NAME in $DIR"
  case "$DIR" in /storage/emulated/0/DroidVM/*UniXNU*) ;; *) echo "VM_DIR must be /storage/emulated/0/DroidVM/<a name with UniXNU>"; exit 1 ;; esac
  MEDIA=/data/media/0/${DIR#/storage/emulated/0/}
  [ -z "$($A shell "su -c 'ls -d $MEDIA 2>/dev/null'")" ] || { echo "$DIR exists on the phone: not touched (a new VM takes a new directory)"; exit 1; }
  [ -f "$FROM/unixnu-vm.config.json" ] || { echo "no $FROM/unixnu-vm.config.json to copy the VM's configuration from"; exit 1; }
  [ -z "$($A shell "su -c 'for p in \$(pidof crosvm); do tr \"\\0\" \" \" < /proc/\$p/cmdline | grep -q UniXNU && echo \$p; done'" | tr -dc 0-9)" ] ||
    { echo "a UniXNU VM is running: stop it first (its rig's vm.sh stop)"; exit 1; }
  # the directory of FROM's VM, from its configuration's boot disk
  FROM_DIR=$(python3 -c "
import json, sys
config = json.load(open(sys.argv[1]))['config']
print([d['path'] for d in config['disks'] if d['path'].endswith('/droidvm-disk.img')][0].rsplit('/', 1)[0])" "$FROM/unixnu-vm.config.json")
  FROM_MEDIA=/data/media/0/${FROM_DIR#/storage/emulated/0/}
  if [ $IMAGES = 0 ]; then
    for name in os.img systemos.img; do
      want=$(field "$R/release.json" "files.$name.sha256")
      got=$($A shell "su -c 'sha256sum $FROM_MEDIA/$name'" | cut -c1-64)
      [ "$want" = "$got" ] || { echo "$FROM_MEDIA/$name is not the release's ($got); --images pushes the release's"; exit 1; }
      echo "$name: the VM reads $FROM_MEDIA/$name"
    done
  fi
  $A shell "mkdir -p $DIR"               # through the shared storage, which gives it the owner the app's other VM directories have
  FILE=$MEDIA/data-store.img "$RIG/make-data-store.sh" >/dev/null
  symbols
  $A push "$O/droidvm-disk.img" "$DIR/droidvm-disk.img" | tail -1
  $A push "$R/release.json" "$DIR/release.json" | tail -1
  if [ $IMAGES = 1 ]; then
    for name in os.img systemos.img; do $A push "$R/$name" "$DIR/$name" | tail -1; done
  fi
  ID=$(python3 - "$FROM/unixnu-vm.config.json" "$RIG/unixnu-vm.config.json" "$NAME" "$FROM_DIR" "$DIR" "$IMAGES" <<'PY'
import json, secrets, sys, time, uuid
source, output, name, old, new, images = sys.argv[1:]
old_media, new_media = (path.replace('/storage/emulated/0/', '/data/media/0/') for path in (old, new))
config = json.load(open(source))['config']
vm_id = str(uuid.uuid4())
text = json.dumps(config)
for mine in ('droidvm-disk.img', 'data-store.img') + (('os.img', 'systemos.img') if images == '1' else ()):
    for a, b in ((old, new), (old_media, new_media)):
        text = text.replace(f'{a}/{mine}', f'{b}/{mine}')
config = json.loads(text)
config.update(id=vm_id, name=name, created_at=int(time.time() * 1000), auto_up=False)
for index, network in enumerate(config.get('networks', [])):
    network['tap_name'] = f'vm{vm_id[:8]}-{index}'
    network['mac_address'] = '02:00:00:' + ':'.join(f'{byte:02x}' for byte in secrets.token_bytes(3))
# crosvm's dump of the device tree it builds, which each VM writes for itself
options = config.get('extra_options', [])
for index, option in enumerate(options):
    if option == '--dump-device-tree-blob' and index + 1 < len(options):
        options[index + 1] = f'/data/local/tmp/{new.rsplit("/", 1)[-1].lower()}-fdt.dtb'
open(output, 'w').write(json.dumps({'config': config}, indent=1) + '\n')
print(vm_id)
PY
)
  if grep -q '^VM=' "$RIG/rig.env" 2>/dev/null; then sed -i "s/^VM=.*/VM=$ID/" "$RIG/rig.env"; else echo "VM=$ID" >> "$RIG/rig.env"; fi
  export VM=$ID
  cp "$R/release.json" "$R/deployed.json"
  $A shell "su -c 'ls -la $MEDIA'"
  echo "VM $ID; its configuration is $RIG/unixnu-vm.config.json"
  echo "== boot"
  boot_vm || { echo "the new VM did not come up"; exit 1; }
  ip=$(timeout 200 python3 "$RIG/health.py" 2>/dev/null | sed -n 's/^network .*address //p' | tr ', ' '\n\n' | grep -E '^[0-9]+\.' | grep -v '^169\.254\.' | head -1 || true)
  if [ -n "$ip" ]; then
    if grep -q '^GUEST_IP=' "$RIG/rig.env"; then sed -i "s/^GUEST_IP=.*/GUEST_IP=$ip/" "$RIG/rig.env"; else echo "GUEST_IP=$ip" >> "$RIG/rig.env"; fi
    echo "guest address $ip"
  fi
  timeout 60 "$RIG/guest-ssh.sh" --key >/dev/null 2>&1 && echo "guest ssh key put" || echo "guest ssh key NOT put (guest-ssh.sh --key)"
  exit 0
fi

rm -rf "$R/plugin.replaced"
if [ $PLUGIN = 1 ]; then
  echo "== plugin into the guest's Data"
  GSSH_TIMEOUT=60 "$RIG/guest-ssh.sh" "$TOYBOX true" >/dev/null 2>&1 </dev/null ||
    { echo "the guest does not answer over ssh (GUEST_IP=${GUEST_IP:-unset}): start it, or --keep-plugin"; exit 1; }
  push_plugin "$R/plugin" "$R/plugin.replaced" || { echo "the guest's plugin is not the release's: not rebooting"; exit 1; }
  python3 -c "import time; time.sleep(15)"       # the guest's own writeback of what gfxprobe sync did not reach
fi

echo "== stop"
stop_vm

echo "== boot disk"
# What this deploy replaces, for --rollback: the phone's boot disk (whoever put it there) and the rig's symbols for it.
rm -rf "$P.new" && mkdir -p "$P.new"
if $A pull "$DIR/droidvm-disk.img" "$P.new/droidvm-disk.img" >/dev/null 2>&1; then
  for name in $SYMBOLS; do [ -f "$O/$name" ] && cp "$O/$name" "$P.new/$name"; done
  [ -f "$R/deployed.json" ] && cp "$R/deployed.json" "$P.new/release.json"
  [ -d "$R/plugin.replaced" ] && mv "$R/plugin.replaced" "$P.new/plugin"
  date '+%F %T' > "$P.new/replaced-at"
  rm -rf "$P" && mv "$P.new" "$P"
  echo "the previous boot disk is in $P (--rollback)"
else
  rm -rf "$P.new"; echo "the phone had no boot disk in $DIR to keep"
fi
symbols
$A shell "mkdir -p $DIR"
$A push "$O/droidvm-disk.img" "$DIR/droidvm-disk.img" | tail -1
$A push "$R/release.json" "$DIR/release.json" | tail -1
if [ $IMAGES = 1 ]; then
  for name in os.img systemos.img; do $A push "$R/$name" "$DIR/$name" | tail -1; done
fi
cp "$R/release.json" "$R/deployed.json"

echo "== boot"
boot_vm || { echo "the guest did not come up on the release's boot disk; deploy27.sh --rollback puts the previous one back"; exit 1; }
want=$(sha256sum "$R/plugin/NativeAdrenoMetal.bundle/NativeAdrenoMetal" | cut -c1-64)
got=$(GSSH_TIMEOUT=100 "$RIG/guest-ssh.sh" "$TOYBOX sha256sum $(guest_path NativeAdrenoMetal.bundle/NativeAdrenoMetal)" 2>/dev/null </dev/null | cut -c1-64 || true)
if [ "$want" = "$got" ]; then echo "plugin in the guest: the release's (${got:0:16})"
elif [ $PLUGIN = 0 ]; then echo "plugin in the guest: ${got:-unreadable} (kept; the release has ${want:0:16})"
else echo "PLUGIN in the guest: ${got:-unreadable}, the release has $want"; exit 1; fi
