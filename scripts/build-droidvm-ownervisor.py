#!/usr/bin/env python3
"""Build ownervisor, ownerctl and ownertest (the guest's owner service manager and its PoC child), sign them ad hoc with
4 KiB pages like the Adreno probe daemon, and wire them into the owner trust module.

  build-droidvm-ownervisor.py TREE_ROOT

TREE_ROOT is the UniXNU build tree whose build/owner-trust-extra UniXNU's make-owner-trust-bundle.py reads; the three .m sources
are kits/droidvm/ownervisor/*.m there. The binaries run because their CDHashes are in the owner module (build/owner-trust-extra/ownervisor.json,
category 3 — unconstrained, as the Adreno probe daemon's com.apple.iomfb_fdr_loader is). launchd loads only the labels of its
prebuilt job set, so ownervisor takes the label of com.apple.mtmergeprops (multitouch calibration, unused with virtio input):
our plist in the Data volume's /private/var/ngpu/daemons shadows the original over /System/Library/LaunchDaemons
(sheng-gpu-addon=1), and launchd execs our ProgramArguments, exactly as it does the Adreno probe daemon's. ownervisor then
spawns the owner debug services itself, so one launchd slot stands in for many.

Staged by the rig (the binaries and plist persist in the writable Data store):
  export.py put build/droidvm-ownervisor/ownervisor /private/var/ngpu/bin/ownervisor 0755
  export.py put build/droidvm-ownervisor/ownertest  /private/var/ngpu/ownervisor/bin/ownertest 0755
  export.py put build/droidvm-ownervisor/ownerctl   /private/var/ngpu/ownervisor/bin/ownerctl 0755
  export.py put build/droidvm-ownervisor/com.apple.mtmergeprops.plist /private/var/ngpu/daemons/com.apple.mtmergeprops.plist
"""
import json
import plistlib
import re
import subprocess
import sys
from pathlib import Path

if len(sys.argv) != 2:
    sys.exit(__doc__)
FORK = Path(sys.argv[1]).resolve()
SRC = FORK / 'kits/droidvm/ownervisor'
OUT = FORK / 'build/droidvm-ownervisor'
OUT.mkdir(parents=True, exist_ok=True)
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
base = ['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-O2', '-Wall', '-Wextra', '-Werror',
        '-fobjc-arc', '-framework', 'Foundation']
entitlements = OUT / 'entitlements.plist'
entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True}))

cdhashes = []
for name in ('ownervisor', 'ownerctl', 'ownertest'):
    binary = OUT / name
    subprocess.run([*base, SRC / f'{name}.m', '-o', binary], check=True)
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', f'org.droidvm.{name}',
                    '--entitlements', entitlements, binary], check=True)
    detail = subprocess.run(['codesign', '--display', '--verbose=4', binary], capture_output=True, text=True, check=True).stderr
    assert re.search(r'^Page size=4096$', detail, re.M), 'the signature must use 4 KiB pages'
    cdhashes.append(re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1])
    print(f'{name} CDHash', cdhashes[-1])

# launchd starts ownervisor once; KeepAlive brings it back if it ever dies. It runs as root (mtmergeprops does), so it can
# start a service under any uid. stderr/stdout land in the Data volume so the rig can read them with export.py.
(OUT / 'com.apple.mtmergeprops.plist').write_bytes(plistlib.dumps(dict(
    Label='com.apple.mtmergeprops', ProgramArguments=['/private/var/ngpu/bin/ownervisor'], RunAtLoad=True, KeepAlive=True,
    ThrottleInterval=5, ProcessType='Interactive', UserName='root',
    StandardErrorPath='/private/var/ngpu/ownervisor.err', StandardOutPath='/private/var/ngpu/ownervisor.out'),
    fmt=plistlib.FMT_XML))

extra = FORK / 'build/owner-trust-extra'
extra.mkdir(parents=True, exist_ok=True)
(extra / 'ownervisor.json').write_text(json.dumps(
    {'cdhashes': cdhashes, 'category': 3, 'binaries': ['build/droidvm-ownervisor/' + n for n in ('ownervisor', 'ownerctl', 'ownertest')]},
    indent=2) + '\n')
print('wrote', extra / 'ownervisor.json')
