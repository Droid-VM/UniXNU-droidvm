#!/usr/bin/env python3
"""The macOS guest's owner boot shims, for its Data seed: the apfs_boot_util launchd's mount-phase-1/2 boot tasks run.

  build-droidvm-macos-shims.py --tree TREE

On the board's layout the root container is the owner-verified OS image, read-only and holding the System volume only, so the
original apfs_boot_util (which creates and mounts the VM, Preboot, Update, xART and Hardware volumes of the root container)
aborted (container_load: EROFS) and launchd panicked. ipados27/macos/apfs_boot_util.c is upstream UniXNU's
scripts/macos-boot-shims/apfs_boot_util.c (14236be, MIT, unchanged): every phase has nothing to mount and succeeds. Built for
arm64e macOS 27 as upstream's build-boot-runtime.py builds it (ad hoc, 4 KiB pages), to TREE/build/droidvm-macos-shims/bootshim/;
its CDHash goes to TREE/build/owner-trust-extra/macos-boot-shims.json, which build-droidvm-trust.py takes into the owner trust
module. make-droidvm-macos-seed.py --shims puts bootshim/ at /private/var/ngpu/bootshim of the Data seed, which the kernel shows
over apfs.fs/Contents/Resources with apfs-boot-shim=1 (macos-apfs-boot-shim.patch).
"""
import argparse
import json
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent


def source(tree):
    """ipados27/macos/apfs_boot_util.c beside these scripts, or through the tree's kits/droidvm link to the device repository
    (a builder the tree runs from its own copy of the scripts)."""
    for board in (HERE.parent, (tree / 'kits/droidvm').resolve().parent / 'ipados27'):
        if (board / 'macos/apfs_boot_util.c').is_file():
            return board / 'macos/apfs_boot_util.c'
    raise SystemExit('no ipados27/macos/apfs_boot_util.c')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--tree', type=Path, required=True)
    tree = parser.parse_args().tree.resolve()
    out = tree / 'build/droidvm-macos-shims/bootshim'
    out.mkdir(parents=True, exist_ok=True)
    exe = out / 'apfs_boot_util'
    sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
    subprocess.run(['xcrun', '--sdk', 'macosx', 'clang', '-target', 'arm64e-apple-macos27.0', '-isysroot', sdk, '-O2', '-Wall',
                    '-Werror', source(tree), '-o', exe], check=True)
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.unixnu.apfs_boot_util', exe],
                   check=True)
    detail = subprocess.run(['codesign', '--display', '--verbose=4', exe], capture_output=True, text=True, check=True).stderr
    cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]
    extra = tree / 'build/owner-trust-extra'
    extra.mkdir(parents=True, exist_ok=True)
    (extra / 'macos-boot-shims.json').write_text(json.dumps(
        {'cdhashes': [cdhash], 'binary': str(exe.relative_to(tree)),
         'source': 'ipados27/macos/apfs_boot_util.c (upstream UniXNU scripts/macos-boot-shims), arm64e-apple-macos27.0'},
        indent=2) + '\n')
    print('apfs_boot_util CDHash', cdhash)


if __name__ == '__main__':
    main()
