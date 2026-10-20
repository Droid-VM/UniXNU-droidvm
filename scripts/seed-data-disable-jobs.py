#!/usr/bin/env python3
"""Make a Data seed whose launchd overrides disable the jobs this platform cannot serve: a copy of an existing packed Data seed
with /private/var/db/com.apple.xpc.launchd/disabled.plist written in it.

  seed-data-disable-jobs.py BASE.dbr OUTPUT.dbr LABEL [LABEL...]

UniXNU-sheng writes the same file from the kernel on every boot (package/BootWatchdogPolicy.c, sheng-disable-watchdogd=1 and
sheng-disable-jobs=..., hooked into load_init_program only with XNU_BOOT_PROGRESS). A DroidVM Data volume is volatile, so the
file can as well be part of the seed: the same plist (label -> true) and the empty disabled.migrated marker that selects launchd's
modern override loader. The UniXNU guide (docs/guide/skip-setup.md) lists the jobs that block the first boot; watchdogd has no
watchdog backend here.

The seed is expanded, attached read-write on this Mac (ownership off, as for seed-data-skip-setup.py: the files belong to the
user who runs it, like the rest of the fixture) and packed again. The volume UUID is unchanged; BASE is not modified.
"""
import json
import os
import plistlib
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'drivers/DarwinBoot/Test'))
import packed_ramdisk


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    base, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    labels = sys.argv[3:]
    if any(not re.fullmatch(r'[A-Za-z0-9.-]+', label) for label in labels):
        sys.exit('A launchd label has only letters, digits, dots and dashes')
    if output.exists():
        sys.exit('Refuse to overwrite ' + str(output))
    logical, extents = packed_ramdisk.regions(base.read_bytes())
    with tempfile.TemporaryDirectory(prefix='data-seed-', dir=str(ROOT / 'build')) as scratch:
        raw = Path(scratch) / 'seed.raw'
        with raw.open('wb') as handle:
            handle.truncate(logical)
            for offset, data in extents:
                handle.seek(offset)
                handle.write(data)
        mount = Path(scratch) / 'mount'
        mount.mkdir()
        attached = plistlib.loads(subprocess.check_output(
            ['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse', '-owners', 'off',
             '-mountpoint', str(mount), '-plist', str(raw)]))
        device = attached['system-entities'][0]['dev-entry']
        try:
            if not (mount / 'mobile').is_dir():
                sys.exit('The Data seed has no /mobile; wrong base image?')
            directory = mount / 'db/com.apple.xpc.launchd'
            directory.mkdir(parents=True, exist_ok=True)
            os.chmod(directory, 0o755)
            policy = directory / 'disabled.plist'
            policy.write_bytes(plistlib.dumps({label: True for label in labels}, fmt=plistlib.FMT_XML))
            os.chmod(policy, 0o644)
            marker = directory / 'disabled.migrated'
            marker.touch()
            os.chmod(marker, 0o644)
            print('disabled', ', '.join(labels))
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True)
        if raw.read_bytes()[32:36] != b'NXSB':
            sys.exit('The edited image is no APFS container any more')
        metadata = packed_ramdisk.pack(raw, output)
    report = dict(base_sha256=packed_ramdisk.inspect(base)[0]['sha256'], output=metadata, disabled_jobs=labels,
                  source='UniXNU-sheng package/BootWatchdogPolicy.c')
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
