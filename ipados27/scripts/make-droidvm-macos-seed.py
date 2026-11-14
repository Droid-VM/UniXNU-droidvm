#!/usr/bin/env python3
"""The Data seed of the DroidVM macOS 27 guest, as a generation (build-droidvm-all.py --os macos-27.0.1-26A434 --generation DIR).

  python3 TREE/scripts/make-droidvm-macos-seed.py --tree TREE --ipsw IPSW_DIR --output DIR [--gib 24]
       [--user NAME:PASSWORD [--auto-login]] [--lima droidvm-apfs]

macOS's Data template keeps its owners (root and service accounts such as _windowserver), and only root keeps them in a copy;
the build Mac has no password-less sudo, and as another user it cannot even read the template's root-only entries. So the seed's
content is written in a Linux VM, as root:

  on the Mac, without root   two empty APFS containers of one volume each (hdiutil create: a 2 GiB scratch, a 512 MiB Data),
                             moved where the VM sees them
  in a Linux VM, as root     make-macos-data-linux.py: the APFS driver (linux-apfs-rw, built in the VM by setup-lima-apfs.sh when it
                             is not loaded) mounts the OS image read-only and the containers read-write, and upstream UniXNU's
                             make-macos-data-seed.py steps run there (its functions where they are portable): the system volume's
                             /System/Library/Templates/Data copied with its owners, Setup Assistant marked done, with --user a
                             local administrator (uid 501) and its home, the template's other files linked to the system volume's,
                             then the result alone into the Data container. The VM is lima's (--lima NAME, on the Mac's
                             Virtualization framework: no sudo), with /Volumes/DATA/UniXNU mounted read-write at the same path
  on the Mac                 a new volume UUID in the Data container's superblocks,

then packed for DarwinBoot and grown to GIB GiB for the persistent Data store (droidvm-data-persist=2). The kernel mounts Data by
the volume UUID (CONFIG_XNU_OWNER_DATA_VOLUME_UUID, set by build-droidvm-all.py from seed.json). Run as root (sudo) it is upstream's
script as it is, owners kept by ditto, and no VM.

DIR gets the seed (data-<GIB>g-macos.dbr) and seed.json (build, volume_uuid, seed, seed_sha256, os_image). The OS image is the
decrypted IPSW member of IPSW_DIR, attached from a copy on the system volume: macOS refuses an image attachment on an external
volume to a process started over ssh (as build-droidvm-data-seed.py's scratch directory).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

HERE = Path(__file__).resolve().parent
SCRATCH = Path('/private/tmp/droidvm-macos-seed')


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as stream:
        while chunk := stream.read(1 << 22):
            digest.update(chunk)
    return digest.hexdigest()


def os_image(ipsw):
    """The decrypted OS member (its record names the component; the plain image sits beside the .aea)."""
    for record in sorted(ipsw.glob('*.json')):
        info = json.loads(record.read_text())
        if info.get('component') != 'OS':
            continue
        path = record.with_name(record.name[:-len('.json')])
        plain = path.with_suffix('') if path.suffix == '.aea' else path
        if plain.is_file():
            return plain
        raise SystemExit(f'{plain} is not decrypted yet (build-droidvm-all.py --os ... --only setup)')
    raise SystemExit(f'{ipsw}: no OS member (unixnu ipsw fetch OS)')


def in_vm(lima, *command):
    """A command as root in the lima VM (the Mac's /Volumes/DATA/UniXNU at the same path there)."""
    return subprocess.run(['limactl', 'shell', lima, 'sudo', *[str(part) for part in command]], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, required=True, help='the macOS build tree (out/droidvm/macos-.../droidvm_macos27/tree)')
    parser.add_argument('--ipsw', type=Path, required=True, help='the IPSW cache (out/ipsw/26A434)')
    parser.add_argument('--output', type=Path, required=True, help='the generation directory to create')
    parser.add_argument('--gib', type=int, default=24, help='the Data container size (default 24)')
    parser.add_argument('--user', help='NAME:PASSWORD of a local administrator, logged in at the login window')
    parser.add_argument('--auto-login', action='store_true', help='log that user in at boot (a test seed only)')
    parser.add_argument('--lima', default='droidvm-apfs', help='the lima VM that sets the owners (without root; default droidvm-apfs)')
    args = parser.parse_args()
    if args.auto_login and not args.user:
        parser.error('--auto-login needs --user')
    if args.output.exists():
        parser.error(f'{args.output} exists: a generation is made once')
    tree, ipsw, output = args.tree.resolve(), args.ipsw.resolve(), args.output.resolve()
    root = os.geteuid() == 0
    if not root and not str(output).startswith('/Volumes/DATA/UniXNU/'):
        parser.error('without root the generation goes below /Volumes/DATA/UniXNU, which the lima VM mounts')
    build = ipsw.name
    image = os_image(ipsw)
    SCRATCH.mkdir(parents=True, exist_ok=True)
    volume = str(uuid.uuid4()).upper()
    seed = f'data-{args.gib}g-macos.dbr'
    output.mkdir(parents=True)
    options = ['--skip-setup', '--volume-uuid', volume, *(['--user', args.user] if args.user else []),
               *(['--auto-login'] if args.auto_login else [])]
    if root:
        local = SCRATCH / image.name     # upstream's script attaches the OS image: from the system volume
        if not (local.is_file() and local.stat().st_size == image.stat().st_size):
            print(f'copying {image} to {local} (an image is attached from the system volume)', flush=True)
            shutil.copyfile(image, local)
        subprocess.run([sys.executable, tree / 'scripts/make-macos-data-seed.py', local, output / seed, '--gib', str(args.gib),
                        *options], check=True, cwd=tree)
    else:
        sys.path.insert(0, str(tree / 'drivers/DarwinBoot/Test'))
        import packed_ramdisk
        spec = __import__('importlib.util').util.spec_from_file_location('upstream_seed', tree / 'scripts/make-macos-data-seed.py')
        upstream = __import__('importlib.util').util.module_from_spec(spec)
        spec.loader.exec_module(upstream)
        shared = {}
        for name, size in (('scratch', '2g'), ('data', '512m')):
            made = SCRATCH / f'{name}.cdr'
            made.unlink(missing_ok=True)
            subprocess.run(['hdiutil', 'create', '-quiet', '-size', size, '-layout', 'NONE', '-fs', 'APFS', '-volname', 'Data',
                            '-type', 'UDTO', SCRATCH / name], check=True)
            shared[name] = output / f'{name}.cdr'          # where the VM sees it
            shutil.copyfile(made, shared[name])
            made.unlink()
        if subprocess.run(['limactl', 'shell', args.lima, 'grep', '-qw', 'apfs', '/proc/filesystems']).returncode:
            in_vm(args.lima, 'bash', tree / 'scripts/setup-lima-apfs.sh')
        in_vm(args.lima, 'python3', tree / 'scripts/make-macos-data-linux.py', '--os-image',
              tree / 'build/droidvm-root-images/os.img', '--scratch', shared['scratch'], '--data', shared['data'],
              *[o for o in options if o not in ('--volume-uuid', volume)])
        upstream.set_volume_uuid(shared['data'], uuid.UUID(volume))
        base = output / 'base.dbr'
        packed_ramdisk.pack(shared['data'], base)
        subprocess.run([sys.executable, tree / 'scripts/seed-data-grow.py', '--gib', str(args.gib), base, output / seed],
                       check=True)
        for path in (base, shared['scratch'], shared['data']):
            path.unlink()
    record = {'build': build, 'volume_uuid': volume, 'seed': seed, 'seed_sha256': sha256(output / seed),
              'os_image': image.name, 'user': args.user.split(':', 1)[0] if args.user else None, 'auto_login': args.auto_login,
              'owners': 'ditto as root' if root else f'make-macos-data-linux.py as root in lima {args.lima}',
              'made_by': 'make-droidvm-macos-seed.py (upstream UniXNU make-macos-data-seed.py 14236be)'}
    (output / 'seed.json').write_text(json.dumps(record, indent=2) + '\n')
    (output / 'trust').mkdir()
    owner = os.environ.get('SUDO_UID'), os.environ.get('SUDO_GID')
    if root and owner[0] and owner[1]:
        for path in (output, output / seed, output / 'seed.json', output / 'trust'):
            os.chown(path, int(owner[0]), int(owner[1]))
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
