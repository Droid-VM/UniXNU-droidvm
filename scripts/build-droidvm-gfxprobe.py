#!/usr/bin/env python3
"""Build gfxprobe (package/gfxprobe/gfxprobe.m): makes the IOSurfaces RenderBox and QuartzCore make and asks the guest's Metal
device for a texture on each -- why a SwiftUI widget's RBLayer gets "no surface".

  build-droidvm-gfxprobe.py TREE_ROOT

Ad-hoc 4K-signed and trusted through the owner module (build/owner-trust-extra/gfxprobe.json, category 3) like lsdiag, with
no-container so an owner process may exec it, and the IOSurface and GPU user-client classes. `gfxprobe sync PATH...` also flushes
files staged into the Data overlay before a VM stop. Staged with
  export.py put build/gfxprobe/gfxprobe /private/var/ngpu/bin/gfxprobe 0755
and run as root over ssh."""
import json
import plistlib
import re
import subprocess
import sys
from pathlib import Path

FORK = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else sys.exit(__doc__)
SOURCE = Path(__file__).resolve().parents[1] / 'package/gfxprobe/gfxprobe.m'
OUT = FORK / 'build/gfxprobe'
OUT.mkdir(parents=True, exist_ok=True)

sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
binary = OUT / 'gfxprobe'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-O2', '-Wall', '-Wextra', '-Werror',
                '-fno-objc-arc', '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
                '-framework', 'QuartzCore', '-framework', 'IOKit', SOURCE, '-o', binary], check=True)
entitlements = OUT / 'entitlements.plist'
# The platform sandbox refuses IOServiceOpen of a user-client class a process is not granted: the surface and GPU clients it opens.
entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True,
                                         'com.apple.security.iokit-user-client-class': ['IOSurfaceRootUserClient',
                                                                                        'AGXDeviceUserClient']}))
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.droidvm.gfxprobe',
                '--entitlements', entitlements, binary], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', binary], capture_output=True, text=True, check=True).stderr
assert re.search(r'^Page size=4096$', detail, re.M)
cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]

extra = FORK / 'build/owner-trust-extra'
extra.mkdir(parents=True, exist_ok=True)
(extra / 'gfxprobe.json').write_text(json.dumps(
    {'cdhashes': [cdhash], 'category': 3, 'binary': 'build/gfxprobe/gfxprobe',
     'source': 'package/gfxprobe/gfxprobe.m, iphoneos arm64'}, indent=2) + '\n')
print('gfxprobe CDHash', cdhash)
