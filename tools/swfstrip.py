"""Write a Flash movie out of a Director XMED chunk as a bare .swf without its streaming sound.

Usage: python tools/swfstrip.py <XMED-nnnn.bin> <out.swf> [--video]
Ruffle ties the timeline to a streaming sound's playback position, which defeats frame-by-frame capture;
without the SoundStreamHead/SoundStreamBlock tags the movie simply advances one frame per frame.
"""
import struct
import sys
from pathlib import Path

import swfdump


def pack(code, t):
    return struct.pack("<HI", code << 6 | 0x3F, len(t)) + t  # long form is always valid


def strip(d, p, end, drop):
    """The tags d[p:end] without the dropped ones, recursing into sprites. Returns (bytes, count)."""
    out = bytearray()
    dropped = 0
    for code, t in swfdump.tags(d, p, end):
        if code in drop:
            dropped += 1
            continue
        if code == 39:  # DefineSprite: id, frame count, then its own tags
            inner, n = strip(t, 4, len(t), drop)
            dropped += n
            t = t[:4] + inner
        out += pack(code, t)
    return bytes(out), dropped


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    drop = {18, 19, 45}  # SoundStreamHead, SoundStreamBlock, SoundStreamHead2
    if "--video" in sys.argv:
        drop.add(61)     # VideoFrame: Ruffle stops decoding some Sorenson streams, and the frames are not needed
    d = Path(args[0]).read_bytes()
    i = d.find(b"FWS")
    p = swfdump.body(d)
    out = bytearray(d[i:p])
    body, dropped = strip(d, p, len(d), drop)
    out += body
    struct.pack_into("<I", out, 4, len(out))
    Path(args[1]).write_bytes(out)
    print("%s: %d bytes, %d stream tags dropped" % (args[1], len(out), dropped))


if __name__ == "__main__":
    main()
