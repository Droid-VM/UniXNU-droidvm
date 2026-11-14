"""The board's drivers that are kexts of the kernel collection (ipados27/kexts/*.json, see build-droidvm-kexts.py), for the
builders of the kernel's own archives: what to leave out of the kernel when a driver is a kext.

Found beside this file (the device repository's ipados27/scripts), or, for a builder the build tree runs from its own copy of
the scripts, through the tree's kits/droidvm link to the device repository's package/ (its sibling ipados27/).

A spec names the userspace targets it serves ("targets": ["ios"], ["macosx"] or both; ios when absent): the kernel build's
UNIXNU_XNU_TARGET (the OS profile's target) selects them, or the target argument where no kernel build runs."""
import json
import os
from pathlib import Path


def board():
    here = Path(__file__).resolve().parent
    for candidate in (here.parent, (here.parent / 'kits/droidvm').resolve().parent / 'ipados27'):
        if (candidate / 'kexts').is_dir():
            return candidate
    return None


def target(given=None):
    return given or os.environ.get('UNIXNU_XNU_TARGET') or 'ios'


def specs(given=None):
    root = board()
    if root is None:
        return []
    wanted = target(given)
    found = [json.loads(p.read_text()) for p in sorted((root / 'kexts').glob('*.json'))]
    return [spec for spec in found if wanted in spec.get('targets', ['ios'])]


def builtin_left_out(given=None):
    """Names of build-droidvm-devices.py's objects (sources without suffix) that are kexts now."""
    return {name for spec in specs(given) for name in spec.get('builtin', [])}


def attach_left_out(given=None):
    """The attach functions the board's late probe no longer calls."""
    found = set()
    for spec in specs(given):
        attach = spec['attach']
        found.update(attach if isinstance(attach, list) else [attach])
    return found
