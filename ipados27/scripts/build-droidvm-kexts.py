#!/usr/bin/env python3
"""build-droidvm-kexts.py --tree TREE

The board's drivers that ship as kexts of the kernel collection rather than inside the kernel: one ipados27/kexts/NAME.json each,
  name, bundle, version   the kext (bundle: its CFBundleIdentifier)
  sources                 the driver's files in package/ (kits/droidvm in the tree), compiled with DROIDVM_NATIVE_KEXT=1
  attach                  the driver's attach function, which the kext's starter calls once IOResources matches it (the
                          board's late probe calls it when the driver is built in; build-droidvm-legacy-display.py leaves it
                          out of the probe for a driver listed here)
  builtin                 the build-droidvm-devices.py objects this kext replaces (left out of the kernel)
  libraries               OSBundleLibraries
Each is compiled with the kernel's own compile records, as the built-in drivers are (build-droidvm-devices.py), with the starter
package/DroidVMKextStarter.cpp, into TREE/build/droidvm-kexts/NAME/; manifest.json there lists them for fileset-add-kexts.py,
which links them into the collection after the prelink (build-droidvm-all.py, step kernel).

droidvm_kexts.py (beside this script) is what the kernel's own builders read to leave these drivers out.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import droidvm_kexts  # noqa: E402

LIB = ('-DLIBKERN_CONSUMED=', '-DLIBKERN_RETURNS_RETAINED=', '-DLIBKERN_RETURNS_NOT_RETAINED=', '-DLIBKERN_CONSUMES_THIS=')


def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=600, **kw)


def command(rec, src, dst, tree, extra=()):
    """A kernel compile record with another source and output, the board's warnings (as build-droidvm-devices.py's)."""
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
    cmd += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
    if Path(src).suffix != '.c':
        cmd += ['-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier', '-Wno-macro-redefined']
    cmd += ['-I', str(tree / '.cache/xnu-work/osfmk'), '-I', str(tree / 'kits/droidvm'), *extra]
    run(cmd, cwd=rec['directory'])
    return dst


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--tree', type=Path, required=True)
    tree = p.parse_args().tree.resolve()
    records = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'

    def record(name):
        return json.loads((records / name).read_text().strip().rstrip(','))

    cpp, c = record('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'), record('osfmk/DEVELOPMENT/startup.o.json')
    out = tree / 'build/droidvm-kexts'
    out.mkdir(parents=True, exist_ok=True)
    manifest = []
    for spec in droidvm_kexts.specs():
        name = spec['name']
        work = out / name
        work.mkdir(exist_ok=True)
        defines = ('-DDROIDVM_NATIVE_KEXT=1', '-DDROIDVM_DEVICE_DYNAMIC=1')
        objects = []
        for source in spec['sources']:
            path = tree / 'kits/droidvm' / source
            rec = c if path.suffix == '.c' else cpp
            objects.append(command(rec, path, work / (path.stem + '.o'), tree, (*LIB, *defines)))
        starter = f'{name}Starter'
        objects.append(command(cpp, tree / 'kits/droidvm/DroidVMKextStarter.cpp', work / f'{starter}.o', tree,
                               (*LIB, f'-DDROIDVM_KEXT_STARTER={starter}', f'-DDROIDVM_KEXT_ATTACH={spec["attach"]}')))
        manifest.append({
            'name': name, 'bundle': spec['bundle'], 'version': spec['version'],
            'objects': [str(o.relative_to(out)) for o in objects],
            'start': '_droidvm_kext_start', 'stop': '_droidvm_kext_stop',
            'libraries': spec['libraries'],
            # The starter matches IOResources once BSD is up (IOBSD), where the board's late probe would have called the attach.
            'personalities': {name: {'CFBundleIdentifier': spec['bundle'], 'IOClass': starter, 'IOProviderClass': 'IOResources',
                                     'IOResourceMatch': 'IOBSD', 'IOMatchCategory': name}},
            'sha256': {str(o.relative_to(out)): hashlib.sha256(o.read_bytes()).hexdigest() for o in objects},
        })
        print(f'{spec["bundle"]}: {len(objects)} objects')
    (out / 'manifest.json').write_text(json.dumps({'kexts': manifest}, indent=2) + '\n')


if __name__ == '__main__':
    main()
