#!/usr/bin/env python3
"""Write a guest kernel variable of the running VM: poke.py NAME VALUE [SIZE] (SIZE 1, 2, 4 or 8 bytes, default 4; VALUE decimal or
0x...). For A/B switches the guest reads as it goes (e.g. droidvm_virtio_retire_on_offer, droidvm_virtio_present_wait)."""
import struct
import sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import export
import peek
if len(sys.argv) not in (3, 4):
    sys.exit(__doc__)
name, value = sys.argv[1], int(sys.argv[2], 0)
size = int(sys.argv[3]) if len(sys.argv) == 4 else 4
pid, maps = peek.crosvm_maps()
export.write_guest(pid, maps, export.physical(name), value.to_bytes(size, 'little'))
print(f'{name} = {value}')
