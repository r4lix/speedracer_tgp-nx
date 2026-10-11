"""Sound of a Flash movie's main timeline, for a video made from its frames.

Usage: python tools/swfaudio.py <XMED-nnnn.bin or .swf> <out dir>
Writes stream.mp3 (the SoundStreamBlock data, which plays from frame 1) and event_<frame>_<id>.mp3 for every
StartSound on the main timeline, and prints "<file> <start seconds>" lines. MP3 only (sound format 2).
"""
import struct
import sys
from pathlib import Path

import swfdump


def main():
    d = Path(sys.argv[1]).read_bytes()
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    i = d.find(b"FWS")
    p = swfdump.body(d)
    fps = d[p - 3] + d[p - 4] / 256
    sounds, stream, frame = {}, bytearray(), 0
    for code, t in swfdump.tags(d, p, len(d)):
        if code == 14 and t[2] >> 4 == 2:
            sounds[struct.unpack_from("<H", t)[0]] = t[9:]  # after id, flags, sample count and the MP3 seek word
        elif code == 19:
            stream += t[4:]  # after the block's sample count and seek word
        elif code == 15:
            sid = struct.unpack_from("<H", t)[0]
            if sid in sounds:
                f = out / ("event_%03d_%d.mp3" % (frame + 1, sid))
                f.write_bytes(sounds[sid])
                print(f.name, "%.4f" % (frame / fps))
        elif code == 1:
            frame += 1
    if stream:
        (out / "stream.mp3").write_bytes(stream)
        print("stream.mp3 0")
    print("# %d frames at %g fps" % (frame, fps))


if __name__ == "__main__":
    main()
