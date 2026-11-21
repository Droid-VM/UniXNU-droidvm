#!/usr/bin/env python3
"""A release of this board as a .vmpkg the DroidVM app imports: a new VM, its disks, its settings, nothing of one phone in it.

  make-vmpkg.py --release DIR --device ipad|mac --out NAME.vmpkg [--name NAME] [--screen WxH[@HZ]] [--memory-mb N] [--cpus N]
                [--data-gib N] [--volume-mib N] [--level N] [--allow-dirty] [--no-verify]
  make-vmpkg.py --verify NAME.vmpkg

DIR is build-droidvm-all.py's release/ (droidvm-disk.img, os.img, systemos.img, macOS's app.img, release.json). The VM is the
device's template, ipados27/vmpkg/<device>.vm.json (vmportable.py template: disks by file name, cpu_pin_auto, no extra
options), with the name, screen, memory and vCPUs given. Its disks: the release's, read-only, and a new Data store (the label
in block 0, the rest zeros: --data-gib, 16 for an iPad, 32 for a Mac -- the store holds the seed's container, which a macOS
seed makes 24 GiB); the app writes it sparsely, so only what the guest writes takes space. The VM's network card refers to
the package's one network ("net0"), a bridge onto the phone's Wi-Fi: the app's import offers to join one of the phone's own or
to make it. The release must be for a new VM -- a Data seed its own (build-droidvm-all.py without --generation) for an iPad,
since the plugin in an old seed is not this build's; macOS keeps its plugin in the boot disk, any seed will do -- and clean.

The format is the app's (app/src/main/java/cn/classfun/droidvm/lib/pkg, daemon/vm/pkg/VMExportTask.java):
  a 24-byte header ("VMPKG\\0", u16 manifest version, u16 app version code, u16 manifest length, u16 compression (3: zstd),
  u16 sub-volume count, i64 compressed data length), zeros to 4096, the manifest JSON, zeros to the next 4096 (a whole page
  when it ends on one); then, in one file, the data (zstd of a tar: manifest.json first, then each disk) and zeros to 4096; or,
  split (--volume-mib, default 500; 0 for one file), the volume index JSON in this file and the data in NAME.vmpkg.001, .002...
A tar entry past 8 GiB has its size in GNU base-256, as the app's TarReader reads it.

After writing, the package is read back as the app reads it (--no-verify skips that): the header and manifest, the volume
index with each sub-volume's size and CRC32, the decompressed tar's entries against the manifest and their SHA-256 against
release.json (the Data store's against the one computed as it was written). The manifest carries droidvm_release (the
release's commits, seed, fixups and file digests), which the app does not read: what a package was built from.
"""
import argparse
import hashlib
import io
import json
import os
import struct
import subprocess
import sys
import tarfile
import threading
import time
import zlib
from pathlib import Path

BOARD = Path(__file__).resolve().parents[1]
MAGIC = b'VMPKG\0'
HEADER = 24
PAGE = 4096
ZSTD = 3                       # Compression.ZSTD.type
MANIFEST_VERSION = 1           # no backing chains: ManifestFeature's base
APP_VERSION_CODE = 3680        # the app version this was written against (the app checks only that header and manifest agree)
CHUNK = 1 << 20
DATA_GIB = {'ipad': 16, 'mac': 32}
OS_OF = {'ipad': 'ipados-', 'mac': 'macos-'}
STORE_LABEL = b'DroidVMDataStore' + struct.pack('<I', 1)   # tools/rig/make-data-store.sh, package/DroidVMDataStore.c
# The package's network: the app's L2 bridge onto the phone's Wi-Fi (what a VM made in the app usually has).
NETWORK = {'schema': 2, 'name': 'br-wifi', 'bridge_name': 'br-wifi', 'bridge_type': 'linux', 'auto_up': True, 'stp': False,
           'uplink_mode': 'l2', 'l2': {'pseudo_bridge': True, 'uplink': 'WiFi'}, 'pkg_network_ref': 'net0'}


def align(value, strict=False):
    up = (value + PAGE - 1) & ~(PAGE - 1)
    return up + PAGE if strict and up == value else up


def header(manifest_size, compression, volumes, data_size):
    return MAGIC + struct.pack('<HHHHHq', MANIFEST_VERSION, APP_VERSION_CODE, manifest_size, compression, volumes, data_size)


class Hashing(io.RawIOBase):
    """A file read through a SHA-256."""

    def __init__(self, path):
        self.handle = open(path, 'rb')
        self.digest = hashlib.sha256()

    def readable(self):
        return True

    def readinto(self, buffer):
        n = self.handle.readinto(buffer)
        if n:
            self.digest.update(memoryview(buffer)[:n])
        return n


class Store(io.RawIOBase):
    """A new Data store of the given size: the label, then zeros."""

    def __init__(self, size):
        self.size, self.at = size, 0
        self.digest = hashlib.sha256()
        self.zeros = bytes(CHUNK)

    def readable(self):
        return True

    def readinto(self, buffer):
        n = min(len(buffer), self.size - self.at)
        if n <= 0:
            return 0
        view = memoryview(buffer)[:n]
        view[:] = self.zeros[:n] if n <= CHUNK else bytes(n)
        if self.at < len(STORE_LABEL):
            head = STORE_LABEL[self.at:self.at + n]
            view[:len(head)] = head
        self.digest.update(view)
        self.at += n
        return n


class Volumes:
    """The compressed stream, into one file after the head (volume_mib 0) or into NAME.001, .002... of volume_mib each."""

    def __init__(self, out, head_length, volume_mib):
        self.out, self.limit = out, volume_mib << 20
        self.entries, self.size, self.handle, self.crc, self.current = [], 0, None, 0, 0
        if not self.limit:
            self.handle = open(out, 'r+b')
            self.handle.seek(head_length)

    def write(self, data):
        self.size += len(data)
        if not self.limit:
            self.handle.write(data)
            return
        view = memoryview(data)
        while view:
            if self.handle is None or self.current >= self.limit:
                self.roll()
            n = min(len(view), self.limit - self.current)
            self.handle.write(view[:n])
            self.crc = zlib.crc32(view[:n], self.crc)
            self.current += n
            view = view[n:]

    def roll(self):
        self.close_volume()
        index = len(self.entries) + 1
        if index > 999:
            sys.exit('more than 999 sub-volumes: a larger --volume-mib')
        self.handle, self.crc, self.current = open(f'{self.out}.{index:03d}', 'wb'), 0, 0
        self.entries.append(None)

    def close_volume(self):
        if self.limit and self.handle is not None:
            self.handle.close()
            self.entries[-1] = {'index': len(self.entries), 'size': self.current, 'crc32': self.crc}
            self.handle = None

    def finish(self):
        if self.limit:
            self.close_volume()
        else:
            end = self.handle.tell()
            self.handle.write(bytes(align(end) - end))
            self.handle.close()


def vm_config(device, args):
    template = BOARD / 'vmpkg' / f'{device}.vm.json'
    config = json.loads(template.read_text())['config']
    config.pop('id', None)
    if args.name:
        config['name'] = args.name
    if args.memory_mb:
        config['memory_mb'] = args.memory_mb
    if args.cpus:
        config['cpu_count'] = args.cpus
    if args.screen:
        size, _, rate = args.screen.partition('@')
        width, height = (int(v) for v in size.lower().split('x'))
        for screen in config.get('screens', {}).values():
            screen['width'], screen['height'] = width, height
            if rate and 'refresh_rate' in screen:
                screen['refresh_rate'] = int(rate)
    # Each card on the package's network; the import joins it to one of the phone's or makes it.
    config['networks'] = [{'pkg_network_ref': NETWORK['pkg_network_ref']} for _ in config.get('networks', [])]
    return config


def make(args):
    release_dir = args.release.resolve()
    release = json.loads((release_dir / 'release.json').read_text())
    if not release.get('clean') and not args.allow_dirty:
        sys.exit(f'{release_dir}: a release built from checkouts with local changes (release.json "changed"); --allow-dirty')
    board = release.get('board', '')
    if OS_OF[args.device] not in board:
        sys.exit(f'{release_dir}: a release of {board!r}, not one for --device {args.device}')
    config = vm_config(args.device, args)
    template_disks = config.pop('disks')
    data_bytes = (args.data_gib or DATA_GIB[args.device]) << 30
    disks, sources = [], []
    for index, disk in enumerate(template_disks):
        name = disk['path']
        if name == 'data-store.img':
            size, source = data_bytes, None
        else:
            path = release_dir / name
            if not path.is_file():
                sys.exit(f'{release_dir}: no {name}, a disk of the {args.device} template')
            want = release.get('files', {}).get(name, {})
            if want.get('bytes') != path.stat().st_size:
                sys.exit(f'{path}: {path.stat().st_size} bytes, release.json says {want.get("bytes")}')
            size, source = path.stat().st_size, path
        disks.append({'index': index, 'readonly': bool(disk.get('readonly')), 'bus': disk.get('bus', 'virtio').upper(),
                      'path': name, 'name': name, 'format': 'raw', 'size': size, 'archive_path': name, 'attached': True})
        sources.append(source)
    manifest = {
        'manifest_version': MANIFEST_VERSION, 'format': 'vmpkg', 'created_at': int(time.time() * 1000),
        'app_version': 'make-vmpkg', 'app_version_code': APP_VERSION_CODE, 'app_build_type': 'release',
        'compression': 'ZSTD', 'vm': config, 'disks': disks, 'boots': [],
        'networks': [dict(NETWORK)] if config['networks'] else [],
        'droidvm_release': {key: release.get(key) for key in ('built', 'board', 'commits', 'seed', 'unique_chip_id',
                                                               'ui_scale_fixups', 'trust_sha256', 'files')},
    }
    manifest_bytes = json.dumps(manifest, indent=1).encode()
    if len(manifest_bytes) > 0xffff:
        sys.exit(f'the manifest is {len(manifest_bytes)} bytes; the header holds at most 65535')
    out = args.out.resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    for stale in sorted(out.parent.glob(out.name + '.[0-9][0-9][0-9]')):
        stale.unlink()
    head = bytes(align(HEADER)) + manifest_bytes       # the header (written last, over these zeros), its padding, the manifest
    head += bytes(align(len(head), strict=True) - len(head))
    out.write_bytes(head)
    volumes = Volumes(out, len(head), args.volume_mib)

    zstd = subprocess.Popen(['zstd', '-q', f'-{args.level}', '-T0', '-c'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    readers, failure = [], []

    def produce():
        try:
            with tarfile.open(fileobj=zstd.stdin, mode='w|', format=tarfile.GNU_FORMAT, copybufsize=CHUNK) as tar:
                info = tarfile.TarInfo('manifest.json')
                info.size, info.mode, info.mtime = len(manifest_bytes), 0o644, manifest['created_at'] // 1000
                tar.addfile(info, io.BytesIO(manifest_bytes))
                for disk, source in zip(disks, sources):
                    reader = Store(disk['size']) if source is None else Hashing(source)
                    readers.append(reader)
                    info = tarfile.TarInfo(disk['archive_path'])
                    info.size, info.mode, info.mtime = disk['size'], 0o644, manifest['created_at'] // 1000
                    print(f'  {disk["archive_path"]}: {disk["size"] >> 20} MiB', flush=True)
                    tar.addfile(info, io.BufferedReader(reader, CHUNK))
        except BaseException as error:   # noqa: BLE001 -- reported after the pipe closes
            failure.append(error)
        finally:
            zstd.stdin.close()

    producer = threading.Thread(target=produce)
    producer.start()
    while chunk := zstd.stdout.read(CHUNK):
        volumes.write(chunk)
    producer.join()
    if zstd.wait() or failure:
        sys.exit(f'packing failed: {failure[0] if failure else "zstd exited " + str(zstd.returncode)}')
    volumes.finish()
    digests = {}
    for disk, reader in zip(disks, readers):
        digests[disk['name']] = reader.digest.hexdigest()
        want = release.get('files', {}).get(disk['name'], {}).get('sha256')
        if want and want != digests[disk['name']]:
            sys.exit(f'{disk["name"]}: SHA-256 {digests[disk["name"]]} as packed, release.json says {want}')
    count = len(volumes.entries)
    with open(out, 'r+b') as handle:
        handle.write(header(len(manifest_bytes), ZSTD, count, volumes.size))
        if count:
            handle.seek(len(head))
            handle.write(json.dumps({'magic': 'VMPV', 'version': 1, 'data_size': volumes.size,
                                     'volumes': volumes.entries}).encode())
            handle.truncate()
    total = sum(d['size'] for d in disks)
    print(f'{out}: {volumes.size >> 20} MiB of {total >> 20} MiB' + (f' in {count} sub-volumes' if count else ''))
    return out, digests


def verify(path, digests=None):
    """Read the package as the app reads it; returns the manifest."""
    path = Path(path)
    data = path.read_bytes() if path.stat().st_size < (64 << 20) else open(path, 'rb').read(1 << 20)
    if data[:6] != MAGIC:
        sys.exit(f'{path}: no VMPKG header')
    version, app_code, manifest_size, compression, count, data_size = struct.unpack_from('<HHHHHq', data, 6)
    manifest = json.loads(data[align(HEADER):align(HEADER) + manifest_size])
    start = align(align(HEADER) + manifest_size, strict=True)
    problems = []
    if (version, app_code) != (manifest['manifest_version'], manifest['app_version_code']) or compression != ZSTD \
            or manifest['compression'].upper() != 'ZSTD':
        problems.append('header and manifest disagree')
    if any(data[HEADER:align(HEADER)]) or any(data[align(HEADER) + manifest_size:start]):
        problems.append('padding is not zero')
    if count:
        index = json.loads(path.read_bytes()[start:])
        if index.get('magic') != 'VMPV' or index.get('version') != 1 or len(index['volumes']) != count \
                or index['data_size'] != data_size:
            problems.append('volume index disagrees with the header')
        parts = []
        for number, entry in enumerate(index['volumes'], 1):
            part = Path(f'{path}.{number:03d}')
            if entry['index'] != number or part.stat().st_size != entry['size']:
                problems.append(f'{part.name}: size or index off')
            parts.append((part, entry['crc32']))
    else:
        parts = [(path, None)]
        size = path.stat().st_size
        if size != align(start + data_size):
            problems.append(f'one file of {size} bytes, the header says {align(start + data_size)}')
    zstd = subprocess.Popen(['zstd', '-q', '-d', '-c'], stdin=subprocess.PIPE, stdout=subprocess.PIPE)

    def feed():
        for part, crc in parts:
            with open(part, 'rb') as handle:
                if crc is None:
                    handle.seek(start)
                left, running = data_size if crc is None else part.stat().st_size, 0
                while left > 0:
                    chunk = handle.read(min(CHUNK, left))
                    running = zlib.crc32(chunk, running)
                    zstd.stdin.write(chunk)
                    left -= len(chunk)
            if crc is not None and running != crc:
                problems.append(f'{part.name}: CRC32 {running:08x}, the index says {crc:08x}')
        zstd.stdin.close()

    feeder = threading.Thread(target=feed)
    feeder.start()
    seen = []
    with tarfile.open(fileobj=zstd.stdout, mode='r|') as tar:
        for member in tar:
            if not member.isfile():
                continue
            stream, digest, length = tar.extractfile(member), hashlib.sha256(), 0
            while chunk := stream.read(CHUNK):
                digest.update(chunk)
                length += len(chunk)
            seen.append((member.name, length, digest.hexdigest()))
    zstd.stdout.read()
    feeder.join()
    if zstd.wait():
        problems.append('the data does not decompress')
    if not seen or seen[0][0] != 'manifest.json':
        problems.append('the tar does not begin with manifest.json')
    expected = [('manifest.json', manifest_size)] + [(d['archive_path'], d['size']) for d in manifest['disks']]
    if [(name, length) for name, length, _ in seen] != expected:
        problems.append(f'tar entries {[(n, l) for n, l, _ in seen]}, the manifest says {expected}')
    files = (manifest.get('droidvm_release') or {}).get('files') or {}
    for name, _, digest in seen[1:]:
        want = (digests or {}).get(name) or files.get(name, {}).get('sha256')
        if want and want != digest:
            problems.append(f'{name}: SHA-256 {digest}, expected {want}')
    if problems:
        sys.exit(f'{path}: ' + '; '.join(problems))
    print(f'{path}: verified ({len(seen) - 1} disks, {count or 1} file{"s" if count else ""} of data)')
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--release', type=Path)
    parser.add_argument('--device', choices=sorted(DATA_GIB))
    parser.add_argument('--out', type=Path)
    parser.add_argument('--name')
    parser.add_argument('--screen', help='WIDTHxHEIGHT[@HZ] of every screen (the template: 2048x2732@120)')
    parser.add_argument('--memory-mb', type=int)
    parser.add_argument('--cpus', type=int)
    parser.add_argument('--data-gib', type=int, help='the Data store (default 16 for ipad, 32 for mac)')
    parser.add_argument('--volume-mib', type=int, default=500, help='sub-volume size; 0: one file (default 500, as the app)')
    parser.add_argument('--level', type=int, default=3, help='zstd level (default 3)')
    parser.add_argument('--allow-dirty', action='store_true')
    parser.add_argument('--no-verify', action='store_true')
    parser.add_argument('--verify', type=Path, metavar='PACKAGE', help='only read PACKAGE back as the app does')
    args = parser.parse_args()
    if args.verify:
        verify(args.verify)
        return
    if not (args.release and args.device and args.out):
        parser.error('--release, --device and --out (or --verify PACKAGE)')
    if args.volume_mib and args.volume_mib < 16:
        parser.error('--volume-mib: 0 or at least 16 (the app\'s smallest)')
    out, digests = make(args)
    if not args.no_verify:
        verify(out, digests)


if __name__ == '__main__':
    main()
