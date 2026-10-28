#!/usr/bin/env python3
"""Build toybox for the guest: one trusted multicall binary with ~137 coreutils applets and a shell, so the run verb
(ownervisor, kits/ownervisor) has real tools on an otherwise bare iPadOS (/bin holds only df and ps).

  build-droidvm-toybox.py TREE_ROOT

toybox is Linux-first; it builds for Darwin/iOS with two shims only: GNU sed for its build scripts (the stock macOS BSD sed
mis-parses them), and the macOS SDK's <sys/disk.h>, which the iphoneos SDK omits. Applets that need Linux-only headers
(CONFIG_ROUTE -> linux/rtnetlink.h) stay off; CONFIG_SH is turned on for a shell. The result is ad-hoc 4K-signed like the
Adreno probe daemon and trusted through the owner module (build/owner-trust-extra/toybox.json, category 3), then baked into the
Data seed at /private/var/ngpu/bin/toybox by scripts/add-ownervisor-data.py. Apple's own file_cmds/shell_cmds/network_cmds/zsh
(apple-oss-distributions) build cleaner and are the route for tools toybox cannot do on iOS (netstat/ifconfig, a full shell).

Needs gsed (brew install gnu-sed) and the macOS + iphoneos SDKs."""
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

VERSION = '0.8.11'
URL = f'https://landley.net/toybox/downloads/toybox-{VERSION}.tar.gz'
TARBALL_SHA256 = '15aa3f832f4ec1874db761b9950617f99e1e38144c22da39a71311093bfe67dc'

FORK = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else sys.exit(__doc__)
OUT = FORK / 'build/toybox'
WORK = OUT / 'work'
OUT.mkdir(parents=True, exist_ok=True)
WORK.mkdir(parents=True, exist_ok=True)

gsed = shutil.which('gsed') or sys.exit('need gsed (brew install gnu-sed)')
iphone_sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
macos_sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()

tarball = WORK / f'toybox-{VERSION}.tar.gz'
if not tarball.is_file() or hashlib.sha256(tarball.read_bytes()).hexdigest() != TARBALL_SHA256:
    urllib.request.urlretrieve(URL, tarball)
if hashlib.sha256(tarball.read_bytes()).hexdigest() != TARBALL_SHA256:
    sys.exit('toybox tarball sha256 mismatch')
src = WORK / f'toybox-{VERSION}'
if src.exists():
    shutil.rmtree(src)
with tarfile.open(tarball) as t:
    t.extractall(WORK, filter='data')

# shims: GNU sed as `sed` on PATH for the build scripts; the macOS <sys/disk.h> the iphoneos SDK lacks.
gnubin = WORK / 'gnubin'
gnubin.mkdir(exist_ok=True)
(gnubin / 'sed').unlink(missing_ok=True)
(gnubin / 'sed').symlink_to(gsed)
shim = WORK / 'shim'
(shim / 'sys').mkdir(parents=True, exist_ok=True)
shutil.copyfile(macos_sdk + '/usr/include/sys/disk.h', shim / 'sys/disk.h')
cc = WORK / 'ios-clang'
cc.write_text(f'#!/bin/sh\nexec xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -isysroot {iphone_sdk} -I{shim} "$@"\n')
cc.chmod(0o755)

env = dict(os.environ, PATH=f'{gnubin}:{os.environ["PATH"]}')
subprocess.run(['make', 'macos_defconfig'], cwd=src, env=env, check=True, stdout=subprocess.DEVNULL)
config = (src / '.config').read_text()
config = config.replace('# CONFIG_SH is not set', 'CONFIG_SH=y').replace('CONFIG_ROUTE=y', '# CONFIG_ROUTE is not set')
(src / '.config').write_text(config)
subprocess.run(['make', 'oldconfig'], cwd=src, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, input='\n' * 50, text=True)
subprocess.run(['make', f'CC={cc}', 'HOSTCC=clang', 'toybox'], cwd=src, env=env, check=True)

binary = OUT / 'toybox'
binary.unlink(missing_ok=True)
shutil.copyfile(src / 'toybox', binary)
binary.chmod(0o755)
# no-container: an owner tool without it is refused (AMFI launch constraints) when another owner process execs it.
entitlements = OUT / 'entitlements.plist'
entitlements.write_text('<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
                        '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict>'
                        '<key>com.apple.private.security.no-container</key><true/></dict></plist>\n')
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.droidvm.toybox',
                '--entitlements', entitlements, binary], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', binary], capture_output=True, text=True, check=True).stderr
assert re.search(r'^Page size=4096$', detail, re.M)
cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]

extra = FORK / 'build/owner-trust-extra'
extra.mkdir(parents=True, exist_ok=True)
(extra / 'toybox.json').write_text(json.dumps(
    {'cdhashes': [cdhash], 'category': 3, 'binary': 'build/toybox/toybox',
     'source': f'toybox {VERSION}, macos_defconfig + CONFIG_SH, iphoneos arm64'}, indent=2) + '\n')
print('toybox CDHash', cdhash)
