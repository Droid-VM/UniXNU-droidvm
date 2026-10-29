#!/usr/bin/env python3
"""Stage the debug shell (ownervisor, its shell and its services) into a copy of a packed Data seed that already carries the
GPU plugin.

  add-ownervisor-data.py BASE.dbr OUTPUT.dbr

ownervisor is launched once at boot by the kernel launch-policy hook (package/DroidVMLaunchPolicy.c, owner-service-manager=1):
launchd takes its jobs from the signed compiled cache and ignores the ngpu/daemons overlay's ProgramArguments, and a new label
is never enumerated, so the kernel posix_spawns the manager itself. The GPU probe is not lost: ownervisor runs
/private/var/ngpu/bin/adrenoprobed as its first managed service (keepalive false, one shot), then the owner debug services.

Baked under /private/var/ngpu (the writable Data volume):
  bin/                       the debug shell (build/debug-shell/bin: ownervisor, ownerctl, ownertest, applebox, dropbear,
                             dropbearkey, zsh, debugserver, libimobiledevice and its tool links) and the board's toybox, gfxprobe,
                             ipatool, lsdiag
  ssh/                       dropbear's keys (0700)
  ownervisor/services.d/     the debug shell's services (dropbear, selftest, serial-shell) and the board's adrenoprobe
  ownervisor/enabled/        the debug shell's boot services (dropbear, selftest)
BASE is not modified and the volume UUID is unchanged."""
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

SHELL = ROOT / 'build/debug-shell'


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    base, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    if output.exists():
        sys.exit('Refuse to overwrite ' + str(output))
    if not (SHELL / 'debug-shell.json').is_file():
        sys.exit('Build the debug shell first: python3 scripts/build-droidvm-ownervisor.py TREE_ROOT')
    blob = base.read_bytes()
    logical, extents = packed_ramdisk.regions(blob)
    with tempfile.TemporaryDirectory(prefix='data-seed-', dir=str(ROOT / 'build')) as td:
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
            bin_dir, daemon_dir = mount / 'ngpu/bin', mount / 'ngpu/daemons'
            if not bin_dir.is_dir() or not (bin_dir / 'adrenoprobed').is_file():
                sys.exit('BASE has no ngpu/bin/adrenoprobed; stage the GPU plugin first (add-gpu-plugin-data.py)')
            # The debug shell (github.com/Droid-VM/ownervisor, scripts/build-droidvm-ownervisor.py): ownervisor, ownerctl and
            # ownertest, the shell -- applebox, dropbear, zsh, debugserver, libimobiledevice with a link per tool -- and its
            # built-in services (dropbear = ssh on port 2222, selftest, serial-shell defined but not started: on this board
            # /dev/console is a sink until XNU's console is wired to a UART the app shows), laid out as they go in.
            for entry in sorted((SHELL / 'bin').iterdir()):
                target = bin_dir / entry.name
                target.unlink(missing_ok=True)
                if entry.is_symlink():
                    os.symlink(os.readlink(entry), target)
                else:
                    shutil.copyfile(entry, target)
                    os.chmod(target, 0o755)
            # dropbear reads authorized_keys and its host keys from /private/var/ngpu/ssh (export.py put writes there; the home
            # /var/root is not writable by the rig): 0700 root, so checkfileperm accepts the keys.
            ssh_dir = mount / 'ngpu/ssh'
            ssh_dir.mkdir(parents=True, exist_ok=True)
            os.chmod(ssh_dir, 0o700)
            # The tools on root's PATH: dropbear starts the shell with /private/var/ngpu/bin first, and iPadOS 27's zsh puts the
            # system's default back for a login shell (17's kept it); every zsh reads ~/.zshenv first.
            zshenv = mount / 'root/.zshenv'
            if (mount / 'root').is_dir() and not zshenv.exists():
                zshenv.write_text('export PATH=/private/var/ngpu/bin:$PATH\n')
                os.chmod(zshenv, 0o644)
            # The board's own tools beside the shell, each staged when it has been built: toybox (~137 coreutils applets and a
            # shell, scripts/build-droidvm-toybox.py), gfxprobe (the GPU and display probes; its orient-follow keeps backboardd's
            # device orientation where the motion sensors say, by hand only), ipatool (the .ipa installer) and lsdiag
            # (LaunchServices questions).
            for name in ('toybox', 'gfxprobe', 'ipatool', 'lsdiag'):
                tool = ROOT / 'build' / name / name
                if tool.is_file():
                    shutil.copyfile(tool, bin_dir / name)
                    os.chmod(bin_dir / name, 0o755)
            # The services: the debug shell's built-in ones (services.d and enabled markers as its build laid them out) and the
            # board's GPU probe. adrenoprobe is defined but NOT started at boot: it opens AGXDeviceUserClient and submits a GPU
            # self-test that can fault (the known CP register-load AHB fault), and running it at boot, racing the real render
            # clients, left the shared GPU state bad and every app's rendering black. Keep it for a manual `ownerctl start
            # adrenoprobe` diagnostic. ownervisor creates run/log/state/control at startup.
            svc_dir, en_dir = mount / 'ngpu/ownervisor/services.d', mount / 'ngpu/ownervisor/enabled'
            for d in (mount / 'ngpu/ownervisor', svc_dir, en_dir):
                d.mkdir(parents=True, exist_ok=True)
                os.chmod(d, 0o755)
            for definition in sorted((SHELL / 'services.d').glob('*.json')):
                shutil.copyfile(definition, svc_dir / definition.name)
            for marker in sorted((SHELL / 'enabled').iterdir()):
                (en_dir / marker.name).write_text('')
            (svc_dir / 'adrenoprobe.json').write_text(json.dumps(
                {'exec': ['/private/var/ngpu/bin/adrenoprobed'], 'keepalive': False, 'uid': 0}, indent=2) + '\n')
            for path in list(svc_dir.iterdir()) + list(en_dir.iterdir()):
                os.chmod(path, 0o644)
            # No orientd: backboardd takes the device orientation from CoreMotion and the kernel's motion HID services
            # (package/DroidVMMotionHid.inc) since 2026-10-28; `gfxprobe orient-follow` stays for debugging by hand.
            print('staged ngpu/bin:', sorted(p.name for p in bin_dir.iterdir()))
            print('staged services:', sorted(p.stem for p in svc_dir.iterdir()), 'at boot:', sorted(p.name for p in en_dir.iterdir()))
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True)
        if raw.read_bytes()[32:36] != b'NXSB':
            sys.exit('The edited image is no APFS container any more')
        metadata = packed_ramdisk.pack(raw, output)
    report = dict(base_sha256=packed_ramdisk.inspect(base)[0]['sha256'], output=metadata,
                  debug_shell=json.loads((SHELL / 'debug-shell.json').read_text()))
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print('ownervisor seed:', output)


if __name__ == '__main__':
    main()
