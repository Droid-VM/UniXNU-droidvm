#!/usr/bin/env python3
"""Build droidvm-diag (kits/droidvm/diag/droidvm-diag.m), the rig\'s question box inside the guest, and its launchd job.

The binary is signed ad hoc with 4 KiB pages like the Adreno probe daemon; it runs because its CDHash is in the owner trust module
(build/owner-trust-extra/droidvm-diag.json, read by UniXNU\'s make-owner-trust-bundle.py). launchd loads only the labels of its
prebuilt job set, and AMFI kills an ad-hoc binary launched under a label whose trust-cache constraint category is 1 (almost all
of them) with a "Launch Constraint Violation". So the job takes the label of com.apple.remindd, whose own binary is category 3
(unconstrained, as the Adreno probe daemon\'s com.apple.iomfb_fdr_loader is) and which this VM does not need; it runs as mobile,
the user SpringBoard and pkd run as, so LaunchServices answers the questions as they see them. The Data volume\'s
/private/var/ngpu/daemons is shown over /System/Library/LaunchDaemons (sheng-gpu-addon=1), and the binary runs from
/private/var/ngpu/bin, both staged by the rig:

  export.py put build/droidvm-diag/droidvm-diag /private/var/ngpu/bin/droidvm-diag 0755
  export.py put build/droidvm-diag/com.apple.remindd.plist /private/var/ngpu/daemons/com.apple.remindd.plist"""
import json
import plistlib
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/droidvm-diag'
OUT.mkdir(parents=True, exist_ok=True)
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
binary = OUT / 'droidvm-diag'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-O2', '-Wall', '-Wextra', '-Werror',
                '-fobjc-arc', '-framework', 'Foundation', ROOT / 'kits/droidvm/diag/droidvm-diag.m', '-o', binary], check=True)
entitlements = OUT / 'entitlements.plist'
entitlements.write_bytes(plistlib.dumps({
    'com.apple.private.security.no-container': True,
    'com.apple.private.coreservices.canmaplsdatabase': True,
    'com.apple.private.persona-mgmt': True,
}))
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.droidvm.diag', '--entitlements',
                entitlements, binary], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', binary], capture_output=True, text=True, check=True).stderr
cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]
assert re.search(r'^Page size=4096$', detail, re.M), 'the signature must use 4 KiB pages'
(OUT / 'com.apple.remindd.plist').write_bytes(plistlib.dumps(dict(
    Label='com.apple.remindd', ProgramArguments=['/private/var/ngpu/bin/droidvm-diag'], RunAtLoad=True, KeepAlive=True,
    ThrottleInterval=5, UserName='mobile', StandardErrorPath='/dev/null'), fmt=plistlib.FMT_XML))
extra = ROOT / 'build/owner-trust-extra'
extra.mkdir(parents=True, exist_ok=True)
(extra / 'droidvm-diag.json').write_text(json.dumps({'cdhashes': [cdhash], 'category': 3, 'binary': 'build/droidvm-diag/droidvm-diag'},
                                                   indent=2) + '\n')
print('droidvm-diag CDHash', cdhash)
