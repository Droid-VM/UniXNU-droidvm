#!/usr/bin/env bash
# Rebuild the Metal plugin (NativeAdrenoMetal.bundle: the NativeMetal frontend, metal2vulkan, Turnip) on the build Mac from the
# local fork's plugin scripts and metal2vulkan patches, keep the plugin running now trusted beside the new one, stage the new one
# into the guest's Data (flushed with gfxprobe sync before anything stops the VM: a stop right after a put truncated the binary),
# then payload-redeploy.sh (new trust, reboot) and check the plugin survived the reboot.
#   plugin-redeploy.sh        FORK: the local UniXNU fork checkout whose scripts/ and patches/metal2vulkan/ are built
# Those are copied to the Mac; anything else the plugin is built from (the NativeMetal submodule above all) must already be there:
# mac-sync-check.sh refuses the build otherwise.
set -u
RIG=$(cd "$(dirname "$0")" && pwd)
. "$RIG/rigenv.sh"
MAC=${BUILD_HOST:?set BUILD_HOST to the build Mac, user@host}
M=${BUILD_OUT:?set BUILD_OUT to out/droidvm of the UniXNU checkout on the build Mac}
F=${FORK:?set FORK to the local UniXNU fork checkout}
for f in build-adreno-metal-plugin.py build-native-metal-bridge.py; do
  scp -q "$F/scripts/$f" "$MAC:$M/../../scripts/$f" && scp -q "$F/scripts/$f" "$MAC:$M/tree/scripts/$f" || exit 1
done
for d in "$M/../../patches/metal2vulkan/" "$M/tree/patches/metal2vulkan/"; do scp -q "$F"/patches/metal2vulkan/*.patch "$MAC:$d" || exit 1; done
if [ "${SYNC_CHECK:-1}" != 0 ]; then
  "$RIG/mac-sync-check.sh" || { echo "the Mac does not build the source here, e.g. NativeMetal edits (SYNC_CHECK=0 to build anyway)"; exit 1; }
fi
ssh "$MAC" "T=$M/tree; mkdir -p $M/rig-logs; cd \$T && python3 - \$T <<'PY'
import json, sys
T = sys.argv[1]; p = T + '/build/owner-trust-extra/plugin-seed.json'; d = json.load(open(p))
old = json.load(open(T + '/build/adreno-metal-plugin/trust.json'))
for h in (old['cdhash'], old['daemon_cdhash']):
    if h not in d['cdhashes']: d['cdhashes'].append(h)
open(p, 'w').write(json.dumps(d, indent=2) + '\n')
PY
export PATH=\$HOME/.cargo/bin:/opt/homebrew/bin:\$PATH
python3 scripts/build-adreno-metal-plugin.py > $M/rig-logs/plugin.log 2>&1 || { tail -5 $M/rig-logs/plugin.log; exit 1; }
python3 - \$T <<'PY'
import json, sys
T = sys.argv[1]; p = T + '/build/owner-trust-extra/plugin-seed.json'; d = json.load(open(p))
new = json.load(open(T + '/build/adreno-metal-plugin/trust.json'))
d['cdhashes'] = [h for h in d['cdhashes'] if h not in (new['cdhash'], new['daemon_cdhash'], new['turnip_cdhash'])]
open(p, 'w').write(json.dumps(d, indent=2) + '\n'); print('kept', len(d['cdhashes']), 'old CDHashes; new plugin', new['cdhash'])
PY" || { echo "plugin build failed"; exit 1; }
cd "$RIG"
rm -rf out/plugin; mkdir -p out/plugin
scp -q -r "$MAC:$M/tree/build/adreno-metal-plugin/NativeAdrenoMetal.bundle" out/plugin/
B=/private/var/ngpu/addons/NativeAdrenoMetal.bundle
for f in NativeAdrenoMetal:0755 libvulkan_freedreno.dylib:0755 _CodeSignature/CodeResources:0644 Info.plist:0644; do
  timeout 600 python3 export.py put "out/plugin/NativeAdrenoMetal.bundle/${f%%:*}" "$B/${f%%:*}" "${f##*:}" 2>&1 | tail -1
done
WANT=$(md5sum out/plugin/NativeAdrenoMetal.bundle/NativeAdrenoMetal | cut -c1-32)
GSSH_TIMEOUT=200 ./guest-ssh.sh "/private/var/ngpu/bin/gfxprobe sync $B/NativeAdrenoMetal $B/libvulkan_freedreno.dylib $B/_CodeSignature/CodeResources $B/Info.plist"
python3 -c "import time; time.sleep(20)"
./payload-redeploy.sh 2>&1 | tail -4
GOT=$(GSSH_TIMEOUT=100 ./guest-ssh.sh "/private/var/ngpu/bin/toybox md5sum $B/NativeAdrenoMetal" 2>&1 | cut -c1-32)
[ "$WANT" = "$GOT" ] && echo "plugin persisted $GOT" || echo "PLUGIN MISMATCH: want $WANT got $GOT"
