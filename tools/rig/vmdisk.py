#!/usr/bin/env python3
"""Give the rig's VM one more read-only virtio-mmio disk, right after another of its disks, everywhere its configuration lives.

  vmdisk.py add PATH AFTER     PATH: the image on the phone (/data/media/0/...); AFTER: the base name of the disk it follows

The daemon keeps the VM's configuration in memory and the app keeps it in files/vms.json, which it hands the daemon again when the
VM is started from the app (vm_modify), so a disk added to one of them alone is undone by the other. This changes the rig's
unixnu-vm.config.json, the daemon's copy (vm_modify, the VM stopped) and the app's store (vmpersist.py, kept as
vms.json.bak-<date>). Read-only: the board's verified disks are read-only, and DarwinBoot takes the first writable disk for the
Data store. Nothing is changed when the VM has PATH already. (A macOS release's app.img, the application cryptex: deploy27.sh.)
"""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rigenv  # noqa: E402,F401  (fills os.environ from rig.env)
import vmpersist  # noqa: E402

RIG = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(RIG, 'unixnu-vm.config.json')


def main():
    if len(sys.argv) != 4 or sys.argv[1] != 'add':
        sys.exit(__doc__)
    path, after = sys.argv[2], sys.argv[3]
    whole = json.load(open(CONFIG))
    config = whole['config']
    options = config.get('extra_options', [])
    if any(o.split(',')[0] == path for o in options):
        print(f'the VM has {path} already')
        return
    at = [i for i, o in enumerate(options) if o.split(',')[0].rsplit('/', 1)[-1] == after and i and options[i - 1] == '--virtio-mmio-block']
    if len(at) != 1:
        sys.exit(f'{CONFIG}: no single --virtio-mmio-block .../{after} to put {path} after')
    options[at[0] + 1:at[0] + 1] = ['--virtio-mmio-block', f'{path},ro=true']
    port = vmpersist.su(f'cat {vmpersist.RUN}/droidvmd-port.txt').decode().strip()
    token = vmpersist.su(f'cat {vmpersist.RUN}/droidvmd-token.txt').decode().strip()
    subprocess.run(['adb', '-s', vmpersist.PHONE, 'forward', f'tcp:{port}', f'tcp:{port}'], check=True, capture_output=True)
    answer = vmpersist.ipc(port, token, 'vm_modify', {'config': config})
    if answer.get('success') is False:
        sys.exit(f'vm_modify refused: {answer}')
    json.dump(whole, open(CONFIG, 'w'), indent=1)
    open(CONFIG, 'a').write('\n')
    print(f'{path} added after {after}: {CONFIG} and the daemon')
    subprocess.run([sys.executable, os.path.join(RIG, 'vmpersist.py'), config['id']], check=True)


if __name__ == '__main__':
    main()
