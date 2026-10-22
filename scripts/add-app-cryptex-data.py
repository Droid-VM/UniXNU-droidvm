#!/usr/bin/env python3
"""Stage the App cryptex (Cryptex1,AppOS) of the board's IPSW into a copy of a packed Data seed.
  add-app-cryptex-data.py BASE.dbr APP_CRYPTEX.dmg OUTPUT.dbr
iPadOS mounts the App cryptex at /private/preboot/Cryptexes/App: Safari's daemons (webbookmarksd, com.apple.Safari.History,
safarifetcherd, ...), SafariSupport.bundle and Safari's settings pane, which the launchd jobs and frameworks of the OS volume reach
through /System/Cryptexes/App. Here it is owner Data: its files go to /private/var/ngpu/preboot/Cryptexes/App, next to the mount
points Cryptexes/OS/{System,usr} for the SystemOS volume's own directories, and the kernel shows that directory over the empty /private/preboot
with owner-app-cryptex=1 (package/DroidVMLaunchPolicy.c). The binaries keep Apple's signatures and pass AMFI through the cryptex's own
trust module, which the owner trust bundle carries unchanged (make-owner-trust-bundle.py). Only the pinned 21H461 cryptex is accepted.
BASE is not modified and the volume UUID is unchanged. Run where hdiutil may attach images (not on an external volume over ssh)."""
import hashlib
import json
import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'drivers/DarwinBoot/Test'))
import packed_ramdisk

APP_CRYPTEX_SHA256 = '52e568e1eaaede5fde4684ad10afc60f8fb236bfb5a23e0869bb0ed8a1d275d7'   # 044-75188-038.dmg of 21H461


def attach(image, mountpoint, raw=False):
    command = ['hdiutil', 'attach', '-readonly', '-nobrowse', '-mountpoint', str(mountpoint), '-plist', str(image)]
    if raw:
        command = ['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse', '-owners', 'off',
                   '-mountpoint', str(mountpoint), '-plist', str(image)]
    entities = plistlib.loads(subprocess.check_output(command))['system-entities']
    if not any(e.get('mount-point') for e in entities):
        sys.exit('Nothing of ' + str(image) + ' was mounted')
    return entities[0]['dev-entry']   # the image's whole disk, which detach takes


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    base, cryptex, output = (Path(a).resolve() for a in sys.argv[1:])
    if output.exists():
        sys.exit('Refuse to overwrite ' + str(output))
    if hashlib.sha256(cryptex.read_bytes()).hexdigest() != APP_CRYPTEX_SHA256:
        sys.exit('Not the App cryptex of 21H461: ' + str(cryptex))
    blob = base.read_bytes()
    logical, extents = packed_ramdisk.regions(blob)
    with tempfile.TemporaryDirectory(prefix='data-seed-', dir=str(ROOT / 'build')) as td:
        raw = Path(td) / 'seed.raw'
        with raw.open('wb') as f:
            f.truncate(logical)
            for offset, data in extents:
                f.seek(offset)
                f.write(data)
        seed, app = Path(td) / 'seed', Path(td) / 'app'
        seed.mkdir()
        app.mkdir()
        seed_device = attach(raw, seed, raw=True)
        try:
            app_device = attach(cryptex, app)
            try:
                if not (seed / 'mobile').is_dir():
                    sys.exit('The Data seed has no /mobile; wrong base image?')
                preboot = seed / 'ngpu/preboot'
                if preboot.exists():
                    shutil.rmtree(preboot)
                target = preboot / 'Cryptexes/App'
                shutil.copytree(app, target, symlinks=True, ignore=shutil.ignore_patterns('.fseventsd', '.Trashes'))
                # Cryptexes/OS holds only mount points: the kernel binds the SystemOS volume's /System and /usr there, read-only
                # and not as a union (package/DroidVMLaunchPolicy.c). A link to / instead made a directory's union traversal
                # (lookup_traverse_union, at its end) arrive at the same directory again, so getdirentries never ended.
                for name in ('System', 'usr'):
                    (preboot / 'Cryptexes/OS' / name).mkdir(parents=True)
                    os.chmod(preboot / 'Cryptexes/OS' / name, 0o755)
                os.chmod(preboot / 'Cryptexes/OS', 0o755)
                for directory in (seed / 'ngpu', preboot, preboot / 'Cryptexes'):
                    os.chmod(directory, 0o755)
                files = [p for p in target.rglob('*') if p.is_file() and not p.is_symlink()]
                print('staged', target.relative_to(seed), len(files), 'files,', sum(p.stat().st_size for p in files) >> 10, 'KiB')
            finally:
                subprocess.run(['hdiutil', 'detach', app_device], check=True)
        finally:
            subprocess.run(['hdiutil', 'detach', seed_device], check=True)
        if raw.read_bytes()[32:36] != b'NXSB':
            sys.exit('The edited image is no APFS container any more')
        metadata = packed_ramdisk.pack(raw, output)
    report = dict(base_sha256=packed_ramdisk.inspect(base)[0]['sha256'], output=metadata, app_cryptex_sha256=APP_CRYPTEX_SHA256)
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
