#!/usr/bin/env python3
"""Build lsdiag (package/lsdiag/lsdiag.m): asks the guest's LaunchServices what it has registered -- an app's placeholder,
restricted and installed state, each extension's containing bundle, and the restriction manager's lists.

  build-droidvm-lsdiag.py TREE_ROOT

Ad-hoc 4K-signed and trusted through the owner module (build/owner-trust-extra/lsdiag.json, category 3) like toybox. Besides
no-container (or another owner process may not exec it), it carries com.apple.private.coreservices.canmaplsdatabase: without
it LaunchServices answers a client only about the client itself; rebuildappdatabases lets `lsdiag rebuild` ask for a rebuild,
the mobileinstall allowedSPI EnumerateInstalledAppsForLaunchServices lets `lsdiag midump` list what installd gives it, and
InstallForLaunchServices lets `lsdiag register` re-register an app (lsd's canRegisterWithOptions: checks it), and
can-register-install-results lets `lsdiag relink` register an app with its plug-ins as an install does. Staged with
  export.py put build/lsdiag/lsdiag /private/var/ngpu/bin/lsdiag 0755
and run as root over ssh; `lsdiag -u 501 ...` reads the mobile user's database, which the apps and widgets use."""
import json
import plistlib
import re
import subprocess
import sys
from pathlib import Path

FORK = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else sys.exit(__doc__)
SOURCE = Path(__file__).resolve().parents[1] / 'package/lsdiag/lsdiag.m'
OUT = FORK / 'build/lsdiag'
OUT.mkdir(parents=True, exist_ok=True)

sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
binary = OUT / 'lsdiag'
# Manual reference counting: the records are made with alloc + an init sent through a cast objc_msgSend, which ARC would
# not see as consuming the allocation (it released it a second time, a crash). A short-lived tool can leak instead.
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-O2', '-Wall', '-Wextra', '-Werror',
                '-fno-objc-arc', '-framework', 'Foundation', SOURCE, '-o', binary], check=True)
entitlements = OUT / 'entitlements.plist'
entitlements.write_bytes(plistlib.dumps({'com.apple.private.security.no-container': True,
                                         'com.apple.private.coreservices.canmaplsdatabase': True,
                                         'com.apple.lsapplicationworkspace.rebuildappdatabases': True,
                                         'com.apple.private.mobileinstall.allowedSPI': ['EnumerateInstalledAppsForLaunchServices',
                                                                                        'InstallForLaunchServices'],
                                         'com.apple.mobileinstallation.enumeration': True,
                                         'com.apple.private.coreservices.can-register-install-results': True,
                                         'com.apple.private.MobileContainerManager.allowed': True,
                                         'com.apple.private.security.storage.AppDataContainers': True}))
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.droidvm.lsdiag',
                '--entitlements', entitlements, binary], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', binary], capture_output=True, text=True, check=True).stderr
assert re.search(r'^Page size=4096$', detail, re.M)
cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]

extra = FORK / 'build/owner-trust-extra'
extra.mkdir(parents=True, exist_ok=True)
(extra / 'lsdiag.json').write_text(json.dumps(
    {'cdhashes': [cdhash], 'category': 3, 'binary': 'build/lsdiag/lsdiag',
     'source': 'package/lsdiag/lsdiag.m, iphoneos arm64'}, indent=2) + '\n')
print('lsdiag CDHash', cdhash)
