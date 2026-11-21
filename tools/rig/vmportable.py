#!/usr/bin/env python3
"""Turn the rig's UniXNU VM into one the app alone describes: no phone's paths or uids in hand-written extra options.

  vmportable.py show [--keep-debug]      what would change (nothing is changed)
  vmportable.py apply [--keep-debug]     change it, the VM stopped: the daemon (vm_modify), the rig's unixnu-vm.config.json and the
                                         app's store (vmpersist.py, files/vms.json.bak-<date> kept)
  vmportable.py template OUT             OUT: the same configuration with nothing of this phone in it (no id, no network, no CPU
                                         affinity; each disk named by its file alone), the VM template a vmpkg is made from

The UniXNU VMs were made by the rig before the app could say what they need, so their extra options carry it: the OS, SystemOS
(and macOS's App) images and the Data store as --virtio-mmio-block with this phone's paths, the sound card and the video unit with
this install's uid, the pools, the battery and crosvm's virtio-mmio switches. The app has a setting for each now:

  --virtio-mmio-block P[,ro=true]      a disk of the VM's own (disks, after the boot disk, in the same order), on the VM's virtio-mmio
                                       transport (virtio_transport mmio: every disk, and with it the input, network, GPU, sound,
                                       console and media devices, are on virtio-mmio; the app adds those switches itself)
  --virtio-snd backend=aaudio,...      a VirtIO sound card with a speaker and a microphone (the app's default card)
  --virtio-media kind=decoder,...      video acceleration on (vpu_enabled): the app adds the decoder and the encoder, with the
                                       backend= given (mediacodec, crosvm's default, when none is)
  --pre-alloc drm-host-mb=,media-host-mb=   gpu_drm2kgsl_pool_mb, vpu_host_pool_mb
  --battery type=goldfish              the Battery (goldfish) peripheral
  --dump-device-tree-blob P            dropped (a rig's debugging aid), unless --keep-debug

The guest finds its disks by what they are, not where they are: DarwinBoot names each read-only virtio-mmio disk virtio-mmio-N and
the first writable one the Data store (FdtAdt.c), and NativeVirtioBlock takes the root policy whose image size is the disk's.
Anything else among the extra options stays there (and is said); an option this script does not know how to read stops it.
"""
import copy
import json
import os
import subprocess
import sys

RIG = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(RIG, 'unixnu-vm.config.json')
SWITCHES = {'--virtio-mmio-input', '--virtio-mmio-net', '--virtio-mmio-gpu', '--virtio-mmio-snd', '--virtio-mmio-media',
            '--virtio-mmio-console'}
DEFAULT_SOUND = {'type': 'virtio_sound', 'underrun': 'silence', 'buffer': 'normal', 'endpoints': [
    {'mode': 'speaker', 'host_label': '', 'host_device': 'DEFAULT|system default'},
    {'mode': 'microphone', 'host_label': '', 'host_device': 'DEFAULT|system default'}]}


def keys(value):
    """crosvm's key=value,key=value option, as a dict."""
    out = {}
    for part in value.split(','):
        key, _, val = part.partition('=')
        out[key] = val
    return out


def app_path(path):
    """The app writes its VMs' files under the shared storage's own name; the rig wrote the daemon's."""
    return '/storage/emulated/0/' + path[len('/data/media/0/'):] if path.startswith('/data/media/0/') else path


def portable(source, keep_debug=False):
    """The configuration with the extra options above turned into the app's settings; and what was done, line by line."""
    config = copy.deepcopy(source)
    options = list(config.get('extra_options', []))
    kept, notes = [], []
    disks = config.setdefault('disks', [])
    peripherals = config.setdefault('peripherals', [])
    types = {p.get('type') for p in peripherals}
    index = 0
    while index < len(options):
        option = options[index]
        value = options[index + 1] if index + 1 < len(options) else None
        index += 1
        if option in SWITCHES:
            notes.append(f'{option}: the app adds it on a virtio-mmio VM')
            continue
        if option == '--virtio-mmio-block':
            index += 1
            path, *rest = value.split(',')
            if any(part not in ('ro=true', 'ro=false', 'lock=false') for part in rest):
                sys.exit(f'--virtio-mmio-block {value}: an option this script does not carry over')
            disks.append({'path': app_path(path), 'bus': 'virtio', 'readonly': 'ro=true' in rest})
            notes.append(f'disk {len(disks) - 1}: {app_path(path)}{" (read-only)" if "ro=true" in rest else ""}')
            continue
        if option == '--virtio-snd':
            index += 1
            snd = keys(value)
            if snd.get('backend') != 'aaudio' or snd.get('num_output_devices', '1') != '1' or \
                    snd.get('num_input_devices', '1' if snd.get('capture') == 'true' else '0') not in ('0', '1'):
                sys.exit(f'--virtio-snd {value}: not the one-speaker, one-microphone card this script knows')
            if 'virtio_sound' not in types:
                peripherals.append(copy.deepcopy(DEFAULT_SOUND))
                types.add('virtio_sound')
            notes.append('--virtio-snd: a VirtIO sound card (speaker, microphone)')
            continue
        if option == '--virtio-media':
            index += 1
            media = keys(value)
            if media.get('kind') != 'decoder':
                sys.exit(f'--virtio-media {value}: only the decoder is carried over')
            backend = media.get('backend', 'mediacodec')
            config.update(vpu_enabled=True, vpu_codec_enabled=True, vpu_decoder_backend=backend, vpu_encoder_backend=backend)
            notes.append(f'--virtio-media: video acceleration on (decoder and encoder, backend {backend})')
            continue
        if option == '--pre-alloc':
            index += 1
            for key, val in keys(value).items():
                if key == 'drm-host-mb':
                    config['gpu_drm2kgsl_pool_mb'] = int(val)
                elif key == 'media-host-mb':
                    config['vpu_host_pool_mb'] = int(val)
                else:
                    sys.exit(f'--pre-alloc {value}: {key} has no setting here')
            notes.append(f'--pre-alloc {value}: the GPU and video pools')
            continue
        if option == '--battery':
            index += 1
            if value != 'type=goldfish':
                sys.exit(f'--battery {value}: only the goldfish battery is carried over')
            if 'goldfish_battery' not in types:
                peripherals.append({'type': 'goldfish_battery'})
                types.add('goldfish_battery')
            notes.append('--battery: the Battery (goldfish) peripheral')
            continue
        if option == '--dump-device-tree-blob':
            index += 1
            if keep_debug:
                kept += [option, value]
            notes.append(f'{option} {value}: {"kept" if keep_debug else "dropped"}')
            continue
        kept.append(option)
        notes.append(f'{option}: stays an extra option')
    config['extra_options'] = kept
    config['virtio_transport'] = 'mmio'
    return config, notes


def template(source):
    """Nothing of this phone: the id, the network attachments, the CPU affinity, the files' directories."""
    config, _ = portable(source)
    for key in ('id', 'created_at'):
        config.pop(key, None)
    config['networks'] = [{} for _ in config.get('networks', [])]
    config['cpu_affinity'] = ''
    for disk in config['disks']:
        disk['path'] = disk['path'].rsplit('/', 1)[-1]
    uefi = config.get('boot', {}).get('uefi', {})
    if uefi.get('firmware'):
        uefi['firmware'] = uefi['firmware'].rsplit('/', 1)[-1]
    return config


def changed(old, new):
    return sorted(k for k in set(old) | set(new) if old.get(k) != new.get(k))


def daemon():
    sys.path.insert(0, RIG)
    import rigenv  # noqa: F401  (fills os.environ from rig.env)
    import vmpersist
    port = vmpersist.su(f'cat {vmpersist.RUN}/droidvmd-port.txt').decode().strip()
    token = vmpersist.su(f'cat {vmpersist.RUN}/droidvmd-token.txt').decode().strip()
    subprocess.run(['adb', '-s', vmpersist.PHONE, 'forward', f'tcp:{port}', f'tcp:{port}'], check=True, capture_output=True)
    return vmpersist, port, token


def main():
    args = [a for a in sys.argv[1:] if a != '--keep-debug']
    keep_debug = '--keep-debug' in sys.argv[1:]
    if not args or args[0] not in ('show', 'apply', 'template') or (args[0] == 'template') != (len(args) == 2):
        sys.exit(__doc__)
    whole = json.load(open(CONFIG))
    if args[0] == 'template':
        json.dump({'config': template(whole['config'])}, open(args[1], 'w'), indent=1)
        open(args[1], 'a').write('\n')
        print(f'{args[1]}: the VM template')
        return
    vmpersist, port, token = daemon()
    vm_id = os.environ['VM']
    answer = vmpersist.ipc(port, token, 'vm_get', {'vm_id': vm_id})
    data = answer.get('data') or answer
    current = data.get('config', data)
    if current.get('id') != vm_id:
        sys.exit(f'the daemon has no VM {vm_id}')
    new, notes = portable(current, keep_debug)
    for line in notes:
        print(' ', line)
    print('settings changed:', ', '.join(changed(current, new)) or 'none')
    if args[0] == 'show':
        return
    state = (data.get('state') or data.get('status') or '').lower()
    if state and state not in ('stopped', 'stop', 'created', 'idle'):
        sys.exit(f'the VM is {state}: stop it first (vm.sh stop)')
    answer = vmpersist.ipc(port, token, 'vm_modify', {'config': new})
    if answer.get('success') is False:
        sys.exit(f'vm_modify refused: {answer}')
    os.replace(CONFIG, CONFIG + '.before-portable')
    whole['config'] = new
    json.dump(whole, open(CONFIG, 'w'), indent=1)
    open(CONFIG, 'a').write('\n')
    print(f'the daemon and {CONFIG} have it (the old one: {CONFIG}.before-portable)')
    subprocess.run([sys.executable, os.path.join(RIG, 'vmpersist.py'), vm_id], check=True)


if __name__ == '__main__':
    main()
