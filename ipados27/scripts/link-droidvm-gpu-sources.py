#!/usr/bin/env python3
"""Link the GPU sources build-droidvm-gpu.py compiles into this board's tree, from the UniXNU checkout the tree belongs to.

The iPadOS 17 board's tree holds every driver; this schema 2 board's holds only its components, and selecting Adreno740 (or
AdrenoMetal, which depends on it) would also build sheng's own GPU driver, whose SM8550 power and firmware code this board has no
use for and which does not build here. build-droidvm-gpu.py takes only NativeAdreno's user client, surface, execbuf and memory
from it, and build-adreno740-accelerator.py the staged accelerator (with AdrenoMetal's ABI header beside it): this step links
drivers/Adreno740, drivers/AdrenoMetal, kits/ArmSmmu4K and kits/AdrenoPM4 of the checkout (its pinned submodules) into the tree.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECKOUT = ROOT.parents[4]          # <UniXNU>/out/<kit>/<os profile>/<board>/tree


def main():
    for relative in ('drivers/Adreno740', 'drivers/AdrenoMetal', 'kits/ArmSmmu4K', 'kits/AdrenoPM4'):
        source, link = CHECKOUT / relative, ROOT / relative
        if not source.is_dir() or not any(source.iterdir()):
            raise SystemExit(f'{source}: not checked out (git submodule update --init {relative})')
        if link.is_symlink() or link.exists():
            if link.resolve() == source.resolve():
                continue
            link.unlink()
        link.symlink_to(source)
        print(f'{relative} -> {source}')


if __name__ == '__main__':
    main()
