#!/usr/bin/env python3
"""The iPadOS 27 board's network, sound, battery and media devices, for the display archive (build-droidvm-legacy-display.py), as the
iPadOS 17 board's scripts/build-native-legacy-display.py builds them:

  - the network: Apple's IONetworkingFamily compiled into the kernel with its static initializers staged for a late call (the
    24A446 kernelcache carries the family, but the fileset keeps only its roots, and the family is not one), the virtio-net
    controller on it (DroidVMVirtioEthernet.cpp) and the late registration that starts IONetworkStack (DroidVMVirtioNetIf.cpp),
    over the virtio-mmio transport (DroidVMVirtioNet.c). IONetworkingFamily-188, the release of the xnu-12377 era (iPadOS 17's is 177),
    has the same twelve sources.
  - the speaker and microphone (DroidVMAudio.cpp, linked under the original IOAudio2Family's classes at attach) over virtio-snd
    (DroidVMVirtioSnd.c), the battery (DroidVMBattery.cpp, an IOPMPowerSource over crosvm's goldfish battery), the motion sensors'
    command channel CoreMotion opens as the sensor processor's (DroidVMSensorProcessor.cpp), the virtio-media
    devices (DroidVMVirtioMedia.c), the virtio-console ports as BSD terminals (DroidVMVirtioConsole.c, /dev/cu.NAME, compiled with a
    BSD file's flags for the tty and devfs interfaces), VideoToolbox's paravirtualization channel (DroidVMVTParavirt.cpp, Apple's names), the vCPUs' performance requests to the host (DroidVMCpufreq.c, crosvm's virtual cpufreq) and
    the scheduler trace the host reads from guest RAM (DroidVMSchedTrace.c, droidvm-sched-trace.patch).

The transports take their rings and buffers from a region allocated when the device starts (DROIDVM_DEVICE_DYNAMIC,
DroidVMDmaRegion.h): their static arrays (over 400 KiB) do not fit the native __DATA's slot in the fileset.

Writes build/droidvm-devices/objects.json and sources.json (paths relative to the tree).
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from kernel_target import ARCH, IOS_VERSION, TRIPLE
if ARCH != 'arm64e' or IOS_VERSION.split('.')[0] != '27':
    raise SystemExit('This device builder only supports arm64e iPadOS 27 (24A446).')
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/droidvm-devices'
OUT.mkdir(parents=True, exist_ok=True)
RECORDS = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
IONET_TAG, IONET_COMMIT = 'IONetworkingFamily-188', 'bb910e2b458939f6b1f53c0fa6907f258b431ea4'
IONET_REPOSITORY = 'https://github.com/apple-oss-distributions/IONetworkingFamily.git'
IONET_FILES = ['IOEthernetController', 'IOEthernetInterface', 'IOKernelDebugger', 'IOMbufMemoryCursor', 'IONetworkController',
               'IONetworkData', 'IONetworkInterface', 'IONetworkMedium', 'IONetworkStack', 'IONetworkUserClient', 'IOOutputQueue',
               'IOPacketQueue']


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=600, **kw)


def require(v, why):
    if not v:
        raise SystemExit(why)


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def record(name):
    return json.loads((RECORDS / name).read_text().strip().rstrip(','))


def command(rec, src, dst, extra=(), warnings=True):
    """A kernel compile record with another source and output (its warning options replaced by the board's, or by none)."""
    cmd, args, i = [], rec['arguments'], 0
    while i < len(args):
        value = args[i]
        if value == rec['file']:
            cmd.append(str(src))
        elif value in ('-o', '-MF', '-MT', '-MQ', '-MJ'):
            if value == '-o':
                cmd += ['-o', str(dst)]
            i += 1
        elif value in ('-MD', '-MMD', '-MP'):
            pass
        elif value == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W'):
            cmd.append(value)
        i += 1
    if warnings:
        cmd += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
        if Path(src).suffix != '.c':
            cmd += ['-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                    '-Wno-unnecessary-virtual-specifier', '-Wno-macro-redefined']
    else:
        cmd += ['-Wno-everything']
    cmd += ['-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(ROOT / 'kits/droidvm'), *extra]
    run(cmd, cwd=rec['directory'])
    return dst


LLVM = Path(run(['brew', '--prefix', 'llvm'], text=True).strip()) / 'bin'


def stage_late(obj, stem):
    """The object with its static initializer out of __mod_init_func and exported, for the board's late registration."""
    late = obj.with_suffix('.late.o')
    run([LLVM / 'llvm-objcopy', '--remove-section=__DATA,__mod_init_func', f'--globalize-symbol=__GLOBAL__sub_I_{stem}.cpp', obj, late])
    return late


def ionetworking():
    source = ROOT / '.cache/sources/IONetworkingFamily'
    if not (source / '.git').is_dir():
        source.parent.mkdir(parents=True, exist_ok=True)
        run(['git', 'clone', '-q', '--depth', '1', '--branch', IONET_TAG, IONET_REPOSITORY, source])
    require(run(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip() == IONET_COMMIT, f'{IONET_TAG} must be {IONET_COMMIT}')
    require(sorted(p.stem for p in source.glob('*.cpp')) == sorted(IONET_FILES), f'{IONET_TAG}: the twelve family sources')
    # The sources include their headers as <IOKit/network/...>; IONetworkInterface.cpp also wants the SDK's TargetConditionals.h.
    include = OUT / 'ionetworking-include'
    (include / 'IOKit/network').mkdir(parents=True, exist_ok=True)
    sdk = Path(run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip())
    for link, target in [*((include / 'IOKit/network' / h.name, h) for h in source.glob('*.h')),
                         (include / 'TargetConditionals.h', sdk / 'usr/include/TargetConditionals.h')]:
        link.unlink(missing_ok=True)
        link.symlink_to(target)
    # xnu-12377's sys/mbuf.h has inline helpers (m_add_hdr_crumb_interface_input/output) that pass an mbuf_t, the opaque
    # struct __mbuf * of the KPI the family sees, to functions taking struct mbuf *; the family never calls them. Overloads taking
    # mbuf_t, declared first, let them compile; nothing references them.
    (include / 'droidvm-mbuf-crumbs.h').write_text(
        '// Generated by build-droidvm-devices.py: see there.\n#pragma once\n#include <stdint.h>\n#include <sys/kpi_mbuf.h>\n'
        'void m_add_hdr_crumb(mbuf_t, uint64_t, uint64_t);\nvoid m_add_hdr_crumb_chain(mbuf_t, uint64_t, uint64_t);\n')
    return source, include


def main():
    cpp, c_mach = record('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'), record('osfmk/DEVELOPMENT/startup.o.json')
    family, include = ionetworking()
    # Every compile here includes sys/systm.h first: the family includes it inside extern "C", where in this private-header
    # universe its zalloc/startup templates cannot be declared. The bsd source tree, last on the path, supplies private headers
    # the export set leaves out (net/classq). The family's classes and the ones built on it see its headers alike
    # (__PRIVATE_SPI__ changes IONetworkController's and IONetworkInterface's layout).
    lib = ('-DLIBKERN_CONSUMED=', '-DLIBKERN_RETURNS_RETAINED=', '-DLIBKERN_RETURNS_NOT_RETAINED=', '-DLIBKERN_CONSUMES_THIS=')
    universe = ('-include', 'sys/systm.h', '-include', str(include / 'droidvm-mbuf-crumbs.h'), '-I', str(ROOT / '.cache/xnu-work/bsd'),
                '-D__PRIVATE_SPI__')
    objects, sources = [], []
    for stem in IONET_FILES:
        obj = command(cpp, family / f'{stem}.cpp', OUT / f'{stem}.o', (*lib, *universe, '-I', str(include), '-I', str(family)),
                      warnings=False)
        objects.append(stage_late(obj, stem))
        sources.append(family / f'{stem}.cpp')
    strict = (*lib, *universe, '-isystem', str(include))
    for name, late in (('DroidVMVirtioEthernet', True), ('DroidVMVirtioNetIf', False), ('DroidVMAudio', True),
                       ('DroidVMBattery', True), ('DroidVMSensorProcessor', True), ('DroidVMSoftwareAES', False),
                       ('DroidVMVTParavirt', True), ('DroidVMVTHost', False)):
        source = ROOT / 'kits/droidvm' / f'{name}.cpp'
        obj = command(cpp, source, OUT / f'{name}.o', strict)
        objects.append(stage_late(obj, name) if late else obj)
        sources.append(source)
    for name in ('DroidVMVirtioNet', 'DroidVMVirtioSnd', 'DroidVMVirtioMedia', 'DroidVMCpufreq', 'DroidVMSchedTrace'):
        source = ROOT / 'kits/droidvm' / f'{name}.c'
        objects.append(command(c_mach, source, OUT / f'{name}.o', ('-DDROIDVM_DEVICE_DYNAMIC=1',)))
        sources.append(source)
    console_source = ROOT / 'kits/droidvm/DroidVMVirtioConsole.c'
    objects.append(command(record('bsd/DEVELOPMENT/kern_exec.o.json'), console_source, OUT / 'DroidVMVirtioConsole.o',
                           ('-DDROIDVM_DEVICE_DYNAMIC=1',)))
    sources.append(console_source)
    sources += [ROOT / 'kits/droidvm/DroidVMDmaRegion.h', ROOT / 'kits/droidvm/DroidVMWifi.inc', ROOT / 'kits/droidvm/DroidVMVTWire.h']
    # SoftwareAES's NativeAES, from a staged copy compiled with component:SoftwareAES's own command: the hardware keys the guest is
    # asked for derived in software (SoftwareAESKeys.inc), its static initializer staged for DroidVMSoftwareAES.cpp's registration,
    # its parent bound to the original IOAESAccelerator of the 24A446 fileset.
    aes_dir = ROOT / 'build/software-aes'
    aes_original = ROOT / 'drivers/SoftwareAES/NativeAES.cpp'
    aes_keys = ROOT / 'kits/droidvm/SoftwareAESKeys.inc'
    aes_text = aes_original.read_text()
    for anchor, replacement in (
            ('#include <kern/task.h>\n', '#include <kern/task.h>\n' + aes_keys.read_text()),
            ('  bool isKeySupported(unsigned selector) override { return selector == 0; }\n',
             '  bool isKeySupported(unsigned selector) override { return selector == 0 || droidvmHardwareKey(selector); }\n'),
            ('  if (key->selector || (operation != IOAESEncrypt && operation != IOAESDecrypt)) {\n',
             '  IOAESKeyData droidvmKey;\n  key = droidvmKeyFor(key, droidvmKey);\n'
             '  if (key->selector || (operation != IOAESEncrypt && operation != IOAESDecrypt)) {\n')):
        require(aes_text.count(anchor) == 1, 'NativeAES.cpp no longer has exactly one ' + anchor.strip())
        aes_text = aes_text.replace(anchor, replacement)
    # the same file name: the late static initializer is named after it (__GLOBAL__sub_I_NativeAES.cpp)
    aes_staged = aes_dir / 'droidvm' / 'NativeAES.cpp'
    aes_staged.parent.mkdir(exist_ok=True)
    aes_staged.write_text(aes_text)
    aes_command = json.loads((aes_dir / 'compile-command.json').read_text())
    require(aes_command.count(str(aes_original)) == 1 and aes_command.count(str(aes_dir / 'NativeAES.o')) == 1,
            'component:SoftwareAES compiles drivers/SoftwareAES/NativeAES.cpp to build/software-aes/NativeAES.o once')
    aes_obj = aes_staged.with_suffix('.o')
    run([str(aes_staged) if a == str(aes_original) else str(aes_obj) if a == str(aes_dir / 'NativeAES.o') else a for a in aes_command]
        + ['-I', str(ROOT / 'drivers/SoftwareAES')], cwd=cpp['directory'])
    objects.append(stage_late(aes_obj, 'NativeAES'))
    aes_bindings = ROOT / 'kits/droidvm/SoftwareAES-24A446.S'
    run(['xcrun', 'clang', '-target', TRIPLE, '-Werror', '-Wno-incompatible-sysroot', '-c', aes_bindings, '-o', OUT / 'SoftwareAES-24A446.o'])
    objects.append(OUT / 'SoftwareAES-24A446.o')
    sources += [aes_original, ROOT / 'drivers/SoftwareAES/OriginalAESABI.h', aes_keys, aes_bindings]
    rel = lambda p: str(p.relative_to(ROOT))  # noqa: E731
    (OUT / 'objects.json').write_text(json.dumps([rel(p) for p in objects], indent=2) + '\n')
    (OUT / 'sources.json').write_text(json.dumps([rel(p) for p in sources], indent=2) + '\n')
    (OUT / 'report.json').write_text(json.dumps(dict(
        ionetworking=dict(tag=IONET_TAG, commit=IONET_COMMIT), objects={rel(p): sha(p) for p in objects}), indent=2) + '\n')
    print(f'devices: {len(objects)} objects ({IONET_TAG}, ethernet, sound, battery, media, console, cpufreq, sched trace)')


if __name__ == '__main__':
    main()
