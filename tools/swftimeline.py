"""Timeline summary of a Flash movie embedded in a Director XMED chunk: per frame the labels, actions,
event sounds and stream sound, for the main timeline and each sprite. Also writes the bare .swf.

Usage: python tools/swftimeline.py <XMED-nnnn.bin> [out.swf]
"""
import struct
import sys
from pathlib import Path

import swfdump

ACTIONS = {0x06: "play", 0x07: "stop", 0x04: "nextFrame", 0x05: "prevFrame", 0x09: "stopSounds"}


def actions(b):
    out, p = [], 0
    while p < len(b):
        c = b[p]
        p += 1
        ln = 0
        if c >= 0x80:
            ln = struct.unpack_from("<H", b, p)[0]
            p += 2
        if c == 0:
            break
        if c in ACTIONS:
            out.append(ACTIONS[c])
        elif c == 0x81:
            out.append("gotoFrame %d" % struct.unpack_from("<H", b, p)[0])
        elif c == 0x8C:
            out.append("gotoLabel %s" % b[p:p + ln - 1].decode("latin-1"))
        elif c == 0x83:
            out.append("getURL %s" % b[p:p + ln].replace(b"\0", b" ").decode("latin-1"))
        elif c == 0x9F:
            out.append("gotoFrame2")
        elif c == 0x96:
            s = b[p:p + ln]
            if s[:1] == b"\0":
                out.append("push '%s'" % s[1:].split(b"\0")[0].decode("latin-1"))
        p += ln
    return out


def walk(d, p, end, name, sounds):
    frame, line = 0, []
    for code, t in swfdump.tags(d, p, end):
        if code == 14:
            sid, fl, n = struct.unpack_from("<HBI", t)
            sounds[sid] = (fl >> 4, (5512, 11025, 22050, 44100)[(fl >> 2) & 3], 1 + (fl & 1), n, t[7:])
            print("  sound %d: format %d, %d Hz, %d ch, %d samples, %d bytes" % (sid, fl >> 4, sounds[sid][1], sounds[sid][2], n, len(t) - 7))
        elif code == 39:
            sid, n = struct.unpack_from("<HH", t)
            print(" sprite %d (%d frames)" % (sid, n))
            walk(t, 4, len(t), "sprite %d" % sid, sounds)
        elif code == 43:
            line.append("label '%s'" % t.split(b"\0")[0].decode("latin-1"))
        elif code == 12:
            a = actions(t)
            if a:
                line.append("do[%s]" % "; ".join(a))
        elif code == 15:
            line.append("startSound %d (flags %02x)" % (struct.unpack_from("<H", t)[0], t[2]))
        elif code in (18, 45):
            fl = t[1]
            line.append("streamHead fmt %d %d Hz %d ch, %d samples/frame" % (fl >> 4, (5512, 11025, 22050, 44100)[(fl >> 2) & 3], 1 + (fl & 1), struct.unpack_from("<H", t, 2)[0]))
        elif code == 19:
            line.append("stream")
        elif code == 26:
            fl = t[0]
            q = 3
            if fl & 2:
                cid = struct.unpack_from("<H", t, 3)[0]
                line.append("place %d@%d" % (cid, struct.unpack_from("<H", t, 1)[0]))
        elif code == 1:
            frame += 1
            keep = [x for x in line if x != "stream" and not x.startswith("place")]
            if keep:
                print("  %s frame %d: %s%s" % (name, frame, ", ".join(keep), " +stream" if "stream" in line else ""))
            line = []
    return frame


def main():
    d = Path(sys.argv[1]).read_bytes()
    i = d.find(b"FWS")
    if len(sys.argv) > 2:
        Path(sys.argv[2]).write_bytes(d[i:i + struct.unpack_from("<I", d, i + 4)[0]])
    walk(d, swfdump.body(d), len(d), "main", {})


if __name__ == "__main__":
    main()
