#!/usr/bin/env python3
"""Build ipatool (package/ipatool/ipatool.m): the debug shell's .ipa installer -- unpack, scan (CDHash + FairPlay check),
install (MCM bundle container + LaunchServices registration) and remove (unregister + destroy containers).

  build-droidvm-ipatool.py TREE_ROOT

Ad-hoc 4K-signed and trusted through the owner module (build/owner-trust-extra/ipatool.json, category 3) like lsdiag. It
carries the SAME install/LaunchServices/MCM entitlement set as lsdiag (least privilege: it is a focused installer, not a
debugger -- the broad curated debug set is for applebox/debugserver). Zip is inflated with libz: the iphoneos SDK ships
libz.tbd (link -lz) but no zlib.h, so zlib.h is shimmed from the macOS SDK, as build-droidvm-toybox.py shims <sys/disk.h>.
Staged with
  export.py put build/ipatool/ipatool /private/var/ngpu/bin/ipatool 0755
and run as root (install's register step as mobile, -u 501) over ssh or by ownervisor. An app only *runs* once its Mach-Os are
trusted (boot trust cache or the planned runtime-trust SPI); ipatool reports the CDHashes, it does not grant trust."""
import json
import plistlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

FORK = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else sys.exit(__doc__)
SOURCE = Path(__file__).resolve().parents[1] / 'package/ipatool/ipatool.m'
OUT = FORK / 'build/ipatool'
WORK = OUT / 'work'
OUT.mkdir(parents=True, exist_ok=True)
WORK.mkdir(parents=True, exist_ok=True)

iphone_sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
macos_sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()

# shim: zlib.h from the macOS SDK (the iphoneos SDK omits it but ships libz.tbd).
shim = WORK / 'shim'
shim.mkdir(exist_ok=True)
shutil.copyfile(macos_sdk + '/usr/include/zlib.h', shim / 'zlib.h')

binary = OUT / 'ipatool'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', iphone_sdk, f'-I{shim}',
                '-O2', '-Wall', '-Wextra', '-Werror', '-fobjc-arc', '-framework', 'Foundation', '-framework', 'MobileCoreServices', '-lz',
                SOURCE, '-o', binary], check=True)

# The install/LaunchServices/MobileContainerManager set lsdiag proved, nothing more.
entitlements = OUT / 'entitlements.plist'
entitlements.write_bytes(plistlib.dumps({
    'com.apple.private.security.no-container': True,
    'com.apple.private.coreservices.canmaplsdatabase': True,
    'com.apple.lsapplicationworkspace.rebuildappdatabases': True,
    'com.apple.private.mobileinstall.allowedSPI': ['EnumerateInstalledAppsForLaunchServices', 'InstallForLaunchServices'],
    'com.apple.mobileinstallation.enumeration': True,
    'com.apple.private.coreservices.can-register-install-results': True,
    'com.apple.private.MobileContainerManager.allowed': True,
    'com.apple.private.security.storage.AppBundles': True,
    'com.apple.private.security.storage.AppDataContainers': True,
}))
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.droidvm.ipatool',
                '--entitlements', entitlements, binary], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', binary], capture_output=True, text=True, check=True).stderr
assert re.search(r'^Page size=4096$', detail, re.M), 'the signature must use 4 KiB pages'
cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]

extra = FORK / 'build/owner-trust-extra'
extra.mkdir(parents=True, exist_ok=True)
(extra / 'ipatool.json').write_text(json.dumps(
    {'cdhashes': [cdhash], 'category': 3, 'binary': 'build/ipatool/ipatool',
     'source': 'package/ipatool/ipatool.m, iphoneos arm64'}, indent=2) + '\n')
print('ipatool CDHash', cdhash)
