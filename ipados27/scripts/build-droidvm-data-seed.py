#!/usr/bin/env python3
"""The Data seed of the iPadOS 27 board: 24A446's own /private/var in a fresh APFS volume, Setup skipped, packed for DarwinBoot.

  build-droidvm-data-seed.py --tree TREE --profile PROFILE --ipsw IPSW_DIR [--gib 2] [--plugin DIR [--clients NAMES] [--store-gib 12]
                             [--tools TREE]]

As the iPadOS 17 build makes its seed (scripts/build-droidvm-all.py data-base): the IPSW's OS image is attached read-only, the
fork's prepare-owner-data-fixture.py copies its /private/var into a new 32 MiB APFS volume (whose volume UUID the kernel mounts
Data by: CONFIG_XNU_OWNER_DATA_VOLUME_UUID), DarwinBoot's packed_ramdisk packs it, seed-data-grow.py makes the container a volatile
GIB GiB one, seed-data-skip-setup.py marks Setup done for 24A446, and seed-data-prefs.py adds PREFS. With --plugin, the fork's
add-gpu-plugin-data.py then stages the Metal plugin (the build directory of build-adreno-metal-plugin.py --target ios27) for the
render server and the GPU clients, and seed-data-grow.py makes the container STORE_GIB GiB for the persistent Data store
(droidvm-data-persist=2): data-<STORE_GIB>g-27-gpu.dbr. Its CDHashes are trusted by build-droidvm-trust.py --owner <DIR>/trust.json.
A new seed needs a new store (tools/rig/make-data-store.sh): the store holds the changes of the seed it ran on.

With --plugin, the App cryptex (the IPSW's Cryptex1,AppOS: Safari's daemons and settings) goes in first (add-app-cryptex-data.py,
shown over /private/preboot with owner-app-cryptex=1; its own trust module is among the original ones), and with --tools TREE the
debug shell (ownervisor, dropbear, applebox, ...) and the board's tools as TREE's build/ holds them go in last
(add-ownervisor-data.py, started with owner-service-manager=1); their CDHashes are TREE's build/owner-trust-extra/*.json, for
build-droidvm-trust.py --owner. iPadOS 27 kills arm64 platform binaries at exec, so they are built for arm64e into this board's
tree first: DROIDVM_IOS_TARGET=arm64e-apple-ios27.0 OWNERVISOR=<ownervisor c6c8b2a or later> APPLEBOX=<applebox 6e3b471 or later>
python3 scripts/build-droidvm-{ownervisor,toybox,gfxprobe,lsdiag,ipatool}.py TREE (gfxprobe takes NativeAdreno's headers from the
tree: link-droidvm-gpu-sources.py first).

The OS image is the decrypted IPSW member whose digest the OS profile pins ([root] os_image_sha256, as recorded by the IPSW cache
next to it). macOS refuses an image attachment on the external DATA volume to a process started over ssh, so the image, its mount
and the scripts' working images live in a scratch directory on the system volume; the scripts run from a copy of the tree's there.
Writes TREE/build/droidvm-data-<build>/: data-32.dbr, data-<GIB>g.dbr, data-<GIB>g-skip.dbr and seed.json (volume UUID, launchd).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import tomllib

SCRATCH = Path('/tmp') / f'droidvm27-seed-{os.environ.get("USER", "build")}'
# The preferences the seed carries (seed-data-prefs.py USER:DOMAIN:KEY=VALUE).
PREFS = ['mobile:com.apple.coreanimation:CAEnableMedina=false', 'root:com.apple.coreanimation:CAEnableMedina=false']
# The processes besides the render server that get a native Metal device (the board's droidvm-gpu-clients), as on iPadOS 17.
GPU_CLIENTS = 'WidgetRenderer_Default,PosterBoard,iconservicesagent'


def run(command, **kwargs):
    print('+', ' '.join(str(part) for part in command), flush=True)
    subprocess.run([str(part) for part in command], check=True, **kwargs)


def os_image(ipsw, pin):
    for record in sorted(ipsw.glob('*.dmg.json')):
        info = json.loads(record.read_text())
        if info.get('sha256') == pin:
            image = record.with_name(record.name[:-len('.json')])
            if image.is_file() and image.stat().st_size == info['size']:
                return image
    raise SystemExit(f'{ipsw}: no OS image with the profile\'s digest {pin}')


def attach(image, mount):
    """The image's read-only attachment at mount (made once); returns its mount point."""
    for item in plistlib.loads(subprocess.check_output(['hdiutil', 'info', '-plist']))['images']:
        if Path(item['image-path']).resolve() == image.resolve():
            if any(e.get('mount-point') == str(mount) for e in item['system-entities']):
                return mount
            run(['hdiutil', 'detach', item['system-entities'][0]['dev-entry']])
    mount.mkdir(parents=True, exist_ok=True)
    run(['hdiutil', 'attach', '-readonly', '-nobrowse', '-owners', 'off', '-mountpoint', mount, image], stdout=subprocess.DEVNULL)
    return mount


def seed_tree(tree, packer):
    """A copy of the tree's scripts (and DarwinBoot's packer, which this board's tree does not hold: DarwinBoot is not one of its
    components) on the system volume."""
    root = SCRATCH / 'seedtree'
    for source, directory in ((tree / 'scripts', 'scripts'), (packer, 'drivers/DarwinBoot/Test')):
        if (root / directory).exists():
            shutil.rmtree(root / directory)
        shutil.copytree(source, root / directory, ignore=shutil.ignore_patterns('__pycache__'))
    (root / 'build').mkdir(parents=True, exist_ok=True)
    cache = root / '.cache/firmware/target-volumes'
    cache.mkdir(parents=True, exist_ok=True)
    return root


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, required=True, help='the board\'s build tree (out/.../tree)')
    parser.add_argument('--profile', type=Path, required=True, help='the OS profile (profiles/os/<name>/profile.toml)')
    parser.add_argument('--ipsw', type=Path, required=True, help='the IPSW cache of the build (out/ipsw/<build>)')
    parser.add_argument('--gib', type=int, default=2, help='the Data container size (volatile; default 2)')
    parser.add_argument('--packer', type=Path, default=None,
                        help='DarwinBoot\'s Test directory (packed_ramdisk.py); default: the UniXNU checkout\'s above the tree')
    parser.add_argument('--plugin', type=Path, default=None,
                        help='the Metal plugin build (TREE/build/adreno-metal-plugin27) to stage, for the persistent Data store')
    parser.add_argument('--clients', default=GPU_CLIENTS, help=f'with --plugin: the GPU clients (default {GPU_CLIENTS})')
    parser.add_argument('--store-gib', type=int, default=12, help='with --plugin: the container size for the store (default 12)')
    parser.add_argument('--tools', type=Path, default=None,
                        help="with --plugin: a tree whose build/ holds the debug shell and the board's tools (the iPadOS 17 board's)")
    args = parser.parse_args()
    tree = args.tree.resolve()
    packer = args.packer or next((base / 'drivers/DarwinBoot/Test' for base in tree.parents
                                  if (base / 'drivers/DarwinBoot/Test/packed_ramdisk.py').is_file()), None)
    if packer is None:
        raise SystemExit('no drivers/DarwinBoot/Test/packed_ramdisk.py above the tree; pass --packer')
    profile = tomllib.loads(args.profile.read_text())
    build_id = profile['ipsw']['build']
    out = tree / f'build/droidvm-data-{build_id}'
    out.mkdir(parents=True, exist_ok=True)
    SCRATCH.mkdir(parents=True, exist_ok=True)

    source = os_image(args.ipsw.resolve(), profile['root']['os_image_sha256'])
    image = SCRATCH / source.name
    if not image.is_file() or image.stat().st_size != source.stat().st_size:
        run(['cp', source, image])
    mount = attach(image, SCRATCH / 'os-mount')
    root = seed_tree(tree, packer)
    link = root / '.cache/firmware/target-volumes/os-mount'
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(mount)
    launchd = hashlib.sha256((mount / 'sbin/launchd').read_bytes()).hexdigest()

    fixture = SCRATCH / 'owner-data-fixture'
    if fixture.exists():
        shutil.rmtree(fixture)
    run(['python3', root / 'scripts/prepare-owner-data-fixture.py', '--output', fixture, '--launchd-sha256', launchd], cwd=root)
    seed = json.loads((fixture / 'seed.json').read_text())
    work = SCRATCH / 'data'
    if work.exists():
        shutil.rmtree(work)
    work.mkdir()
    packer = root / 'drivers/DarwinBoot/Test'
    run(['python3', '-c', 'import sys; from pathlib import Path; sys.path.insert(0, sys.argv[1]); import packed_ramdisk; '
         'print(packed_ramdisk.pack(Path(sys.argv[2]), Path(sys.argv[3])))', packer, fixture / 'owner-data-32.cdr',
         work / 'data-32.dbr'], cwd=root)
    grown = work / f'data-{args.gib}g.dbr'
    run(['python3', root / 'scripts/seed-data-grow.py', '--volatile', '--gib', args.gib, work / 'data-32.dbr', grown], cwd=root)
    skipped = work / f'data-{args.gib}g-skip.dbr'
    run(['python3', root / 'scripts/seed-data-skip-setup.py', grown, skipped, f'--build={build_id}'], cwd=root)
    # CAEnableMedina false: the J820 is a device QuartzCore draws secure (camera, microphone) indicators for through the display
    # pipe (CADeviceSupportsMedina), and SpringBoard's indicator layer then needs a valid indicator name; this guest's has none and
    # every commit raised CASecureIndicatorLayerInvalidName ("Invalid indicator name (null)"), aborting SpringBoard.
    final = work / f'data-{args.gib}g-27.dbr'
    run(['python3', root / 'scripts/seed-data-prefs.py', skipped, final, *PREFS], cwd=root)
    plugin = None
    if args.plugin:
        # The App cryptex, from the IPSW cache, copied to the system volume as the OS image is.
        record = next(json.loads(p.read_text()) for p in args.ipsw.resolve().glob('*.dmg.json')
                      if json.loads(p.read_text()).get('component') == 'Cryptex1,AppOS')
        cryptex = SCRATCH / record['member']
        if not cryptex.is_file() or cryptex.stat().st_size != record['size']:
            run(['cp', args.ipsw.resolve() / record['member'], cryptex])
        with_app = work / f'data-{args.gib}g-27-app.dbr'
        run(['python3', root / 'scripts/add-app-cryptex-data.py', final, cryptex, with_app], cwd=root)
        # The plugin is copied to the system volume as well: the staging script attaches the seed there.
        plugin = SCRATCH / 'plugin27'
        if plugin.exists():
            shutil.rmtree(plugin)
        shutil.copytree(args.plugin.resolve(), plugin)
        staged = work / f'data-{args.gib}g-27-gpu.dbr'
        run(['python3', root / 'scripts/add-gpu-plugin-data.py', '--publish', '--plugin', plugin, '--clients', args.clients, with_app,
             staged], cwd=root)
        if args.tools:
            # The debug shell and the tools, where add-ownervisor-data.py looks for them (the seed tree's build/).
            for name in ('debug-shell', 'toybox', 'gfxprobe', 'ipatool', 'lsdiag'):
                source, target = args.tools.resolve() / 'build' / name, root / 'build' / name
                if target.exists():
                    shutil.rmtree(target)
                if source.is_dir():
                    shutil.copytree(source, target, symlinks=True)
            with_tools = work / f'data-{args.gib}g-27-ov.dbr'
            run(['python3', root / 'scripts/add-ownervisor-data.py', staged, with_tools], cwd=root)
            staged = with_tools
        final = work / f'data-{args.store_gib}g-27-gpu.dbr'
        run(['python3', root / 'scripts/seed-data-grow.py', '--gib', args.store_gib, staged, final], cwd=root)
    for path in sorted(work.iterdir()):
        shutil.copyfile(path, out / path.name)
    record = dict(build=build_id, volume_uuid=seed['volume_uuid'], launchd_sha256=launchd, os_image=source.name,
                  seed=final.name, seed_sha256=hashlib.sha256((out / final.name).read_bytes()).hexdigest(), preferences=PREFS,
                  plugin=json.loads((plugin / 'trust.json').read_text()) if plugin else None,
                  gpu_clients=args.clients if plugin else None)
    (out / 'seed.json').write_text(json.dumps(record, indent=2) + '\n')
    print(f'Data seed {out / final.name}: volume UUID {seed["volume_uuid"]}')
    # The kernel mounts Data by its volume UUID, which every fixture is made with anew: the board's configuration must name it.
    defconfig = Path(__file__).resolve().parents[1] / 'configs/droidvm_ipados27_defconfig'
    if f'CONFIG_XNU_OWNER_DATA_VOLUME_UUID="{seed["volume_uuid"]}"' not in defconfig.read_text():
        # The XNU build passes it as a define and does not track defines: kern_exec.o (its only user) is built again only if removed.
        print(f'NOTE: set CONFIG_XNU_OWNER_DATA_VOLUME_UUID="{seed["volume_uuid"]}" in {defconfig}, merge it again (unixnu '
              f'defconfig -D <board> droidvm_ipados27_defconfig), remove {tree}/build/xnu/ios-obj/*/bsd/DEVELOPMENT/kern_exec.o '
              f'and build the kernel again')


if __name__ == '__main__':
    main()
