#!/usr/bin/env python3
"""Bundle the two unchanged original trust-cache modules from the extracted IPSW members.

The counterpart of UniXNU's build-original-system-trust.py for a tree that has the members "unixnu ipsw
fetch" extracted (StaticTrustCache and Cryptex1,SystemTrustCache) instead of the whole restore archive.
Each member is an IM4P; its payload must be the exact version-2 module whose SHA-256 the upstream script
pins, and the bundle has the same layout (count, offsets, modules), so payload.py accepts it by hash.

  build-original-trust-from-members.py IPSW_MEMBER_DIR
"""
import hashlib
import json
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
MEMBERS = (('044-76040-038.dmg.trustcache', 'da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2'),
           ('044-76034-038.dmg.trustcache', 'd121ba89167e03aafd00fa687c30cd8ebc4ccafd10c1408adb2ba3de12403651'))


def der(data, at):
    if at + 2 > len(data):
        raise ValueError('Truncated DER')
    tag, size = data[at:at + 2]
    at += 2
    if size & 128:
        count = size & 127
        if not 0 < count <= 4 or at + count > len(data):
            raise ValueError('Invalid DER length')
        size = int.from_bytes(data[at:at + count], 'big')
        at += count
    if at + size > len(data):
        raise ValueError('DER exceeds original input')
    return tag, data[at:at + size], at + size


def payload(im4p):
    tag, sequence, end = der(im4p, 0)
    fields, cursor = [], 0
    while cursor < len(sequence):
        kind, data, cursor = der(sequence, cursor)
        fields.append((kind, data))
    if tag != 0x30 or end != len(im4p) or [kind for kind, _ in fields][:4] != [0x16, 0x16, 0x16, 4] or fields[0][1] != b'IM4P':
        raise SystemExit('Require an original trust-cache IM4P')
    return fields[3][1]


directory = Path(sys.argv[1])
modules = []
for name, digest in MEMBERS:
    module = payload((directory / name).read_bytes())
    if (hashlib.sha256(module).hexdigest() != digest or struct.unpack_from('<I', module)[0] != 2 or
            len(module) != 24 + struct.unpack_from('<I', module, 20)[0] * 24):
        raise SystemExit(f'{name}: not the complete exact original version-2 trust module')
    modules.append(module)
static, system = modules
bundle = struct.pack('<3I', 2, 12, 12 + len(static)) + static + system
out = ROOT / 'build/original-static-system-trust'
out.mkdir(parents=True, exist_ok=True)
(out / 'original-static-system.bundle').write_bytes(bundle)
(out / 'inputs.json').write_text(json.dumps(dict(original_static_sha256=MEMBERS[0][1], original_system_sha256=MEMBERS[1][1],
    bundle_sha256=hashlib.sha256(bundle).hexdigest(), source='extracted IPSW members',
    own_code_entries_added=False, original_modules_modified=False, apple_personalization_claimed=False), indent=2) + '\n')
print('Bundled only two unchanged original modules:', hashlib.sha256(bundle).hexdigest())
