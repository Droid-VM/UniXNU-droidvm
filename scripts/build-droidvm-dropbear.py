#!/usr/bin/env python3
"""Build dropbear for the guest: a pubkey-only ssh server, so the rig gets an interactive shell into the guest over IPv6.

  build-droidvm-dropbear.py TREE_ROOT

Dropbear carries its own crypto (libtomcrypt/libtommath), so it needs no OpenSSL -- which the iphoneos SDK lacks -- and only
openpty, which the SDK has. localoptions.h turns off password auth (no crypt() on iOS), PAM, utmp/wtmp/lastlog, and points the
default host-key files and $PATH at /private/var/ngpu. Three small source edits fit it to this VM, which is not a security
boundary and has no writable /etc or home: the login shell is zsh (/bin/sh does not exist), and authorized_keys is read from
/private/var/ngpu/ssh (the rig writes there with export.py put; /var/root is not writable by it). dropbear and dropbearkey are
signed ad-hoc 4K WITH com.apple.private.security.no-container -- without it AMFI's launch constraints (owner-skip-launch-
constraints clears them for a kernel spawn, but a plain execve by an owner process is still refused) stop dropbear from exec'ing
the shell. Trusted through the owner module (category 3), baked at /private/var/ngpu/bin by scripts/add-ownervisor-data.py and
run as an ownervisor service.

Needs the iphoneos SDK."""
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

TAG = 'DROPBEAR_2022.83'
URL = f'https://github.com/mkj/dropbear/archive/refs/tags/{TAG}.tar.gz'
TARBALL_SHA256 = 'e02c5c36eb53bfcd3f417c6e40703a50ec790a1a772269ea156a2ccef14998d2'
SHELL = '/private/var/ngpu/bin/zsh'
AUTHKEYS = '/private/var/ngpu/ssh/authorized_keys'

FORK = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else sys.exit(__doc__)
OUT = FORK / 'build/dropbear'
WORK = OUT / 'work'
OUT.mkdir(parents=True, exist_ok=True)
WORK.mkdir(parents=True, exist_ok=True)
iphone_sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()

tarball = WORK / f'{TAG}.tar.gz'
if not tarball.is_file() or hashlib.sha256(tarball.read_bytes()).hexdigest() != TARBALL_SHA256:
    urllib.request.urlretrieve(URL, tarball)
if hashlib.sha256(tarball.read_bytes()).hexdigest() != TARBALL_SHA256:
    sys.exit('dropbear tarball sha256 mismatch')
src = WORK / f'dropbear-{TAG}'
if src.exists():
    shutil.rmtree(src)
with tarfile.open(tarball) as t:
    t.extractall(WORK, filter='data')

cc = WORK / 'ios-clang'
cc.write_text(f'#!/bin/sh\nexec xcrun -sdk iphoneos clang -arch arm64 -miphoneos-version-min=17.0 -isysroot {iphone_sdk} "$@"\n')
cc.chmod(0o755)

(src / 'localoptions.h').write_text(
    '#define DROPBEAR_SVR_PASSWORD_AUTH 0\n#define DROPBEAR_SVR_PAM_AUTH 0\n#define DROPBEAR_SVR_PUBKEY_AUTH 1\n'
    '#define DROPBEAR_SVR_MULTIUSER 1\n#define DROPBEAR_UTMP 0\n#define DROPBEAR_WTMP 0\n#define DROPBEAR_LASTLOG 0\n'
    '#define DROPBEAR_PUTUTLINE 0\n#define DO_MOTD 0\n#define DEFAULT_PATH "/private/var/ngpu/bin"\n'
    '#define ED25519_PRIV_FILENAME "/private/var/ngpu/ssh/dropbear_ed25519_host_key"\n'
    '#define RSA_PRIV_FILENAME "/private/var/ngpu/ssh/dropbear_rsa_host_key"\n'
    '#define ECDSA_PRIV_FILENAME "/private/var/ngpu/ssh/dropbear_ecdsa_host_key"\n'
    '#define DSS_PRIV_FILENAME "/private/var/ngpu/ssh/dropbear_dss_host_key"\n')


def patch(path, old, new):
    p = src / path
    s = p.read_text()
    if s.count(old) != 1:
        sys.exit(f'{path}: expected exactly one match to patch, found {s.count(old)}')
    p.write_text(s.replace(old, new))


# The login shell is zsh; /bin/sh (root's pw_shell) does not exist on stock iPadOS.
patch('common-session.c',
      '\tif (ses.authstate.pw_shell[0] == \'\\0\') {\n\t\treturn "/bin/sh";\n\t} else {\n\t\treturn ses.authstate.pw_shell;\n\t}',
      f'\t(void)ses.authstate.pw_shell;\n\treturn "{SHELL}";')
# authorized_keys lives under /private/var/ngpu/ssh (the rig can write there; /var/root it cannot).
patch('svr-authpubkey.c',
      '\t\tlen = strlen(ses.authstate.pw_dir);\n\t\t/* allocate max required pathname storage,\n'
      '\t\t * = path + "/.ssh/authorized_keys" + \'\\0\' = pathlen + 22 */\n\t\tfilename = m_malloc(len + 22);\n'
      '\t\tsnprintf(filename, len + 22, "%s/.ssh/authorized_keys",\n\t\t\t\t\tses.authstate.pw_dir);',
      f'\t\tfilename = m_strdup("{AUTHKEYS}");')
patch('svr-authpubkey.c',
      '\tif (ses.authstate.pw_dir == NULL) {\n\t\tgoto out;\n\t}\n\n'
      '\tif ((len = strlen(ses.authstate.pw_dir)) == 0) {\n\t\tgoto out;\n\t}\n\n'
      '\t/* allocate max required pathname storage,\n\t * = path + "/.ssh/authorized_keys" + \'\\0\' = pathlen + 22 */\n'
      '\tlen += 22;\n\tfilename = m_malloc(len);\n\tstrlcpy(filename, ses.authstate.pw_dir, len);\n\n'
      '\t/* check ~ */\n\tif (checkfileperm(filename) != DROPBEAR_SUCCESS) {\n\t\tgoto out;\n\t}\n\n'
      '\t/* check ~/.ssh */\n\tstrlcat(filename, "/.ssh", len);\n\tif (checkfileperm(filename) != DROPBEAR_SUCCESS) {\n\t\tgoto out;\n\t}\n\n'
      '\t/* now check ~/.ssh/authorized_keys */\n\tstrlcat(filename, "/authorized_keys", len);\n\tif (checkfileperm(filename) != DROPBEAR_SUCCESS) {\n\t\tgoto out;\n\t}',
      f'\t(void)len;\n\tfilename = m_strdup("{AUTHKEYS}");\n\tif (checkfileperm(filename) != DROPBEAR_SUCCESS) {{\n\t\tgoto out;\n\t}}')

subprocess.run(['./configure', '--host=arm-apple-darwin', f'CC={cc}', '--disable-zlib',
                '--disable-lastlog', '--disable-utmp', '--disable-wtmp'], cwd=src, check=True, stdout=subprocess.DEVNULL)
subprocess.run(['make', 'PROGRAMS=dropbear dropbearkey', '-j10'], cwd=src, check=True, stdout=subprocess.DEVNULL)

entitlements = OUT / 'entitlements.plist'
entitlements.write_text('<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
                        '"http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n<plist version="1.0"><dict>'
                        '<key>com.apple.private.security.no-container</key><true/></dict></plist>\n')
cdhashes = []
for name in ('dropbear', 'dropbearkey'):
    binary = OUT / name
    binary.unlink(missing_ok=True)
    shutil.copyfile(src / name, binary)
    binary.chmod(0o755)
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', f'org.droidvm.{name}',
                    '--entitlements', entitlements, binary], check=True)
    detail = subprocess.run(['codesign', '--display', '--verbose=4', binary], capture_output=True, text=True, check=True).stderr
    assert re.search(r'^Page size=4096$', detail, re.M)
    cdhashes.append(re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1])
    print(f'{name} CDHash', cdhashes[-1])

extra = FORK / 'build/owner-trust-extra'
extra.mkdir(parents=True, exist_ok=True)
(extra / 'dropbear.json').write_text(json.dumps(
    {'cdhashes': cdhashes, 'category': 3,
     'source': f'dropbear {TAG}, pubkey only, zsh login shell, authkeys+hostkeys under /private/var/ngpu/ssh, no-container'},
    indent=2) + '\n')
