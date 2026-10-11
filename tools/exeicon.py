"""Pull the largest icon out of a Windows .exe (PE resources) and write it as a 256x256 JPEG.

Usage: python tools/exeicon.py <game.exe> <out.jpg>
Handles the usual icon forms: DIB (BMP data with an AND mask) and embedded PNG.
"""
import io
import struct
import sys
from pathlib import Path

from PIL import Image


def read_resources(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    dd = opt + (96 if magic == 0x10B else 112)
    rva, size = struct.unpack_from("<II", d, dd + 16)
    secs = []
    sp = opt + struct.unpack_from("<H", d, pe + 20)[0]
    for i in range(nsec):
        vs, va, rs, ro = struct.unpack_from("<IIII", d, sp + 40 * i + 8)
        secs.append((va, max(vs, rs), ro))

    def off(r):
        for va, sz, ro in secs:
            if va <= r < va + sz:
                return r - va + ro
        raise ValueError("rva %x" % r)

    base = off(rva)

    def entries(o):
        named, ids = struct.unpack_from("<HH", d, o + 12)
        for i in range(named + ids):
            nm, dat = struct.unpack_from("<II", d, o + 16 + 8 * i)
            yield nm, dat

    def walk(o, depth, path, out):
        for nm, dat in entries(o):
            if dat & 0x80000000:
                walk(base + (dat & 0x7FFFFFFF), depth + 1, path + [nm], out)
            else:
                drva, dsize = struct.unpack_from("<II", d, base + dat)
                out[tuple(path + [nm])] = d[off(drva):off(drva) + dsize]

    out = {}
    walk(base, 0, [], out)
    return out


def main():
    d = Path(sys.argv[1]).read_bytes()
    res = read_resources(d)
    groups = {k: v for k, v in res.items() if k[0] == 14}  # RT_GROUP_ICON
    icons = {k[1]: v for k, v in res.items() if k[0] == 3}  # RT_ICON by id
    print("%d icon groups, %d icon images" % (len(groups), len(icons)))
    best = None
    for k, g in groups.items():
        _, _, n = struct.unpack_from("<HHH", g)
        for i in range(n):
            w, h, cols, _, planes, bpp, size, iid = struct.unpack_from("<BBBBHHIH", g, 6 + 14 * i)
            w, h = w or 256, h or 256
            print("  group %s: %dx%d %d bpp (icon %d)" % (k[1], w, h, bpp, iid))
            # largest first; at equal size prefer 8 bpp. The 48x48 32 bpp image in this game's exe is a
            # different, unrelated picture, so 32 bpp ranks last.
            rank = (w, 0 if bpp == 32 else bpp)
            if iid in icons and (best is None or rank > best[0]):
                best = (rank, icons[iid])
    if not best:
        sys.exit("no icon found")
    data = best[1]
    if data[:4] == b"\x89PNG":
        im = Image.open(io.BytesIO(data)).convert("RGBA")
    else:  # DIB: BITMAPINFOHEADER with doubled height, XOR bitmap then AND mask
        hdr = struct.unpack_from("<IiiHHIIiiII", data)
        w, h2, bpp = hdr[1], hdr[2], hdr[4]
        h = h2 // 2
        wrapped = bytearray(b"BM" + struct.pack("<IHHI", 0, 0, 0, 14 + hdr[0] + (4 * (hdr[9] or (1 << bpp) if bpp <= 8 else 0))))
        bi = bytearray(data[:hdr[0]])
        struct.pack_into("<i", bi, 8, h)  # real height
        pal = (hdr[9] or (1 << bpp)) * 4 if bpp <= 8 else 0
        xor_size = ((w * bpp + 31) // 32) * 4 * h
        body = data[hdr[0]:hdr[0] + pal + xor_size]
        bmp = bytes(wrapped[:2]) + struct.pack("<IHHI", 14 + len(bi) + len(body), 0, 0, 14 + len(bi) + pal) + bytes(bi) + body
        im = Image.open(io.BytesIO(bmp)).convert("RGBA")
        if bpp < 32:  # transparency from the AND mask
            mstride = ((w + 31) // 32) * 4
            mask = data[hdr[0] + pal + xor_size:]
            px = im.load()
            for y in range(h):
                row = mask[(h - 1 - y) * mstride:(h - y) * mstride]
                for x in range(w):
                    if row[x >> 3] & (0x80 >> (x & 7)):
                        px[x, y] = px[x, y][:3] + (0,)
    print("using %dx%d" % im.size)
    # a round badge: Switch icons are square JPEGs, so it sits on the game's dark red
    big = im.resize((256, 256), Image.LANCZOS)
    bg = Image.new("RGBA", big.size, (58, 6, 8, 255))
    bg.alpha_composite(big)
    bg.convert("RGB").save(sys.argv[2], quality=95)


if __name__ == "__main__":
    main()
