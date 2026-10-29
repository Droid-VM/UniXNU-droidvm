#!/usr/bin/env python3
"""Merge preference values into a copy of a packed Data seed.

  seed-data-prefs.py BASE.dbr OUTPUT.dbr USER:DOMAIN:KEY=VALUE...

USER is mobile or root (their Library/Preferences under the Data volume); VALUE is true, false, an integer or a string. As the
fork's seed-data-skip-setup.py edits a seed: BASE (a DBRDISK1 packed APFS container, drivers/DarwinBoot/Test/packed_ramdisk.py)
is expanded to a raw image, attached read-write on this Mac, the values are merged into DOMAIN.plist, and the result is packed
again. The volume UUID is unchanged; BASE is not modified. Run where hdiutil may attach images (not on an external volume over ssh).
"""
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'drivers/DarwinBoot/Test'))
import packed_ramdisk  # noqa: E402

HOMES = {'mobile': 'mobile', 'root': 'root'}


def value_of(text):
    if text in ('true', 'false'):
        return text == 'true'
    try:
        return int(text, 0)
    except ValueError:
        return text


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    base, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    if output.exists():
        sys.exit(f'Refuse to overwrite {output}')
    edits = {}
    for spec in sys.argv[3:]:
        user, domain, assignment = spec.split(':', 2)
        key, _, text = assignment.partition('=')
        if user not in HOMES or not key or not text:
            sys.exit(f'{spec}: USER:DOMAIN:KEY=VALUE with USER mobile or root')
        edits.setdefault((user, domain), {})[key] = value_of(text)
    logical, extents = packed_ramdisk.regions(base.read_bytes())
    with tempfile.TemporaryDirectory(prefix='data-prefs-', dir=str(ROOT / 'build')) as td:
        raw = Path(td) / 'seed.raw'
        with raw.open('wb') as f:
            f.truncate(logical)
            for offset, data in extents:
                f.seek(offset)
                f.write(data)
        mount = Path(td) / 'mount'
        mount.mkdir()
        attached = plistlib.loads(subprocess.check_output(
            ['hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nobrowse', '-owners', 'off',
             '-mountpoint', str(mount), '-plist', str(raw)]))
        device = attached['system-entities'][0]['dev-entry']
        try:
            if not (mount / 'mobile').is_dir():
                sys.exit('The Data seed has no /mobile; wrong base image?')
            for (user, domain), values in sorted(edits.items()):
                path = mount / HOMES[user] / 'Library/Preferences' / (domain + '.plist')
                path.parent.mkdir(parents=True, exist_ok=True)
                data = plistlib.loads(path.read_bytes()) if path.exists() else {}
                data.update(values)
                path.write_bytes(plistlib.dumps(data, fmt=plistlib.FMT_BINARY))
                os.chmod(path, 0o600)
                print('pref', user, domain, values)
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True)
        metadata = packed_ramdisk.pack(raw, output)
    report = dict(base_sha256=packed_ramdisk.inspect(base)[0]['sha256'], output=metadata,
                  preferences={f'{user}:{domain}': values for (user, domain), values in sorted(edits.items())})
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
