#!/usr/bin/env python3
"""Build the debug shell for this guest and trust it: ownervisor (the owner service manager, with ownerctl and ownertest),
the shell it serves (applebox, dropbear, zsh, debugserver, libimobiledevice) and its built-in services (ssh, serial-shell)
-- github.com/Droid-VM/ownervisor; feeds.toml pins it, unixnu fetch puts it at kits/ownervisor of the tree.

  build-droidvm-ownervisor.py TREE_ROOT          OWNERVISOR=DIR takes another checkout of ownervisor instead of kits/ownervisor

Output: build/debug-shell/ (bin/, services.d/, enabled/ and debug-shell.json, as ownervisor's build.py lays them out), which
scripts/add-ownervisor-data.py puts into the Data seed; the kernel's launch-policy hook starts ownervisor at boot as root
(package/DroidVMLaunchPolicy.c, owner-service-manager=1). Every binary is trusted through the owner module
(build/owner-trust-extra/debug-shell.json, category 3 -- unconstrained, as the Adreno probe daemon's
com.apple.iomfb_fdr_loader is). The per-tool records the debug shell replaces are removed: make-owner-trust-bundle.py
refuses a CDHash listed twice."""
import json
import os
import subprocess
import sys
from pathlib import Path

TREE = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else sys.exit(__doc__)
SOURCE = Path(os.environ.get('OWNERVISOR', TREE / 'kits/ownervisor'))
OUT = TREE / 'build/debug-shell'
if not (SOURCE / 'build.py').is_file():
    sys.exit(f'no ownervisor at {SOURCE}: run unixnu fetch (feeds.toml), or set OWNERVISOR to a checkout')
subprocess.run([sys.executable, SOURCE / 'build.py', OUT], check=True)
built = json.loads((OUT / 'debug-shell.json').read_text())
extra = TREE / 'build/owner-trust-extra'
extra.mkdir(parents=True, exist_ok=True)
for replaced in ('ownervisor', 'applebox', 'dropbear', 'zsh', 'debugserver', 'libimobiledevice'):
    (extra / f'{replaced}.json').unlink(missing_ok=True)
(extra / 'debug-shell.json').write_text(json.dumps(
    {'cdhashes': list(built['binaries'].values()), 'category': 3, 'binaries': sorted(built['binaries']),
     'source': 'github.com/Droid-VM/ownervisor: ' + '; '.join(f'{k} {v}' for k, v in built['sources'].items())},
    indent=2) + '\n')
print('debug shell trusted:', ', '.join(f'{name} {cdhash[:8]}' for name, cdhash in built['binaries'].items()))
