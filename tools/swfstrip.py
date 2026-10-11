"""Write a Flash movie out of a Director XMED chunk as a bare .swf without its streaming sound.

Usage: python tools/swfstrip.py <XMED-nnnn.bin> <out.swf>
Ruffle ties the timeline to a streaming sound's playback position, which defeats frame-by-frame capture;
without the SoundStreamHead/SoundStreamBlock tags the movie simply advances one frame per frame.
"""
import struct
import sys
from pathlib import Path

import swfdump


def main():
    d = Path(sys.argv[1]).read_bytes()
    i = d.find(b"FWS")
    p = swfdump.body(d)
    out = bytearray(d[i:p])
    dropped = 0
    for code, t in swfdump.tags(d, p, len(d)):
        if code in (18, 19, 45):
            dropped += 1
            continue
        out += struct.pack("<HI", code << 6 | 0x3F, len(t)) + t  # long form is always valid
    struct.pack_into("<I", out, 4, len(out))
    Path(sys.argv[2]).write_bytes(out)
    print("%s: %d bytes, %d stream tags dropped" % (sys.argv[2], len(out), dropped))


if __name__ == "__main__":
    main()
