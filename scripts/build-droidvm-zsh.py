#!/usr/bin/env python3
"""Build zsh for the guest: a real interactive shell (apple-oss-distributions/zsh, the one macOS ships), for the run verb and
later an ssh login shell.

  build-droidvm-zsh.py TREE_ROOT

Apple's zsh already cross-compiles for embedded: its Makefile uses configure.cache-embedded for the values the generic
cross-configure gets wrong, which this reuses. Two shims only: the ncurses headers (libncurses is in the iphoneos SDK but its
headers are not, so they come from the macOS SDK, as toybox's <sys/disk.h> does), and <System/sys/codesign.h> -- Apple's
init.c calls csops(CS_OPS_STATUS) to spot the installer environment; the header is private, so a tiny stub supplies the two
names (values from XNU bsd/sys/codesign.h, osfmk/kern/cs_blobs.h) and _csops is exported by the iphoneos libSystem. Modules are
static (--disable-dynamic), so the one binary needs no module directory. Result: ad-hoc 4K-signed, trusted through the owner
module (category 3), baked at /private/var/ngpu/bin/zsh by scripts/add-ownervisor-data.py.

Needs the macOS + iphoneos SDKs."""
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

TAG = 'zsh-118'  # zsh 5.9
URL = f'https://github.com/apple-oss-distributions/zsh/archive/refs/tags/{TAG}.tar.gz'
TARBALL_SHA256 = '9ee07990dca582d977ecb99f0f4396c301c5d197e29e9ea6be1d23fbe0ec9ce0'

FORK = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else sys.exit(__doc__)
OUT = FORK / 'build/zsh'
WORK = OUT / 'work'
OUT.mkdir(parents=True, exist_ok=True)
WORK.mkdir(parents=True, exist_ok=True)

iphone_sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
macos_sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()

tarball = WORK / f'{TAG}.tar.gz'
if not tarball.is_file() or hashlib.sha256(tarball.read_bytes()).hexdigest() != TARBALL_SHA256:
    urllib.request.urlretrieve(URL, tarball)
if hashlib.sha256(tarball.read_bytes()).hexdigest() != TARBALL_SHA256:
    sys.exit('zsh tarball sha256 mismatch')
top = WORK / f'zsh-{TAG}'
if top.exists():
    shutil.rmtree(top)
with tarfile.open(tarball) as t:
    t.extractall(WORK, filter='data')

# shims: ncurses headers from the macOS SDK, and a stub for the private <System/sys/codesign.h>.
shim = WORK / 'shim'
(shim / 'System/sys').mkdir(parents=True, exist_ok=True)
for header in ('curses.h', 'ncurses.h', 'ncurses_dll.h', 'term.h', 'termcap.h', 'unctrl.h'):
    shutil.copyfile(macos_sdk + '/usr/include/' + header, shim / header)
(shim / 'System/sys/codesign.h').write_text(
    '#pragma once\n#include <sys/types.h>\n#define CS_OPS_STATUS 0\n#define CS_INSTALLER 0x00000008\n'
    'int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);\n')
cc = WORK / 'ios-clang'
cc.write_text(f'#!/bin/sh\nexec xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -isysroot {iphone_sdk} -I{shim} "$@"\n')
cc.chmod(0o755)

src = top / 'zsh'
cache = WORK / 'config.cache'
shutil.copyfile(top / 'configure.cache-embedded', cache)
subprocess.run([
    './configure', '--host=arm-apple-darwin', f'CC={cc}', 'CPPFLAGS=-DUSE_GETCWD',
    '--prefix=/private/var/ngpu/zsh', '--enable-etcdir=/private/var/ngpu/etc',
    '--with-tcsetpgrp', '--enable-multibyte', '--enable-unicode9', '--enable-max-function-depth=700',
    '--disable-dynamic', '--disable-gdbm', '--disable-pcre', '--with-term-lib=ncurses',
    f'--cache-file={cache}'], cwd=src, check=True, stdout=subprocess.DEVNULL)
subprocess.run(['make', '-j10'], cwd=src, check=True, stdout=subprocess.DEVNULL)

binary = OUT / 'zsh'
binary.unlink(missing_ok=True)
shutil.copyfile(src / 'Src/zsh', binary)
binary.chmod(0o755)
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.droidvm.zsh', binary], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', binary], capture_output=True, text=True, check=True).stderr
assert re.search(r'^Page size=4096$', detail, re.M)
cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]

extra = FORK / 'build/owner-trust-extra'
extra.mkdir(parents=True, exist_ok=True)
(extra / 'zsh.json').write_text(json.dumps(
    {'cdhashes': [cdhash], 'category': 3, 'binary': 'build/zsh/zsh',
     'source': f'apple-oss-distributions/zsh {TAG} (zsh 5.9), iphoneos arm64, static modules'}, indent=2) + '\n')
print('zsh CDHash', cdhash)
