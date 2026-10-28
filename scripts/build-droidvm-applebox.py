#!/usr/bin/env python3
"""Build applebox for this guest and trust it: the debug shell's coreutils, one multicall binary of Apple's own open-source
command-line tools (github.com/Droid-VM/applebox; feeds.toml pins it, unixnu fetch puts it at kits/applebox of the tree).
zsh is the shell and dropbear the ssh server; applebox is the utilities beside them.

  build-droidvm-applebox.py TREE_ROOT          APPLEBOX=DIR takes another checkout of applebox instead of kits/applebox

The binary is signed with the curated debug entitlement set (package/debug-entitlements.plist, 109 keys -- the full
lockdownd+debugger set, plans/UNIXNUSETTING.md sec 1.2) and trusted through the owner module (build/owner-trust-extra/
applebox.json, category 3). Output: build/applebox/applebox. Staged with
  export.py put build/applebox/applebox /private/var/ngpu/bin/applebox 0755
and the per-command names symlinked to it."""
import json
import os
import subprocess
import sys
from pathlib import Path

TREE = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else sys.exit(__doc__)
HERE = Path(__file__).resolve().parents[1]
PLIST = HERE / 'package/debug-entitlements.plist'
SOURCE = Path(os.environ.get('APPLEBOX', TREE / 'kits/applebox'))
OUT = TREE / 'build/applebox'
if not (SOURCE / 'build.py').is_file():
    sys.exit(f'no applebox at {SOURCE}: run unixnu fetch (feeds.toml), or set APPLEBOX to a checkout')
subprocess.run([sys.executable, SOURCE / 'build.py', OUT, '--entitlements', PLIST, '--identifier', 'org.droidvm.applebox'],
               check=True)
built = json.loads((OUT / 'applebox.json').read_text())
extra = TREE / 'build/owner-trust-extra'
extra.mkdir(parents=True, exist_ok=True)
(extra / 'applebox.json').write_text(json.dumps(
    {'cdhashes': [built['cdhash']], 'category': 3, 'binary': 'build/applebox/applebox', 'commands': built['commands'],
     'aliases': built['aliases'], 'source': built['source'] + ' (github.com/Droid-VM/applebox)'}, indent=2) + '\n')
print('applebox trusted:', built['cdhash'])
