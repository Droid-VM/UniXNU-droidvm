#!/usr/bin/env python3
"""Build Apple's lldb debugserver (the remote debug stub) from llvm-project, cross-compiled for iphoneos arm64, so the rig can
remote-debug guest processes over gdb-remote (lldb `platform connect` / `process connect`).

  build-droidvm-debugserver.py TREE_ROOT

debugserver is self-contained: it links no llvm/clang libraries. Its only lldb dependency is lldb_private::SocketAddress
(lldb/Host/SocketAddress.h + .cpp, which RNBSocket.cpp uses), vendored alongside the tool. The source is fetched by a sparse,
shallow git checkout pinned to a release tag (reproducible). Two SDK gaps are shimmed the way toybox/zsh shim <sys/disk.h>:

  * mig (the Mach Interface Generator) is run against the *macOS* SDK -- the iphoneos SDK ships no .defs, so
    `#import <mach/mach_exc.defs>` can't resolve there; the generated C is SDK-neutral and compiled for iphoneos afterward.
  * headers the iphoneos SDK omits (libproc.h and its BSD-networking transitive deps) are copied from the macOS SDK into a
    shim include dir; mach/mach_vm.h -- which iphoneos ships only as `#error ... unsupported` -- is pre-seeded the same way
    (the mach_vm_* routines themselves exist in the iphoneos libSystem dylibs, so the macOS declarations link cleanly). The
    shim copy is self-healing: any further `'X' file not found` that the macOS SDK provides is copied in and the file retried.

Built WITHOUT WITH_LOCKDOWN/WITH_FBS/WITH_BKS (the upstream "non-UI" variant), so it needs no iOS private frameworks -- only
Foundation, CoreFoundation, Security and libcompression. The two parallel entry points debugserver.cpp and libdebugserver.cpp
share global state, so the common objects are archived (like CMake's lldbDebugserverCommon) and the standalone main linked
against the archive, leaving the unused libdebugserver.cpp object unpulled. Ad-hoc 4 KiB-signed with the curated debug
entitlement set (package/debug-entitlements.plist -- com.apple.private.cs.debugger, thread-set-state,
system-task-ports*, set-exception-port, network server/client, ...) and trusted through the owner module
(build/owner-trust-extra/debugserver.json, category 3) like lsdiag/applebox. Staged with
  export.py put build/debugserver/debugserver /private/var/ngpu/bin/debugserver 0755
and run as root; point lldb at it with `process connect connect://[<guest-ip>]:<port>`.

Needs the iphoneos + macOS SDKs, git, mig and codesign (the build Mac)."""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

TAG = 'llvmorg-19.1.7'
REPO = 'https://github.com/llvm/llvm-project.git'
_v = re.match(r'llvmorg-(\d+)\.(\d+)\.(\d+)', TAG)
if not _v:
    sys.exit(f'cannot parse a version out of TAG {TAG!r}')
VMAJ, VMIN, VPATCH = _v.groups()

# sparse paths: the debugserver tool, plus the one lldb dependency (SocketAddress + the two headers it pulls on non-Windows).
SPARSE = ['/lldb/tools/debugserver/',
          '/lldb/include/lldb/Host/SocketAddress.h',
          '/lldb/include/lldb/Host/PosixApi.h',
          '/lldb/source/Host/common/SocketAddress.cpp']

FORK = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else sys.exit(__doc__)
HERE = Path(__file__).resolve().parents[1]
PLIST = HERE / 'package/debug-entitlements.plist'
if not PLIST.is_file():
    sys.exit(f'debug entitlements plist not found: {PLIST}')
OUT = FORK / 'build/debugserver'
WORK = OUT / 'work'
LP = WORK / 'llvm-project'
OBJ = WORK / 'obj'
MIGOUT = WORK / 'migout'
SHIM = WORK / 'shim'
WORK.mkdir(parents=True, exist_ok=True)
for d in (OBJ, MIGOUT, SHIM):          # regenerated every run (keep LP to avoid re-fetching)
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True, exist_ok=True)

ios_sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
mac_sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()

# ---- fetch: sparse, shallow, pinned to the release tag ----
if not (LP / '.git').is_dir():
    subprocess.run(['git', 'clone', '--filter=blob:none', '--no-checkout', '--depth', '1',
                    '--branch', TAG, REPO, str(LP)], check=True)
subprocess.run(['git', '-C', str(LP), 'fetch', '--depth', '1', 'origin', 'tag', TAG], check=True)
subprocess.run(['git', '-C', str(LP), 'sparse-checkout', 'set', '--no-cone', *SPARSE], check=True)
subprocess.run(['git', '-C', str(LP), 'checkout', '-q', TAG], check=True)

SRC = LP / 'lldb/tools/debugserver/source'
LLDB_INC = LP / 'lldb/include'
SOCK = LP / 'lldb/source/Host/common/SocketAddress.cpp'

# ---- MIG: mach exception server/user glue (macOS SDK has the .defs; output is SDK-neutral) ----
subprocess.run(['xcrun', 'mig', '-arch', 'arm64', '-isysroot', mac_sdk, str(SRC / 'MacOSX/dbgnub-mig.defs')],
               cwd=MIGOUT, check=True)
mig_c = [MIGOUT / 'mach_excUser.c', MIGOUT / 'mach_excServer.c']
for f in mig_c:
    if not f.is_file():
        sys.exit(f'mig did not produce {f.name}')

# ---- version symbols (what debugserver_vers.c.in expands to at this tag) ----
vers = MIGOUT / 'debugserver_vers.c'
vers.write_text(
    f'const unsigned char debugserverVersionString[] __attribute__ ((used)) = '
    f'"@(#)PROGRAM:LLDB  PROJECT:lldb-{VMAJ}.{VMIN}.{VPATCH}" "\\n";\n'
    f'const double debugserverVersionNumber __attribute__ ((used)) = (double){VMAJ}.{VMIN};\n')

# ---- shim: pre-seed mach/mach_vm.h (iphoneos ships it only as "#error ... unsupported", so the
# self-heal loop below -- which only catches "file not found" -- can't reach it). ----
(SHIM / 'mach').mkdir(parents=True, exist_ok=True)
shutil.copyfile(Path(mac_sdk) / 'usr/include/mach/mach_vm.h', SHIM / 'mach/mach_vm.h')
shimmed = {'mach/mach_vm.h'}

CXX = ['xcrun', 'clang++', '-target', 'arm64-apple-ios17.0', '-isysroot', ios_sdk,
       '-std=c++17', '-fno-objc-arc', '-DLLDB_USE_OS_LOG']
CC = ['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', ios_sdk, '-DLLDB_USE_OS_LOG']
INCS = ['-I' + str(SRC), '-I' + str(SRC / 'MacOSX'), '-I' + str(SRC / 'MacOSX/arm64'),
        '-I' + str(MIGOUT), '-I' + str(SHIM), '-I' + str(LLDB_INC)]


def compile_one(comp, src, obj):
    for _ in range(40):
        r = subprocess.run(comp + INCS + ['-c', str(src), '-o', str(obj)], capture_output=True, text=True)
        if r.returncode == 0:
            return
        m = re.search(r"fatal error: '([^']+)' file not found", r.stderr)
        cand = Path(mac_sdk) / 'usr/include' / m.group(1) if m else None
        if cand and cand.is_file():               # self-heal from the macOS SDK and retry
            dst = SHIM / m.group(1)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(cand, dst)
            shimmed.add(m.group(1))
            continue
        sys.exit(f'compile failed: {src}\n{r.stderr}')
    sys.exit(f'too many shim iterations compiling {src}')


# ((source, compiler) relative to SRC unless absolute). debugserver.cpp and libdebugserver.cpp are BOTH compiled (per the
# tool's source set); the archive link below keeps only the one that main pulls, avoiding their shared-global collisions.
CPP = ['DNB.cpp', 'DNBArch.cpp', 'DNBBreakpoint.cpp', 'DNBDataRef.cpp', 'DNBError.cpp', 'DNBLog.cpp', 'DNBRegisterInfo.cpp',
       'DNBThreadResumeActions.cpp', 'JSON.cpp', 'PThreadEvent.cpp', 'PseudoTerminal.cpp', 'RNBContext.cpp', 'RNBRemote.cpp',
       'RNBServices.cpp', 'RNBSocket.cpp', 'StdStringExtractor.cpp', 'StringConvert.cpp', 'SysSignal.cpp', 'TTYState.cpp',
       'debugserver.cpp', 'libdebugserver.cpp',
       'MacOSX/CFBundle.cpp', 'MacOSX/CFString.cpp', 'MacOSX/Genealogy.cpp', 'MacOSX/MachException.cpp',
       'MacOSX/MachThread.cpp', 'MacOSX/MachThreadList.cpp', 'MacOSX/MachVMMemory.cpp', 'MacOSX/MachVMRegion.cpp',
       'MacOSX/OsLogger.cpp', 'MacOSX/arm64/DNBArchImplARM64.cpp']
MM = ['MacOSX/MachProcess.mm', 'MacOSX/MachTask.mm']

for rel in CPP:
    compile_one(CXX, SRC / rel, OBJ / (rel.replace('/', '_') + '.o'))
for rel in MM:
    compile_one(CXX, SRC / rel, OBJ / (rel.replace('/', '_') + '.o'))
compile_one(CXX, SOCK, OBJ / 'SocketAddress.o')
for f in mig_c + [vers]:
    compile_one(CC, f, OBJ / (f.name + '.o'))

print('shimmed from the macOS SDK:', ', '.join(sorted(shimmed)))

# ---- link: archive the common objects (everything but the standalone main) like lldbDebugserverCommon, then link main ----
main_o = OBJ / 'debugserver.cpp.o'
common = sorted(o for o in OBJ.glob('*.o') if o != main_o)
lib = OBJ / 'libdebugserverCommon.a'
lib.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(lib), *map(str, common)], check=True)
binary = OUT / 'debugserver'
binary.unlink(missing_ok=True)
subprocess.run(['xcrun', 'clang++', '-target', 'arm64-apple-ios17.0', '-isysroot', ios_sdk,
                str(main_o), str(lib), '-o', str(binary),
                '-framework', 'Foundation', '-framework', 'CoreFoundation', '-framework', 'Security',
                '-lcompression'], check=True)

# ---- sign ad-hoc, 4 KiB pages, with the curated debug entitlement set ----
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.droidvm.debugserver',
                '--entitlements', str(PLIST), str(binary)], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', str(binary)], capture_output=True, text=True, check=True).stderr
assert re.search(r'^Page size=4096$', detail, re.M), 'the signature must use 4 KiB pages'
cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]

extra = FORK / 'build/owner-trust-extra'
extra.mkdir(parents=True, exist_ok=True)
(extra / 'debugserver.json').write_text(json.dumps(
    {'cdhashes': [cdhash], 'category': 3, 'binary': 'build/debugserver/debugserver',
     'source': f'llvm-project {TAG} lldb/tools/debugserver, iphoneos arm64'}, indent=2) + '\n')
print('debugserver CDHash', cdhash, 'size', binary.stat().st_size)
