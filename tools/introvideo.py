"""Build the startup intro as an FLV: the original's intro movie (a Flash movie inside the projector's own
Director movie, tools/swftimeline.py shows its sounds) rendered by Ruffle frame by frame, with its sounds
mixed in where its StartSound tags fire.

Frames: local/cutscene/frames/<swf name>/NNNN.png from tools/swfcapture. Output: local/cutscene/out/intro.flv
"""
import shutil
import struct
import subprocess
import sys
from pathlib import Path

import swfdump

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "local" / "cutscene"
CHUNK = ROOT / "local" / "out" / "stub" / "dump" / "inner_1671299" / "chunks" / "XMED-4412.bin"
NAME, FPS, LAST = "I4412", 30, 705


def ffmpeg():
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    import imageio_ffmpeg
    return imageio_ffmpeg.get_ffmpeg_exe()


def main():
    d = CHUNK.read_bytes()
    out = WORK / "audio" / "intro"
    out.mkdir(parents=True, exist_ok=True)
    defs, events, frame = {}, [], 0
    for code, t in swfdump.tags(d, swfdump.body(d), len(d)):
        if code == 14 and t[2] >> 4 == 2:
            sid = struct.unpack_from("<H", t)[0]
            f = out / ("s%d.mp3" % sid)
            f.write_bytes(t[9:])
            defs[sid] = (f, struct.unpack_from("<I", t, 3)[0], (5512, 11025, 22050, 44100)[(t[2] >> 2) & 3])
        elif code == 15:
            sid, fl = struct.unpack_from("<HB", t)
            q, loops, vol = 3, 1, 1.0
            if fl & 1: q += 4
            if fl & 2: q += 4
            if fl & 4:
                loops = struct.unpack_from("<H", t, q)[0]
                q += 2
            if fl & 8 and t[q] >= 1:  # envelope: first point's left volume, 0..32768
                vol = struct.unpack_from("<H", t, q + 5)[0] / 32768.0
            events.append((frame, sid, fl, loops, vol))
        elif code == 1:
            frame += 1
            if frame >= LAST:
                break
    length = LAST / FPS
    # a loop (32767 times) runs until the same sound is stopped (flag 0x20 "sync stop")
    plays = []  # (path, start s, duration s, volume)
    for i, (fr, sid, fl, loops, vol) in enumerate(events):
        if sid not in defs or fl & 0x20:
            continue
        path, samples, rate = defs[sid]
        own = samples / rate
        start = fr / FPS
        stop = length
        for fr2, sid2, fl2, _, _ in events[i + 1:]:
            if sid2 == sid and fl2 & 0x20:
                stop = fr2 / FPS
                break
        dur = min(own * loops, stop - start)
        if dur > 0.05 and start < length:
            plays.append((path, start, dur, vol))
    for p in plays:
        print("%s at %.2fs for %.2fs vol %.2f" % (p[0].name, p[1], p[2], p[3]))
    exe = ffmpeg()
    cmd = [exe, "-y", "-loglevel", "error", "-framerate", str(FPS), "-i", str(WORK / "frames" / NAME / "%04d.png")]
    parts = []
    for i, (path, start, dur, vol) in enumerate(plays):
        cmd += ["-stream_loop", "-1" if dur > 0 else "0", "-i", str(path)]
        parts.append("[%d:a]atrim=0:%.3f,asetpts=PTS-STARTPTS,volume=%.2f,adelay=%d,apad[a%d]" % (i + 1, dur, vol, round(start * 1000), i))
    mix = "".join("[a%d]" % i for i in range(len(plays)))
    parts.append("%samix=inputs=%d:normalize=0,apad,atrim=0:%.3f[a]" % (mix, len(plays), length + 1.5))
    dst = WORK / "out" / "intro.flv"
    dst.parent.mkdir(exist_ok=True)
    cmd += ["-filter_complex", ";".join(parts), "-map", "0:v", "-map", "[a]", "-vf", "scale=640:480:flags=lanczos,tpad=stop_mode=clone:stop_duration=1.5",
            "-c:a", "libmp3lame", "-ar", "22050", "-ac", "1", "-b:a", "64k", "-frames:v", str(LAST + 45),
            "-c:v", "flv", "-q:v", "4", "-g", "30", str(dst)]
    subprocess.run(cmd, check=True)
    print("%s: %.1f s, %d KB" % (dst, length, dst.stat().st_size // 1024))


if __name__ == "__main__":
    sys.exit(main())
