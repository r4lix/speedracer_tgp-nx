"""Build the result screens' opening cartoons as FLV clips the port's video player already handles.

The three result movies (Flash, inside SR_MAIN) open on a short animation. Their frames come from Ruffle,
stepped one movie frame at a time by tools/swfcapture (capture.html + server.py) into
local/cutscene/frames/<chunk>/NNNN.png; the sound comes straight out of the same movies (swfaudio.py).
This script cuts each to the animation, mixes the sound and writes local/cutscene/out/result_*.flv plus
music_result.wav, the loop the pages play afterwards. bake.py copies them into romfs.

Needs ffmpeg: `pip install imageio-ffmpeg` (or ffmpeg on PATH).
"""
import shutil
import struct
import subprocess
import sys
import wave
from pathlib import Path

import swfdump

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "local" / "cutscene"
CHUNKS = ROOT / "local" / "out" / "main" / "SR_MAIN" / "chunks"
# chunk -> (clip name, frames folder, last frame before the result page appears, movie frame rate).
# The two with streaming sound are captured from a copy without it (swfstrip.py), hence the other folder.
MOVIES = {"5198": ("result_lose", "5198n", 136, 30), "5204": ("result_die", "5204n", 115, 30),
          "5200": ("result_win", "5200", 156, 31)}


def ffmpeg():
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    import imageio_ffmpeg
    return imageio_ffmpeg.get_ffmpeg_exe()


def sounds(chunk, fps, last):
    """-> [(mp3 path, start seconds)] for what is heard during frames 1..last, and the page's music loop."""
    d = (CHUNKS / ("XMED-%s.bin" % chunk)).read_bytes()
    out = WORK / "audio" / chunk
    out.mkdir(parents=True, exist_ok=True)
    defs, stream, frame, heard, music, stream_from = {}, bytearray(), 0, [], None, 0
    for code, t in swfdump.tags(d, swfdump.body(d), len(d)):
        if code == 14 and t[2] >> 4 == 2:
            sid, _, count, seek = struct.unpack_from("<HBIh", t)
            defs[sid] = (t[9:], count, seek, (5512, 11025, 22050, 44100)[(t[2] >> 2) & 3])
        elif code == 19 and frame < last:
            if not stream:
                stream_from = frame  # the stream starts on the frame of its first block
            stream += t[4:]
        elif code == 15:
            sid, flags = struct.unpack_from("<HB", t)
            if sid in defs:
                f = out / ("event_%d.mp3" % sid)
                f.write_bytes(defs[sid][0])
                if flags & 4:  # looping: the page's music, not part of the animation
                    music = (f,) + defs[sid][1:]
                elif frame < last:
                    heard.append((f, frame / fps))
        elif code == 1:
            frame += 1
    if stream:
        (out / "stream.mp3").write_bytes(stream)
        heard.append((out / "stream.mp3", stream_from / fps))
    return heard, music


def main():
    exe = ffmpeg()
    out = WORK / "out"
    out.mkdir(parents=True, exist_ok=True)
    music_done = False
    for chunk, (name, folder, last, fps) in MOVIES.items():
        frames = WORK / "frames" / folder
        if not (frames / ("%04d.png" % last)).exists():
            print("%s: frames missing in %s, skipped" % (name, frames))
            continue
        heard, music = sounds(chunk, fps, last)
        cmd = [exe, "-y", "-loglevel", "error", "-framerate", str(fps), "-i", str(frames / "%04d.png")]
        for f, _ in heard:
            cmd += ["-i", str(f)]
        length = last / fps
        if heard:
            parts = ["[%d:a]adelay=%d,apad[a%d]" % (i + 1, round(t * 1000), i) for i, (_, t) in enumerate(heard)]
            mix = "".join("[a%d]" % i for i in range(len(heard)))
            parts.append("%samix=inputs=%d:normalize=0,atrim=0:%.3f[a]" % (mix, len(heard), length))
            cmd += ["-filter_complex", ";".join(parts), "-map", "0:v", "-map", "[a]", "-c:a", "libmp3lame", "-ar", "22050", "-ac", "1", "-b:a", "64k"]
        cmd += ["-frames:v", str(last), "-c:v", "flv", "-q:v", "3", "-g", "30", str(out / (name + ".flv"))]
        subprocess.run(cmd, check=True)
        print("%s.flv: %d frames, %.1f s, %d sounds, %d KB" % (name, last, length, len(heard), (out / (name + ".flv")).stat().st_size // 1024))
        if music and not music_done and music[3] >= 22050:
            # decode, then drop the encoder delay Flash skips and keep exactly the stated length, so it loops cleanly
            tmp = out / "music_raw.wav"
            subprocess.run([exe, "-y", "-loglevel", "error", "-i", str(music[0]), "-ac", "1", "-c:a", "pcm_s16le", str(tmp)], check=True)
            with wave.open(str(tmp)) as w:
                rate, pcm = w.getframerate(), w.readframes(w.getnframes())
            pcm = pcm[2 * max(0, music[2]):][:2 * music[1]]
            with wave.open(str(out / "music_result.wav"), "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(rate)
                w.writeframes(pcm)
            tmp.unlink()
            music_done = True
            print("music_result.wav: %.1f s at %d Hz" % (len(pcm) / 2 / rate, rate))


if __name__ == "__main__":
    sys.exit(main())
