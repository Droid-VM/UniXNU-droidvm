#!/usr/bin/env python3
"""Build libimobiledevice for the guest: its 18 tools (ideviceinfo, idevicepair, idevicesyslog, idevicedebug,
idevicedebugserverproxy, ...) with libusbmuxd's iproxy and inetcat and libplist's plistutil, as one multicall binary of the
debug shell -- argv[0] (a link named after the tool) or `libimobiledevice TOOL ...` selects the tool.

  build-droidvm-libimobiledevice.py TREE_ROOT

Cross-compiled for iphoneos arm64 from the release tarballs (they ship configure: the Mac has no autoreconf or pkg-config),
each pinned by its SHA-256: libplist 2.6.0, libimobiledevice-glue 1.3.1, libusbmuxd 2.1.0, OpenSSL 3.0.15 (static, ios64-cross)
and libimobiledevice 1.3.0, all static. pkg-config is bypassed with the <module>_CFLAGS/_LIBS variables configure reads.
libimobiledevice 1.3.0's own PLIST_FORMAT_XML/BINARY enumerators collide with libplist 2.6.0's: they are renamed in its tree.

One binary, because each tool linked alone carries OpenSSL (3.8 MB each, 70 MB for the suite: more than the payload, which
holds the Data seed, has room for). Each tool's object is partial-linked (ld -r) with its main aliased to <tool>_main and
only that exported, so the tools' own globals stay private to them; a generated dispatcher and the static libraries are
linked once. Signed ad hoc with 4 KiB pages, no-container and the network client/server entitlements, and
trusted through the owner module (build/owner-trust-extra/libimobiledevice.json, category 3, which also lists the tool names
add-ownervisor-data.py links to it). Output: build/libimobiledevice/libimobiledevice.

The device tools reach a device through usbmuxd, which the guest does not have (pairing is the parked track of
plans/UNIXNUSETTING.md sec 4): they report "No device found"; plistutil works on its own."""
import hashlib
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

TREE = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else sys.exit(__doc__)
OUT = TREE / 'build/libimobiledevice'
SRC, STAGE, OBJ = OUT / 'src', OUT / 'stage', OUT / 'obj'
for d in (SRC, STAGE, OBJ):
    d.mkdir(parents=True, exist_ok=True)

GH = 'https://github.com/libimobiledevice'
SOURCES = {   # name: (version, url, sha256)
    'libplist': ('2.6.0', f'{GH}/libplist/releases/download/2.6.0/libplist-2.6.0.tar.bz2',
                 '67be9ee3169366589c92dc7c22809b90f51911dd9de22520c39c9a64fb047c9c'),
    'libimobiledevice-glue': ('1.3.1', f'{GH}/libimobiledevice-glue/releases/download/1.3.1/libimobiledevice-glue-1.3.1.tar.bz2',
                              '6e2849f221e6ab970566a115d42f3c20f8848e4d40c2ed61ac20dc85f40fa54f'),
    'libusbmuxd': ('2.1.0', f'{GH}/libusbmuxd/releases/download/2.1.0/libusbmuxd-2.1.0.tar.bz2',
                   'c35bf68f8e248434957bd5b234c389b02206a06ecd9303a7fb931ed7a5636b16'),
    'openssl': ('3.0.15', 'https://github.com/openssl/openssl/releases/download/openssl-3.0.15/openssl-3.0.15.tar.gz',
                '23c666d0edf20f14249b3d8f0368acaee9ab585b09e1de82107c66e1f3ec9533'),
    'libimobiledevice': ('1.3.0', f'{GH}/libimobiledevice/releases/download/1.3.0/libimobiledevice-1.3.0.tar.bz2',
                         '53f2640c6365cd9f302a6248f531822dc94a6cced3f17128d4479a77bd75b0f6'),
}
# tool: the object its build leaves, under the source tree it comes from
TOOLS = {name: ('libimobiledevice', f'tools/{name}-{name}.o') for name in [
    'idevice_id', 'idevicebackup', 'idevicebackup2', 'idevicecrashreport', 'idevicedate', 'idevicedebug',
    'idevicedebugserverproxy', 'idevicediagnostics', 'ideviceenterrecovery', 'ideviceimagemounter', 'ideviceinfo', 'idevicename',
    'idevicenotificationproxy', 'idevicepair', 'ideviceprovision', 'idevicescreenshot', 'idevicesetlocation', 'idevicesyslog']}
TOOLS.update({'iproxy': ('libusbmuxd', 'tools/iproxy-iproxy.o'), 'inetcat': ('libusbmuxd', 'tools/inetcat-inetcat.o'),
              'plistutil': ('libplist', 'tools/plistutil.o')})
LIBS = ['libimobiledevice-1.0.a', 'libusbmuxd-2.0.a', 'libimobiledevice-glue-1.0.a', 'libplist-2.0.a', 'libssl.a', 'libcrypto.a']

iphone_sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
clang = subprocess.check_output(['xcrun', '-f', 'clang'], text=True).strip()
target = ['-arch', 'arm64', '-miphoneos-version-min=17.0', '-isysroot', iphone_sdk]
S = str(STAGE)
env = dict(os.environ, IPHONEOS_DEPLOYMENT_TARGET='17.0', CC=' '.join([clang, *target]),
           CXX=' '.join([clang + '++', *target]), PKG_CONFIG='/usr/bin/false',
           libplist_CFLAGS=f'-I{S}/include', libplist_LIBS=f'-L{S}/lib -lplist-2.0',
           limd_glue_CFLAGS=f'-I{S}/include', limd_glue_LIBS=f'-L{S}/lib -limobiledevice-glue-1.0',
           libimobiledevice_glue_CFLAGS=f'-I{S}/include', libimobiledevice_glue_LIBS=f'-L{S}/lib -limobiledevice-glue-1.0',
           libusbmuxd_CFLAGS=f'-I{S}/include', libusbmuxd_LIBS=f'-L{S}/lib -lusbmuxd-2.0',
           openssl_CFLAGS=f'-I{S}/include', openssl_LIBS=f'-L{S}/lib -lssl -lcrypto')


def run(command, cwd, log):
    with open(OUT / log, 'w') as stream:
        result = subprocess.run([str(c) for c in command], cwd=cwd, env=env, stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        sys.exit('\n'.join((OUT / log).read_text(errors='replace').splitlines()[-20:]) + f'\nfailed: {command} (log {OUT / log})')


def unpack(name):
    """The pinned release tarball, extracted fresh: <name>-<version>/."""
    version, url, sha = SOURCES[name]
    tarball = SRC / Path(url).name
    if not tarball.is_file():
        urllib.request.urlretrieve(url, tarball)
    digest = hashlib.sha256(tarball.read_bytes()).hexdigest()
    if digest != sha:
        sys.exit(f'{tarball.name}: sha256 {digest}, pinned {sha}')
    tree = SRC / f'{name}-{version}'
    if tree.exists():
        shutil.rmtree(tree)
    with tarfile.open(tarball) as archive:
        archive.extractall(SRC, filter='data')
    return tree


def autotools(name, *options):
    tree = unpack(name)
    if name == 'libimobiledevice':
        # its output-format enumerators collide with libplist 2.6.0's enum plist_format_t
        for path in tree.rglob('*'):
            if path.suffix in ('.c', '.h') and path.is_file():
                text = path.read_text(errors='surrogateescape')
                if 'PLIST_FORMAT_' in text:
                    path.write_text(text.replace('PLIST_FORMAT_XML', 'IMD_OUT_XML').replace('PLIST_FORMAT_BINARY', 'IMD_OUT_BIN'),
                                    errors='surrogateescape')
    run(['./configure', '--host=arm-apple-darwin', f'--prefix={S}', '--enable-static', '--disable-shared', *options], tree,
        f'{name}-configure.log')
    run(['make', f'-j{os.cpu_count() or 4}'], tree, f'{name}-make.log')
    run(['make', 'install'], tree, f'{name}-install.log')
    return tree


# The libraries, in dependency order; a stamp per source keeps a rebuild from compiling them again.
trees = {}
for name, options in [('libplist', ['--without-cython', '--without-tests']), ('libimobiledevice-glue', ['--without-cython']),
                      ('libusbmuxd', ['--without-cython']), ('openssl', None),
                      ('libimobiledevice', ['--without-cython', '--with-openssl'])]:
    version = SOURCES[name][0]
    stamp = OUT / f'{name}-{version}.built'
    trees[name] = SRC / f'{name}-{version}'
    if stamp.is_file() and trees[name].is_dir():
        continue
    if name == 'openssl':
        tree = unpack(name)
        env_ossl = dict(env, CROSS_TOP=subprocess.check_output(['xcode-select', '-p'], text=True).strip()
                        + '/Platforms/iPhoneOS.platform/Developer', CROSS_SDK='iPhoneOS.sdk')
        env_ossl.pop('CC'), env_ossl.pop('CXX')
        for command, log in ((['./Configure', 'ios64-cross', 'no-shared', 'no-tests', 'no-engine', 'no-legacy', f'--prefix={S}',
                               '-mios-version-min=17.0'], 'openssl-configure.log'),
                             (['make', f'-j{os.cpu_count() or 4}', 'build_libs'], 'openssl-make.log'),
                             (['make', 'install_dev'], 'openssl-install.log')):
            with open(OUT / log, 'w') as stream:
                if subprocess.run(command, cwd=tree, env=env_ossl, stdout=stream, stderr=subprocess.STDOUT).returncode:
                    sys.exit(f'openssl: {command} failed (log {OUT / log})')
    else:
        autotools(name, *options)
    stamp.write_text(version + '\n')

# One object per tool: its main as <tool>_main, everything else private to the tool. The tools are compiled with hidden
# visibility, so main is a private external that a rename would leave hidden: ld's alias is global, and the export list then
# makes main itself (and every other global of the tool) local.
merged = []
for tool, (source, path) in TOOLS.items():
    obj = trees[source] / path
    if not obj.is_file():
        sys.exit(f'{tool}: the {source} build left no {path}')
    exports, out = OBJ / f'{tool}.exp', OBJ / f'{tool}.tool.o'
    exports.write_text(f'_{tool}_main\n')
    subprocess.run(['ld', '-r', '-alias', '_main', f'_{tool}_main', '-exported_symbols_list', exports, obj, '-o', out],
                   check=True)
    merged.append(out)

dispatcher = OBJ / 'libimobiledevice_main.c'
dispatcher.write_text('#include <libgen.h>\n#include <stdio.h>\n#include <string.h>\n'
                      + ''.join(f'extern int {tool}_main(int, char **);\n' for tool in TOOLS)
                      + 'static const struct { const char *name; int (*fn)(int, char **); } kTools[] = {\n'
                      + ''.join(f'    {{"{tool}", {tool}_main}},\n' for tool in TOOLS) + '''};
static int list(void) {
    for (unsigned i = 0; i < sizeof(kTools) / sizeof(kTools[0]); i++) printf("%s\\n", kTools[i].name);
    return 0;
}
int main(int argc, char **argv) {
    char path[1024];
    strlcpy(path, argc > 0 ? argv[0] : "libimobiledevice", sizeof path);
    const char *name = basename(path);
    if (!strcmp(name, "libimobiledevice")) {
        if (argc < 2 || !strcmp(argv[1], "--list")) return list();
        argv++; argc--;
        strlcpy(path, argv[0], sizeof path);
        name = basename(path);
    }
    for (unsigned i = 0; i < sizeof(kTools) / sizeof(kTools[0]); i++)
        if (!strcmp(name, kTools[i].name)) return kTools[i].fn(argc, argv);
    fprintf(stderr, "libimobiledevice: no tool '%s' (try: libimobiledevice --list)\\n", name);
    return 127;
}
''')
binary = OUT / 'libimobiledevice'
# The tools also take helpers from libimobiledevice's own convenience library (common/, not installed).
internal = trees['libimobiledevice'] / 'common/.libs/libinternalcommon.a'
subprocess.run([clang, *target, '-O2', dispatcher, *merged, internal, *[STAGE / 'lib' / lib for lib in LIBS], '-lpthread',
                '-o', binary], check=True)

entitlements = OUT / 'entitlements.plist'
entitlements.write_bytes(plistlib.dumps({
    'com.apple.private.security.no-container': True,
    'com.apple.security.network.client': True,
    'com.apple.security.network.server': True,
}))
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.droidvm.libimobiledevice',
                '--entitlements', entitlements, binary], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', binary], capture_output=True, text=True, check=True).stderr
assert re.search(r'^Page size=4096$', detail, re.M), 'the signature must use 4 KiB pages'
cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]

extra = TREE / 'build/owner-trust-extra'
extra.mkdir(parents=True, exist_ok=True)
(extra / 'libimobiledevice.json').write_text(json.dumps(
    {'cdhashes': [cdhash], 'category': 3, 'binary': 'build/libimobiledevice/libimobiledevice', 'commands': list(TOOLS),
     'source': ', '.join(f'{name} {version}' for name, (version, _, _) in SOURCES.items()) + ', iphoneos arm64, static'},
    indent=2) + '\n')
print('libimobiledevice CDHash', cdhash, 'size', binary.stat().st_size, 'tools', len(TOOLS))
