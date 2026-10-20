#!/usr/bin/env python3
"""Press the Home button of the running guest once: native_hid_input_command = 1 (HidInputProbe.cpp). NativeHidInput's
timer (1 s) takes the request and sends a Consumer Menu press, then the release 100 ms later."""
import struct
import sys

sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
import export
import peek

pid, maps = peek.crosvm_maps()
export.write_guest(pid, maps, export.physical('native_hid_input_command'), struct.pack('<I', 1))
print('Home requested')
