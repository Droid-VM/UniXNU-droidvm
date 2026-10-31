#!/usr/bin/env python3
"""Write the daemon's current config of the rig's VM into the app's files/vms.json, as the app's editor saves it.

droidvmd keeps its VM configs in memory: it reads files/vms.json once when it starts, and vm_modify changes only that copy.
The app's editor writes both, the store (files/vms.json, VMStore.save) and the daemon (vm_modify when it starts the VM), so a
change made over IPC alone (a rig script) is gone when the daemon restarts, and is undone when the VM is started from the
app, which hands the daemon the store's copy. Run this after any vm_modify a change should outlive.

  vmpersist.py              the VM of rig.env (VM)
  vmpersist.py VM_ID

Keeps a copy of the file as files/vms.json.bak-<date>, and rewrites it in place (owner, mode and SELinux label stay).
"""
import datetime
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rigenv  # noqa: E402,F401  (fills os.environ from rig.env)

PHONE = os.environ['PHONE']
IPC = os.environ['DVMIPC']
RUN = '/data/data/cn.classfun.droidvm/run'
STORE = '/data/data/cn.classfun.droidvm/files/vms.json'


def su(command, data=None):
    return subprocess.run(['adb', '-s', PHONE, 'shell', f"su -c '{command}'"], input=data, capture_output=True, check=True,
                          timeout=120).stdout


def ipc(port, token, verb, payload):
    out = subprocess.run(['python3', IPC, port, token, verb, json.dumps(payload)], capture_output=True, text=True, check=True,
                         timeout=120).stdout
    # After the client's own lines ("auth: {...}"), the answer: from the first line that is a JSON object.
    lines = out.splitlines()
    return json.loads('\n'.join(lines[next(i for i, line in enumerate(lines) if line.startswith('{')):]))


def main():
    vm_id = sys.argv[1] if len(sys.argv) > 1 else os.environ['VM']
    port = su(f'cat {RUN}/droidvmd-port.txt').decode().strip()
    token = su(f'cat {RUN}/droidvmd-token.txt').decode().strip()
    subprocess.run(['adb', '-s', PHONE, 'forward', f'tcp:{port}', f'tcp:{port}'], check=True, capture_output=True)
    answer = ipc(port, token, 'vm_get', {'vm_id': vm_id})
    data = answer.get('data') or answer
    config = data.get('config', data)
    if config.get('id') != vm_id:
        sys.exit(f'the daemon has no VM {vm_id}')
    store = json.loads(su(f'cat {STORE}'))
    vms = store['vms']
    at = [i for i, vm in enumerate(vms) if vm.get('id') == vm_id]
    if len(at) != 1:
        sys.exit(f'{STORE} holds {len(at)} VMs of id {vm_id}: not changing it')
    if vms[at[0]] == config:
        print(f'{STORE}: VM {vm_id} already as the daemon has it')
        return
    changed = sorted(k for k in set(vms[at[0]]) | set(config) if vms[at[0]].get(k) != config.get(k))
    vms[at[0]] = config
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    su(f'cp -p {STORE} {STORE}.bak-{stamp}')
    # cat into the existing file: its inode, owner, mode and label stay the app's.
    su(f'cat > {STORE}', json.dumps(store, indent=2).encode())
    if json.loads(su(f'cat {STORE}'))['vms'][at[0]] != config:
        sys.exit(f'{STORE} did not take the config (backup {STORE}.bak-{stamp})')
    print(f'{STORE}: VM {vm_id} written as the daemon has it ({", ".join(changed)}); backup {STORE}.bak-{stamp}')


if __name__ == '__main__':
    main()
