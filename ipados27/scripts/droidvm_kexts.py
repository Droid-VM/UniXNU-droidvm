"""The board's drivers that are kexts of the kernel collection (ipados27/kexts/*.json, see build-droidvm-kexts.py), for the
builders of the kernel's own archives: what to leave out of the kernel when a driver is a kext.

Found beside this file (the device repository's ipados27/scripts), or, for a builder the build tree runs from its own copy of
the scripts, through the tree's kits/droidvm link to the device repository's package/ (its sibling ipados27/)."""
import json
from pathlib import Path


def board():
    here = Path(__file__).resolve().parent
    for candidate in (here.parent, (here.parent / 'kits/droidvm').resolve().parent / 'ipados27'):
        if (candidate / 'kexts').is_dir():
            return candidate
    return None


def specs():
    root = board()
    if root is None:
        return []
    return [json.loads(p.read_text()) for p in sorted((root / 'kexts').glob('*.json'))]


def builtin_left_out():
    """Names of build-droidvm-devices.py's objects (sources without suffix) that are kexts now."""
    return {name for spec in specs() for name in spec.get('builtin', [])}


def attach_left_out():
    """The attach functions the board's late probe no longer calls."""
    return {spec['attach'] for spec in specs()}
