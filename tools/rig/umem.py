#!/usr/bin/env python3
"""Small reader of a guest process's memory for interactive digging: umem.py PID EXPR... where EXPR is an address chain
like 0x1f06131e0 or *0x1f06131e0+8 (dereference, then add). Prints each step."""
import struct
import sys
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from export import request


def read(pid, address, size):
    body, _ = request(4, str(pid), address, size)
    return body


def q(pid, address):
    return struct.unpack('<Q', read(pid, address, 8))[0] & 0x0000ffffffffffff
