#!/usr/bin/env python3
"""Stage ownervisor (the owner service manager) into a copy of a packed Data seed that already carries the GPU plugin.

  add-ownervisor-data.py BASE.dbr OUTPUT.dbr

ownervisor is launched once at boot by the kernel launch-policy hook (package/DroidVMLaunchPolicy.c, owner-service-manager=1):
launchd takes its jobs from the signed compiled cache and ignores the ngpu/daemons overlay's ProgramArguments, and a new label
is never enumerated, so the kernel posix_spawns the manager itself. The GPU probe is not lost: ownervisor runs
/private/var/ngpu/bin/adrenoprobed as its first managed service (keepalive false, one shot), then the owner debug services.

Baked under /private/var/ngpu (the writable Data volume):
  bin/ownervisor bin/ownerctl bin/ownertest     the trusted binaries (build/droidvm-ownervisor, build-droidvm-ownervisor.py)
  ownervisor/services.d/adrenoprobe.json         exec adrenoprobed once (GPU probe), enabled
  ownervisor/services.d/selftest.json            exec ownertest (proves owner->owner spawn under AMFI), enabled
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

OV = ROOT / 'build/droidvm-ownervisor'


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    base, output = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    if output.exists():
        sys.exit('Refuse to overwrite ' + str(output))
    for name in ('ownervisor', 'ownerctl', 'ownertest'):
        if not (OV / name).is_file():
            sys.exit('Build ownervisor first: python3 scripts/build-droidvm-ownervisor.py')
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
            for name in ('ownervisor', 'ownerctl', 'ownertest'):
                shutil.copyfile(OV / name, bin_dir / name)
                os.chmod(bin_dir / name, 0o755)
            # toybox: ~137 coreutils applets + a shell in one trusted multicall binary (scripts/build-droidvm-toybox.py),
            # so the run verb has real tools on an otherwise bare iPadOS (/bin holds only df and ps). Called as
            # `toybox <applet> ...`; no symlinks needed. Optional -- staged only when it has been built.
            toybox = ROOT / 'build/toybox/toybox'
            if toybox.is_file():
                shutil.copyfile(toybox, bin_dir / 'toybox')
                os.chmod(bin_dir / 'toybox', 0o755)
            # zsh: Apple's own shell (apple-oss-distributions/zsh, scripts/build-droidvm-zsh.py), a real interactive shell for
            # the run verb and, later, the ssh login shell. Static modules, so the one binary needs no module dir. Optional.
            zsh = ROOT / 'build/zsh/zsh'
            if zsh.is_file():
                shutil.copyfile(zsh, bin_dir / 'zsh')
                os.chmod(bin_dir / 'zsh', 0o755)
            # dropbear: a pubkey-only ssh server (scripts/build-droidvm-dropbear.py), patched to use zsh as the login shell and
            # to read authorized_keys from /private/var/ngpu/ssh (export.py put writes there; the home /var/root is not writable
            # by the rig). The ssh dir is 0700 root so checkfileperm accepts the keys. Optional.
            dropbear = ROOT / 'build/dropbear'
            if (dropbear / 'dropbear').is_file():
                for b in ('dropbear', 'dropbearkey'):
                    shutil.copyfile(dropbear / b, bin_dir / b)
                    os.chmod(bin_dir / b, 0o755)
                ssh_dir = mount / 'ngpu/ssh'
                ssh_dir.mkdir(parents=True, exist_ok=True)
                os.chmod(ssh_dir, 0o700)
            # The managed services. ownervisor creates run/log/state/control at startup; the seed carries the
            # definitions and the enable markers so they autostart from the first boot.
            services = {
                'adrenoprobe': {'exec': ['/private/var/ngpu/bin/adrenoprobed'], 'keepalive': False, 'uid': 0},
                'selftest': {'exec': ['/private/var/ngpu/bin/ownertest', 'boot'], 'keepalive': True, 'throttle': 10},
            }
            if (dropbear / 'dropbear').is_file():
                # Foreground (-F) so ownervisor supervises it; log to stderr (-E); generate the host key on first need (-R;
                # the default path is /private/var/ngpu/ssh, patched in build-droidvm-dropbear.py). pubkey only. Binds every
                # address on 2222; the login shell is zsh. Needs the rig's key at /private/var/ngpu/ssh/authorized_keys.
                services['dropbear'] = {'exec': [
                    '/private/var/ngpu/bin/dropbear', '-F', '-E', '-R', '-p', '2222'], 'keepalive': True, 'throttle': 5}
            svc_dir, en_dir = mount / 'ngpu/ownervisor/services.d', mount / 'ngpu/ownervisor/enabled'
            for d in (mount / 'ngpu/ownervisor', svc_dir, en_dir):
                d.mkdir(parents=True, exist_ok=True)
                os.chmod(d, 0o755)
            for name, spec in services.items():
                (svc_dir / (name + '.json')).write_text(json.dumps(spec, indent=2) + '\n')
                os.chmod(svc_dir / (name + '.json'), 0o644)
                (en_dir / name).write_text('')
                os.chmod(en_dir / name, 0o644)
            print('staged ngpu/bin:', sorted(p.name for p in bin_dir.iterdir()))
            print('staged services:', sorted(services))
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True)
        if raw.read_bytes()[32:36] != b'NXSB':
            sys.exit('The edited image is no APFS container any more')
        metadata = packed_ramdisk.pack(raw, output)
    report = dict(base_sha256=packed_ramdisk.inspect(base)[0]['sha256'], output=metadata,
                  ownervisor=json.loads((ROOT / 'build/owner-trust-extra/ownervisor.json').read_text()))
    output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
    print('ownervisor seed:', output)


if __name__ == '__main__':
    main()
