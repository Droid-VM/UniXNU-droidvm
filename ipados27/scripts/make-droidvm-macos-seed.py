#!/usr/bin/env python3
"""The Data seed of the DroidVM macOS 27 guest, as a generation (build-droidvm-all.py --os macos-27.0.1-26A434 --generation DIR).

  sudo python3 TREE/scripts/make-droidvm-macos-seed.py --tree TREE --ipsw IPSW_DIR --output DIR
       [--gib 24] [--user NAME:PASSWORD [--auto-login]]

Runs as root: macOS's Data template keeps its owners (root and service accounts such as _windowserver), and only root keeps them in
a copy. The seed itself is upstream UniXNU's scripts/make-macos-data-seed.py (14236be, unchanged, beside this script as
make-macos-data-seed.py): the IPSW system volume's /System/Library/Templates/Data in a new case-insensitive APFS volume, Setup
Assistant marked done, with --user a local administrator (uid 501) and its home, the template's other files linked to the system
volume's, packed for DarwinBoot and grown to GIB GiB for the persistent Data store (droidvm-data-persist=2). This gives it a new
volume UUID, which the kernel mounts Data by (CONFIG_XNU_OWNER_DATA_VOLUME_UUID, set by build-droidvm-all.py from seed.json).

DIR gets the seed (data-<GIB>g-macos.dbr) and seed.json (build, volume_uuid, seed, seed_sha256, os_image), owned by the user who
ran sudo. The OS image is the decrypted IPSW member of IPSW_DIR (unixnu ipsw fetch OS, decrypted by build-droidvm-all.py's setup);
it is attached from a copy on the system volume: macOS refuses an image attachment on an external volume to a process started over
ssh (as build-droidvm-data-seed.py's scratch directory).
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


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, required=True, help='the macOS build tree (out/droidvm/macos-.../droidvm_macos27/tree)')
    parser.add_argument('--ipsw', type=Path, required=True, help='the IPSW cache (out/ipsw/26A434)')
    parser.add_argument('--output', type=Path, required=True, help='the generation directory to create')
    parser.add_argument('--gib', type=int, default=24, help='the Data container size (default 24)')
    parser.add_argument('--user', help='NAME:PASSWORD of a local administrator, logged in at the login window')
    parser.add_argument('--auto-login', action='store_true', help='log that user in at boot (a test seed only)')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('run with sudo: the Data template keeps its owners')
    if args.auto_login and not args.user:
        parser.error('--auto-login needs --user')
    if args.output.exists():
        parser.error(f'{args.output} exists: a generation is made once')
    tree, ipsw = args.tree.resolve(), args.ipsw.resolve()
    build = ipsw.name
    image = os_image(ipsw)
    SCRATCH.mkdir(parents=True, exist_ok=True)
    local = SCRATCH / image.name
    if not (local.is_file() and local.stat().st_size == image.stat().st_size):
        print(f'copying {image} to {local} (an image is attached from the system volume)', flush=True)
        shutil.copyfile(image, local)
    volume = str(uuid.uuid4()).upper()
    seed = f'data-{args.gib}g-macos.dbr'
    args.output.mkdir(parents=True)
    command = [sys.executable, tree / 'scripts/make-macos-data-seed.py', local, args.output / seed, '--gib', str(args.gib),
               '--skip-setup', '--volume-uuid', volume]
    if args.user:
        command += ['--user', args.user]
    if args.auto_login:
        command.append('--auto-login')
    subprocess.run([str(part) for part in command], check=True, cwd=tree)
    record = {'build': build, 'volume_uuid': volume, 'seed': seed, 'seed_sha256': sha256(args.output / seed),
              'os_image': image.name, 'user': args.user.split(':', 1)[0] if args.user else None, 'auto_login': args.auto_login,
              'made_by': 'make-droidvm-macos-seed.py (upstream UniXNU make-macos-data-seed.py 14236be)'}
    (args.output / 'seed.json').write_text(json.dumps(record, indent=2) + '\n')
    (args.output / 'trust').mkdir()
    owner = os.environ.get('SUDO_UID'), os.environ.get('SUDO_GID')
    if owner[0] and owner[1]:
        for path in (args.output, args.output / seed, args.output / 'seed.json', args.output / 'trust'):
            os.chown(path, int(owner[0]), int(owner[1]))
    print(json.dumps(record, indent=2))


if __name__ == '__main__':
    main()
