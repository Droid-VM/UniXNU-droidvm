#!/usr/bin/env python3
"""The content of a macOS Data seed, written as root on Linux with the APFS driver (make-droidvm-macos-seed.py, in a lima VM).

  make-macos-data-linux.py --os-image OS.img --scratch SCRATCH.cdr --data DATA.cdr [--skip-setup] [--user NAME:PASSWORD
                           [--auto-login]]

Upstream UniXNU's make-macos-data-seed.py (beside this script) builds the seed on macOS as root, where ditto keeps the Data
template's owners. The build Mac has no password-less sudo, and as another user it can neither read the template's root-only
entries nor give a copy their owners; here the same steps run as root in a Linux VM with linux-apfs-rw's driver
(setup-lima-apfs.sh), taking that script's own functions where they are portable (the user record, the auto login, the Setup
Assistant skip list, the volume logs) and doing in Linux terms what it does with macOS tools:

  ditto TEMPLATE MOUNT     cp -dR --preserve=mode,ownership,timestamps,links: owners, modes, links. Not the extended attributes:
                           the template's are the compression's (com.apple.decmpfs with its resource fork; the driver reads the
                           files decompressed, and Linux sets no UF_COMPRESSED that would make macOS read them) and
                           com.apple.rootless, which upstream's copy cannot keep either (no process but Apple's may set it)
  chflags / st_flags       none on Linux (and none set: no flags are copied)

SCRATCH and DATA are empty APFS containers of one volume each (hdiutil create -layout NONE -fs APFS -type UDTO: the Mac makes
them without root). The template goes to SCRATCH and is reduced there (every file outside upstream's KEEP becomes a link to the
system volume's template copy, the READONLY directories one link each); DATA then gets SCRATCH's result alone, so it holds no
blocks of the replaced files. The OS image is mounted read-only; DATA's volume UUID, packing and growing follow on the Mac.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import stat
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('upstream_seed', HERE / 'make-macos-data-seed.py')
seed = importlib.util.module_from_spec(spec)
spec.loader.exec_module(seed)


def run(*command):
    return subprocess.run([str(part) for part in command], check=True, capture_output=True, text=True).stdout.strip()


class Mounted:
    def __init__(self, image, point, writable):
        self.image, self.point, self.writable = image, point, writable

    def __enter__(self):
        with open(self.image, 'rb') as f:
            bare = f.read(36)[32:36] == b'NXSB'
        offset = 0 if bare else self.gpt_apfs_offset()
        self.loop = run('losetup', '-f', '--show', *([] if self.writable else ['-r']), '-o', offset, self.image)
        self.point.mkdir(parents=True, exist_ok=True)
        run('mount', '-t', 'apfs', '-o', 'readwrite' if self.writable else 'ro', self.loop, self.point)
        return self.point

    def gpt_apfs_offset(self):
        import struct
        import uuid
        with open(self.image, 'rb') as f:
            for sector in (512, 4096):
                f.seek(sector)
                header = f.read(92)
                if header[:8] != b'EFI PART':
                    continue
                entries, count, size = struct.unpack_from('<QII', header, 72)
                f.seek(entries * sector)
                table = f.read(count * size)
                for i in range(count):
                    entry = table[i * size:(i + 1) * size]
                    if uuid.UUID(bytes_le=entry[:16]) == uuid.UUID('7C3457EF-0000-11AA-AA11-00306543ECAC'):
                        return struct.unpack_from('<Q', entry, 32)[0] * sector
        raise SystemExit(f'{self.image}: no APFS container')

    def __exit__(self, *_):
        os.sync()
        subprocess.run(['umount', str(self.point)], check=False)
        subprocess.run(['losetup', '-d', self.loop], check=False)


def copy_tree(source, target):
    """ditto SOURCE TARGET as root keeps it, without the extended attributes (see above)."""
    run('cp', '-dR', '--preserve=mode,ownership,timestamps,links', f'{source}/.', target)


def link_template_directories(mount):
    for relative in seed.READONLY:
        path = mount / relative
        if not path.is_dir() or path.is_symlink():
            continue
        st = os.lstat(path)
        run('rm', '-rf', path)
        os.symlink('/' + seed.TEMPLATE + '/' + relative, path)
        os.lchown(path, st.st_uid, st.st_gid)


def link_template_files(mount):
    kept = linked = 0
    for current, _, files in os.walk(mount):
        for name in files:
            path = Path(current) / name
            relative = path.relative_to(mount).as_posix()
            st = os.lstat(path)
            if not stat.S_ISREG(st.st_mode) or relative.startswith(seed.KEEP):
                kept += stat.S_ISREG(st.st_mode)
                continue
            path.unlink()
            os.symlink('/' + seed.TEMPLATE + '/' + relative, path)
            os.lchown(path, st.st_uid, st.st_gid)
            linked += 1
    return kept, linked


def add_home(template, mount, name, uid=501, gid=20):
    """upstream's add_home, with cp for ditto: the user template's unlocalized and English parts, .skipbuddy, the user's own."""
    home = mount / 'Users' / name
    home.mkdir(parents=True)
    for part in ('Non_localized', 'English.lproj'):
        source = template / seed.USER_TEMPLATE / part
        if source.is_dir():
            copy_tree(source, home)
    (home / '.skipbuddy').touch()
    run('chown', '-R', f'{uid}:{gid}', home)
    home.chmod(0o750)
    for private in ('Desktop', 'Documents', 'Downloads', 'Library', 'Movies', 'Music', 'Pictures'):
        if (home / private).is_dir():
            (home / private).chmod(0o700)
    permissions = template / seed.USER_TEMPLATE / '__permissions.plist'
    for entry in plistlib.loads(permissions.read_bytes()) if permissions.is_file() else []:
        for path in entry.get('Paths', []) if entry.get('Command') == 'CHMOD' else []:
            target = home / path.lstrip('/')
            if target.exists():
                target.chmod(int(entry['Argument'], 8))


def remove_volume_logs(mount):
    for name in ('.fseventsd', '.Spotlight-V100'):
        run('rm', '-rf', mount / name)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--os-image', type=Path, required=True)
    parser.add_argument('--scratch', type=Path, required=True)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--skip-setup', action='store_true')
    parser.add_argument('--user', help='NAME:PASSWORD of a local administrator to add')
    parser.add_argument('--auto-login', action='store_true')
    parser.add_argument('--install', action='append', default=[], metavar='SRC:DEST',
                        help='a directory tree put at DEST of the volume (below private/: kept as real files), owned by root:wheel '
                             '(the owner boot shims at private/var/ngpu/bootshim)')
    args = parser.parse_args()
    if os.geteuid() != 0:
        parser.error('run as root (the APFS driver, the template\'s owners)')
    if 'apfs' not in open('/proc/filesystems').read():
        parser.error('the APFS driver is not loaded (setup-lima-apfs.sh)')
    if args.auto_login and not args.user:
        parser.error('--auto-login needs --user')
    with tempfile.TemporaryDirectory(prefix='macos-data-') as work:
        work = Path(work)
        with Mounted(args.os_image.resolve(), work / 'os', False) as system:
            template = system / seed.TEMPLATE
            if not (system / 'sbin/launchd').is_file() or not template.is_dir():
                raise SystemExit(f'{args.os_image}: not a macOS system volume')
            with Mounted(args.scratch.resolve(), work / 'scratch', True) as scratch:
                copy_tree(template, scratch)
                for entry in args.install:
                    source, _, destination = entry.partition(':')
                    target = scratch / destination
                    target.mkdir(parents=True, exist_ok=True)
                    run('cp', '-dR', '--preserve=mode,timestamps', f'{source}/.', target)
                    run('chown', '-R', '0:0', target)   # its parents made here are root's already (the VM's root makes them)
                if args.skip_setup:
                    seed.write_root(scratch / 'private/var/db/.AppleSetupDone', b'', 0o644)
                if args.user:
                    seed.add_user(scratch / seed.DSLOCAL, *args.user.split(':', 1))
                link_template_directories(scratch)
                kept, linked = link_template_files(scratch)
                if args.skip_setup:
                    seed.write_root(scratch / seed.SETUP_SKIP,
                                    plistlib.dumps({'SkipSetupItems': seed.SETUP_SKIP_ITEMS}, fmt=plistlib.FMT_XML), 0o644)
                if args.user:
                    add_home(template, scratch, args.user.split(':', 1)[0])
                if args.auto_login:
                    seed.add_auto_login(scratch, *args.user.split(':', 1))
                remove_volume_logs(scratch)
                with Mounted(args.data.resolve(), work / 'data', True) as data:
                    copy_tree(scratch, data)
                    remove_volume_logs(data)
                    owners = {}
                    for current, dirs, files in os.walk(data):
                        for name in dirs + files:
                            st = os.lstat(Path(current) / name)
                            owners[st.st_uid] = owners.get(st.st_uid, 0) + 1
    print(json.dumps({'kept': kept, 'linked': linked, 'readonly_links': len(seed.READONLY), 'owners_by_uid': owners,
                      'user': args.user.split(':', 1)[0] if args.user else None}, indent=2))


if __name__ == '__main__':
    main()
