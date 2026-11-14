#!/usr/bin/env python3
"""build-droidvm-kexts.py --tree TREE

The board's drivers that ship as kexts of the kernel collection rather than inside the kernel: one ipados27/kexts/NAME.json each,
  name, bundle, version   the kext (bundle: its CFBundleIdentifier)
  sources                 the driver's files in package/ (kits/droidvm in the tree), compiled with DROIDVM_NATIVE_KEXT=1
  attach                  the driver's attach function, or a list of them called in order, which the kext's starter calls
                          once IOResources matches it (the board's late probe calls them when the driver is built in;
                          build-droidvm-legacy-display.py leaves them out of the probe for a driver listed here)
  records                 optional {source: the kernel compile record to compile it with}, for a source that needs another
                          object's flags (a BSD file's, bsd/DEVELOPMENT/kern_exec.o.json); C++ takes
                          iokit/DEVELOPMENT/IOPlatformExpert.cpo.json and C osfmk/DEVELOPMENT/startup.o.json otherwise
  groups                  optional, beside or instead of sources: [{sources, flags, warnings}] -- sources compiled with extra
                          flags ({tree} is the build tree) and, with warnings false, without the board's -Werror set (Apple's
                          IONetworkingFamily, as build-droidvm-devices.py compiles it)
  objects                 optional: objects another step of the kernel build made, tree:PATH (SoftwareAES's NativeAES.o, compiled
                          with its component's own command, and its fileset bindings, by build-droidvm-devices.py)
  object_lists            optional: tree:PATH of a JSON list of such objects (paths in the tree), as a builder writes it
                          (build-droidvm-hid-input.py's kext-objects.json)
A source is a file of package/ (kits/droidvm), or tree:PATH, a path in the build tree (.cache/sources/...).
  builtin                 the build-droidvm-devices.py objects this kext replaces (left out of the kernel)
  libraries               OSBundleLibraries
  targets                 optional: the userspaces it serves, ["ios"] (the default), ["macosx"] or both (droidvm_kexts.py)
  vtables                 optional: {class: the original class it derives from}; fileset-add-kexts.py checks that the
                          original's slots keep their diversity, where the collection names the original (macOS)
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


def command(rec, src, dst, tree, extra=(), warnings=True):
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
    if warnings:
        cmd += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
        if Path(src).suffix != '.c':
            cmd += ['-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                    '-Wno-unnecessary-virtual-specifier', '-Wno-macro-redefined']
    else:
        cmd += ['-Wno-everything']
    cmd += ['-I', str(tree / '.cache/xnu-work/osfmk'), '-I', str(tree / 'kits/droidvm'), *extra]
    run(cmd, cwd=rec['directory'])
    return dst


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--tree', type=Path, required=True)
    p.add_argument('--target', choices=('ios', 'macosx'), help='the userspace of the kernel (default: UNIXNU_XNU_TARGET, else ios)')
    args = p.parse_args()
    tree = args.tree.resolve()
    records = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'

    def record(name):
        return json.loads((records / name).read_text().strip().rstrip(','))

    cpp, c = record('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'), record('osfmk/DEVELOPMENT/startup.o.json')
    out = tree / 'build/droidvm-kexts'
    out.mkdir(parents=True, exist_ok=True)
    manifest = []
    for spec in droidvm_kexts.specs(args.target):
        name = spec['name']
        work = out / name
        work.mkdir(exist_ok=True)
        defines = ('-DDROIDVM_NATIVE_KEXT=1', '-DDROIDVM_DEVICE_DYNAMIC=1')
        objects = []
        groups = [{'sources': spec.get('sources', []), 'flags': [], 'warnings': True}, *spec.get('groups', [])]
        for group in groups:
            flags = [f.replace('{tree}', str(tree)) for f in group.get('flags', [])]
            for source in group['sources']:
                path = tree / source[len('tree:'):] if source.startswith('tree:') else tree / 'kits/droidvm' / source
                named = spec.get('records', {}).get(source)
                rec = record(named) if named else c if path.suffix == '.c' else cpp
                objects.append(command(rec, path, work / (path.stem + '.o'), tree, (*LIB, *defines, *flags),
                                       group.get('warnings', True)))
        listed = []
        for listing in spec.get('object_lists', []):
            if not listing.startswith('tree:'):
                raise SystemExit(f'{name}: object_lists are tree:PATH')
            listed += ['tree:' + p for p in json.loads((tree / listing[len('tree:'):]).read_text())]
        for prebuilt in [*spec.get('objects', []), *listed]:
            if not prebuilt.startswith('tree:'):
                raise SystemExit(f'{name}: objects are tree:PATH')
            path = tree / prebuilt[len('tree:'):]
            if not path.is_file():
                raise SystemExit(f'{name}: {path} was not built')
            objects.append(path)
        starter = f'{name}Starter'
        attach = spec['attach']
        if isinstance(attach, list):
            # several attach functions: one that calls them in order, for the starter to call
            caller = work / f'{name}Attach.c'
            caller.write_text('// Generated by build-droidvm-kexts.py: the attach functions of ' + name + ', in order.\n'
                              + ''.join(f'extern void {f}(void);\n' for f in attach)
                              + f'void droidvm_kext_attach_{name}(void) {{\n' + ''.join(f'    {f}();\n' for f in attach) + '}\n')
            objects.append(command(c, caller, work / f'{name}Attach.o', tree, (*LIB,)))
            attach = f'droidvm_kext_attach_{name}'
        objects.append(command(cpp, tree / 'kits/droidvm/DroidVMKextStarter.cpp', work / f'{starter}.o', tree,
                               (*LIB, f'-DDROIDVM_KEXT_STARTER={starter}', f'-DDROIDVM_KEXT_ATTACH={attach}')))
        manifest.append({
            'name': name, 'bundle': spec['bundle'], 'version': spec['version'],
            'objects': [str(o.relative_to(out)) if o.is_relative_to(out) else str(o) for o in objects],
            'start': '_droidvm_kext_start', 'stop': '_droidvm_kext_stop',
            'libraries': spec['libraries'],
            'vtables': spec.get('vtables', {}),
            # The starter matches IOResources once BSD is up (IOBSD), where the board's late probe would have called the attach.
            'personalities': {name: {'CFBundleIdentifier': spec['bundle'], 'IOClass': starter, 'IOProviderClass': 'IOResources',
                                     'IOResourceMatch': 'IOBSD', 'IOMatchCategory': name}},
            'sha256': {str(o): hashlib.sha256(o.read_bytes()).hexdigest() for o in objects},
        })
        print(f'{spec["bundle"]}: {len(objects)} objects')
    (out / 'manifest.json').write_text(json.dumps({'kexts': manifest}, indent=2) + '\n')


if __name__ == '__main__':
    main()
