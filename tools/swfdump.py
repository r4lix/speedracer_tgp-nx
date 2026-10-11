"""List the tags of the Flash movies embedded in a Director movie (XMED chunks starting with a
12-byte header then "FWS"), and optionally pull out their bitmaps and ActionScript strings.

Usage: python tools/swfdump.py <XMED-nnnn.bin> [--images out_dir] [--strings]
JPEG tags are written as .jpg (with the alpha plane, if any, as raw .alpha: u32 w, u32 h, bytes);
lossless tags as raw .rgba (u32 w, u32 h, RGBA bytes).
"""
import struct
import sys
import zlib
from pathlib import Path

NAMES = {0: "End", 1: "ShowFrame", 2: "DefineShape", 6: "DefineBits", 8: "JPEGTables", 9: "SetBackgroundColor",
         10: "DefineFont", 11: "DefineText", 12: "DoAction", 14: "DefineSound", 20: "DefineBitsLossless",
         21: "DefineBitsJPEG2", 22: "DefineShape2", 26: "PlaceObject2", 28: "RemoveObject2", 32: "DefineShape3",
         33: "DefineText2", 34: "DefineButton2", 35: "DefineBitsJPEG3", 36: "DefineBitsLossless2", 37: "DefineEditText",
         39: "DefineSprite", 43: "FrameLabel", 45: "SoundStreamHead2", 46: "DefineMorphShape", 48: "DefineFont2",
         56: "ExportAssets", 59: "DoInitAction", 60: "DefineVideoStream", 19: "SoundStreamBlock", 15: "StartSound"}


def tags(d, p, end):
    while p < end:
        code_len = struct.unpack_from("<H", d, p)[0]
        code, ln = code_len >> 6, code_len & 0x3F
        p += 2
        if ln == 0x3F:
            ln = struct.unpack_from("<I", d, p)[0]
            p += 4
        yield code, d[p:p + ln]
        p += ln
        if code == 0:
            return


def body(d):
    i = d.find(b"FWS")
    nbits = d[i + 8] >> 3
    return i + 8 + (5 + nbits * 4 + 7) // 8 + 4


def jpeg_fix(j):
    # Flash JPEGs may start with an erroneous EOI+SOI pair, and keep the tables in a separate stream.
    if j[:4] == b"\xff\xd9\xff\xd8":
        j = j[4:]
    return j.replace(b"\xff\xd9\xff\xd8", b"")


def join_tables(tables, image):
    """DefineBits keeps only the scan data; put the JPEGTables stream (minus its end marker) in front of it."""
    if len(tables) < 4:
        return image
    t = tables[:-2] if tables[-2:] == bytes((0xFF, 0xD9)) else tables
    return t + (image[2:] if image[:2] == bytes((0xFF, 0xD8)) else image)


def main():
    d = Path(sys.argv[1]).read_bytes()
    out = Path(sys.argv[sys.argv.index("--images") + 1]) if "--images" in sys.argv else None
    want_strings = "--strings" in sys.argv
    counts, strings = {}, []
    tables = [b""]  # JPEGTables: the shared encoding tables for DefineBits images
    if out:
        out.mkdir(parents=True, exist_ok=True)

    def walk(buf, p, end, depth):
        for code, t in tags(buf, p, end):
            counts[code] = counts.get(code, 0) + 1
            if code == 39:  # DefineSprite: id, frame count, then nested tags
                walk(t, 4, len(t), depth + 1)
            if code in (12, 59, 34) and want_strings:
                for s in t.split(b"\0"):
                    if len(s) >= 3 and all(32 <= c < 127 for c in s):
                        strings.append(s.decode())
            if code == 43:
                strings.append("LABEL " + t.split(b"\0")[0].decode("latin-1"))
            if code == 8:
                tables[0] = jpeg_fix(t)
            if not out:
                continue
            if code in (6, 21, 35):
                cid = struct.unpack_from("<H", t, 0)[0]
                if code == 35:
                    alen = struct.unpack_from("<I", t, 2)[0]
                    j, alpha = t[6:6 + alen], zlib.decompress(t[6 + alen:])
                else:
                    j, alpha = t[2:], None
                j = jpeg_fix(j)
                if code == 6:
                    j = join_tables(tables[0], j)
                (out / f"{cid}.jpg").write_bytes(j)
                if alpha:
                    s = j.find(b"\xff\xc0")
                    h, w = struct.unpack_from(">HH", j, s + 5)
                    (out / f"{cid}.alpha").write_bytes(struct.pack("<2I", w, h) + alpha)
            if code in (20, 36):
                cid, fmt, w, h = struct.unpack_from("<HBHH", t, 0)
                if fmt == 5:
                    raw = zlib.decompress(t[7:])
                    px = bytearray(w * h * 4)
                    for i in range(w * h):
                        a, r, g, b = raw[i * 4:i * 4 + 4]
                        if code == 20:
                            a = 255
                        elif a:  # stored premultiplied
                            r, g, b = min(255, r * 255 // a), min(255, g * 255 // a), min(255, b * 255 // a)
                        px[i * 4:i * 4 + 4] = bytes((r, g, b, a))
                    (out / f"{cid}.rgba").write_bytes(struct.pack("<2I", w, h) + px)

    walk(d, body(d), len(d), 0)
    print(Path(sys.argv[1]).name, {NAMES.get(k, k): v for k, v in sorted(counts.items())})
    for s in dict.fromkeys(strings):
        print("  ", s)


if __name__ == "__main__":
    main()
