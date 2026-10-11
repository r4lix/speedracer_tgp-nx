#!/usr/bin/env python3
"""Walk the block list of a Shockwave 3D (W3D / IFX) scene, as stored in a Director XMED chunk."""
import struct, sys
from collections import Counter


def ifx_payload(data):
    """Strip the XMED '3DEM' wrapper and return the bytes starting at 'IFX\0'."""
    i = data.find(b"IFX\0")
    if i < 0:
        raise ValueError("no IFX header")
    return data[i:]


def blocks(ifx):
    """Yield (type, offset, payload) for every block. Blocks are 4-byte aligned."""
    pos = 0
    while pos + 8 <= len(ifx):
        typ, size = struct.unpack_from("<II", ifx, pos)
        yield typ, pos, ifx[pos + 8:pos + 8 + size]
        pos += 8 + ((size + 3) & ~3)


def name_of(payload):
    n = struct.unpack_from("<H", payload, 0)[0] if len(payload) >= 2 else 0
    s = payload[2:2 + n]
    return s.decode("latin-1") if n and len(s) == n and all(32 <= c < 127 for c in s) else None


if __name__ == "__main__":
    ifx = ifx_payload(open(sys.argv[1], "rb").read())
    verbose = "-v" in sys.argv
    count, total = Counter(), Counter()
    for typ, off, p in blocks(ifx):
        count[typ] += 1
        total[typ] += len(p)
        if verbose:
            print(f"{off:8x} {typ:08x} {len(p):8d} {name_of(p) or ''}")
    for typ, n in sorted(count.items()):
        print(f"{typ:08x} x{n:5d} {total[typ]:9d} bytes")
