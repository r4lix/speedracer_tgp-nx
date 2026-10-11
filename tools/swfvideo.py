"""Pull an embedded video out of a Flash movie (XMED chunk) as an FLV.

Usage: python tools/swfvideo.py <XMED-nnnn.bin> <out.flv> [stream id] [fps]
DefineVideoStream (tag 60) says which codec the frames use, VideoFrame (tag 61) carries them; for Sorenson H.263 the
payload is what an FLV video tag holds. Without a stream id the one with the most frames is taken.
"""
import struct
import sys
from collections import defaultdict
from pathlib import Path

import swfdump


def walk(d, p, end, defs, frames):
    for code, t in swfdump.tags(d, p, end):
        if code == 39:
            walk(t, 4, len(t), defs, frames)
        elif code == 60:
            cid, n, w, h, fl, codec = struct.unpack_from("<HHHHBB", t)
            defs[cid] = (n, w, h, codec)
        elif code == 61:
            sid, num = struct.unpack_from("<HH", t)
            frames[sid].append((num, t[4:]))


def h263_key(payload):
    # Sorenson H.263: 17-bit start code, 5 bit version, 8 bit temporal ref, 3 bit size, [8+8 custom size], 2 bit type
    bits = int.from_bytes(payload[:8].ljust(8, b"\0"), "big")
    pos = 17 + 5 + 8
    size = (bits >> (64 - pos - 3)) & 7
    pos += 3
    if size == 0:
        pos += 8
    elif size == 1:
        pos += 16
    return ((bits >> (64 - pos - 2)) & 3) == 0


def main():
    d = Path(sys.argv[1]).read_bytes()
    defs, frames = {}, defaultdict(list)
    walk(d, swfdump.body(d), len(d), defs, frames)
    sid = int(sys.argv[3]) if len(sys.argv) > 3 else max(frames, key=lambda k: len(frames[k]))
    fps = float(sys.argv[4]) if len(sys.argv) > 4 else 30.0
    n, w, h, codec = defs[sid]
    fr = sorted(frames[sid])
    print("stream %d: %dx%d codec %d, %d frames declared, %d present" % (sid, w, h, codec, n, len(fr)))
    out = bytearray(b"FLV\x01\x01" + struct.pack(">I", 9) + struct.pack(">I", 0))
    last = 0
    for num, data in fr:
        key = h263_key(data) if codec == 2 else True
        body = bytes([(1 if key else 2) << 4 | codec]) + data
        ts = int(num * 1000 / fps)
        out += bytes([9]) + struct.pack(">I", len(body))[1:] + struct.pack(">I", ts)[1:] + bytes([ts >> 24 & 0xFF]) + b"\0\0\0" + body
        out += struct.pack(">I", 11 + len(body))
        last = ts
    Path(sys.argv[2]).write_bytes(out)
    print("%s: %d bytes, %.1f s" % (sys.argv[2], len(out), last / 1000))


if __name__ == "__main__":
    main()
