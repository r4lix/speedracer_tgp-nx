#!/usr/bin/env python3
"""Convert the chunks ProjectorRays dumps from a Director movie into usable files.

  snd  -> WAV            BITD -> PNG
  ediM -> JPEG as stored, plus <name>.alpha.png when the member has an ALFA chunk

Usage: dirassets.py <chunks dir> <out dir>
"""
import os, re, struct, sys, wave, zlib


def cast_members(chunks):
    """Return {cast chunk id: {"name", "type", "specific", chunk fourcc: path}} using the KEY_ table."""
    files = {}
    for f in os.listdir(chunks):
        m = re.match(r"(.{4})-(\d+)\.bin$", f)
        if m:
            files[int(m.group(2))] = (m.group(1), os.path.join(chunks, f))
    key = open(next(p for t, p in files.values() if t == "KEY_"), "rb").read()
    entry_size, _, _, used = struct.unpack_from("<HHII", key, 0)
    members = {}
    for i in range(used):
        section, owner, fourcc = struct.unpack_from("<II4s", key, 12 + i * entry_size)
        if owner in files and files[owner][0] == "CASt" and section in files:
            members.setdefault(owner, {})[fourcc[::-1].decode("latin-1")] = files[section][1]
    for owner, m in members.items():
        cast = open(files[owner][1], "rb").read()
        m["type"], info_len, specific_len = struct.unpack_from(">III", cast, 0)
        m["specific"] = cast[12 + info_len:12 + info_len + specific_len]
        m["name"] = _cast_name(cast[12:12 + info_len]) or f"member{owner}"
    return members


def _cast_name(info):
    if len(info) < 6:
        return ""
    table = struct.unpack_from(">I", info, 0)[0]
    count = struct.unpack_from(">H", info, table)[0]
    offsets = struct.unpack_from(f">{count + 1}I", info, table + 2)
    items = table + 2 + 4 * (count + 1)
    if count < 2 or offsets[2] <= offsets[1]:
        return ""
    raw = info[items + offsets[1]:items + offsets[2]]
    return raw[1:1 + raw[0]].decode("latin-1")


def snd_to_wav(data, path):
    """Mac 'snd ' resource (format 1 or 2, standard or extended header) -> WAV."""
    fmt = struct.unpack_from(">H", data, 0)[0]
    pos = 4 if fmt == 2 else 4 + 6 * struct.unpack_from(">H", data, 2)[0]
    num_cmds = struct.unpack_from(">H", data, pos)[0]
    header = None
    for i in range(num_cmds):
        cmd, _, param = struct.unpack_from(">HHI", data, pos + 2 + 8 * i)
        if cmd & 0x7FFF in (80, 81):  # soundCmd / bufferCmd with the header offset
            header = param
    if header is None:
        raise ValueError("no sound header")
    _, length, rate, _, _, encode = struct.unpack_from(">IIIIIB", data, header)
    rate >>= 16
    if encode == 0xFF:
        channels, frames = length, struct.unpack_from(">I", data, header + 22)[0]
        bits = struct.unpack_from(">H", data, header + 48)[0]
        pcm = data[header + 64:header + 64 + frames * channels * bits // 8]
    elif encode == 0:
        channels, bits, pcm = 1, 8, data[header + 22:header + 22 + length]
    else:
        raise ValueError(f"compressed sound header {encode:#x}")
    if bits == 16:
        pcm = bytearray(pcm[:len(pcm) & ~1])
        pcm[0::2], pcm[1::2] = pcm[1::2], pcm[0::2]  # big endian -> little endian
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(bits // 8)
        w.setframerate(rate)
        w.writeframes(bytes(pcm))


def unpackbits(data, want):
    out, i = bytearray(), 0
    while i < len(data) and len(out) < want:
        n = data[i]
        i += 1
        if n < 128:
            out += data[i:i + n + 1]
            i += n + 1
        elif n > 128:
            out += data[i:i + 1] * (257 - n)
            i += 1
    return bytes(out)


def write_png(path, width, height, channels, pixels):
    """pixels: height rows of width*channels bytes (1 = gray, 4 = RGBA)."""
    stride = width * channels
    raw = b"".join(b"\0" + pixels[y * stride:(y + 1) * stride] for y in range(height))

    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, {1: 0, 4: 6}[channels], 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def bitmap_info(specific):
    pitch, top, left, bottom, right = struct.unpack_from(">Hhhhh", specific, 0)
    depth = specific[23] if len(specific) > 23 else 1
    return right - left, bottom - top, pitch & 0x7FFF, depth


def bitd_to_png(data, specific, path):
    width, height, pitch, depth = bitmap_info(specific)
    if depth != 32:
        raise ValueError(f"{depth}-bit bitmap not supported yet")
    out = bytearray(width * height * 4)
    if len(data) == pitch * height:  # stored raw as ARGB
        for y in range(height):
            row = data[y * pitch:y * pitch + width * 4]
            for c in range(3):
                out[y * width * 4 + c::4][:width] = row[c + 1::4]
            out[y * width * 4 + 3:(y + 1) * width * 4:4] = row[0::4]
    else:  # PackBits, each row stored as A, R, G, B planes
        raw = unpackbits(data, width * height * 4)
        for y in range(height):
            row = raw[y * width * 4:(y + 1) * width * 4]
            for c, plane in ((3, 0), (0, 1), (1, 2), (2, 3)):
                out[y * width * 4 + c:(y + 1) * width * 4:4] = row[plane * width:(plane + 1) * width]
    write_png(path, width, height, 4, bytes(out))


def main(chunks, out):
    counts = {"wav": 0, "png": 0, "jpg": 0, "skipped": 0}
    used = set()
    for owner, m in sorted(cast_members(chunks).items()):
        name = re.sub(r"[^\w.-]+", "_", m["name"])
        while name.lower() in used:
            name += f"_{owner}"
        used.add(name.lower())
        try:
            if "snd " in m:
                os.makedirs(f"{out}/sounds", exist_ok=True)
                snd_to_wav(open(m["snd "], "rb").read(), f"{out}/sounds/{name}.wav")
                counts["wav"] += 1
            elif "ediM" in m and m["type"] == 1:
                os.makedirs(f"{out}/bitmaps", exist_ok=True)
                with open(f"{out}/bitmaps/{name}.jpg", "wb") as f:
                    f.write(open(m["ediM"], "rb").read())
                counts["jpg"] += 1
                if "ALFA" in m:
                    width, height, _, _ = bitmap_info(m["specific"])
                    alpha = open(m["ALFA"], "rb").read()
                    if len(alpha) != width * height:
                        alpha = unpackbits(alpha, width * height)
                    write_png(f"{out}/bitmaps/{name}.alpha.png", width, height, 1, alpha)
            elif "BITD" in m:
                os.makedirs(f"{out}/bitmaps", exist_ok=True)
                bitd_to_png(open(m["BITD"], "rb").read(), m["specific"], f"{out}/bitmaps/{name}.png")
                counts["png"] += 1
        except Exception as e:
            counts["skipped"] += 1
            print(f"skipped {m['name']} ({owner}): {e}")
    print(f"{out}: " + ", ".join(f"{v} {k}" for k, v in counts.items()))


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
