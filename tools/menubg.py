"""Build the main page's background loop as an FLV the port's video player already handles.

The big panel under the title ("THE GREAT PLAN": Speed in the Mach 5, speed lines, the rival car) is an animation
in the menu movie (XMED-5196, sprite 106, 330 frames at 30 fps, no sound) that the original loops behind the
buttons. Its frames come from Ruffle through tools/swfcapture: make a copy of the movie without its sound and
video frames (`python tools/swfstrip.py XMED-5196.bin local/cutscene/M5196v.swf --video`; with the preview video in,
Ruffle's playback froze 143 frames in) and run the capture page with
  swf=M5196v  from=11  n=345  fps=30  w=960  h=720
(the movie sits on its 'welcome' frame from frame 10 on, the sprite keeps looping, and frame 341 equals frame 11),
which leaves local/cutscene/frames/M5196v/0011.png ... 0345.png. This script cuts the panel out of 11..340 and writes
local/cutscene/out/menu_bg.flv at the stage's own 512x181 resolution; bake.py copies it into romfs.

Needs ffmpeg: `pip install imageio-ffmpeg` (or ffmpeg on PATH).
"""
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
FRAMES = ROOT / "local" / "cutscene" / "frames" / "M5196v"
OUT = ROOT / "local" / "cutscene" / "out" / "menu_bg.flv"
FIRST, COUNT = 11, 330
SCALE = 960 / 512            # capture pixels per stage pixel
TOP, BOTTOM = 135, 316       # panel rows in the stage (the title art above and the HELP strip below stay a still)
W, H = 512, BOTTOM - TOP


def ffmpeg():
    exe = shutil.which("ffmpeg")
    if exe:
        return exe
    import imageio_ffmpeg
    return imageio_ffmpeg.get_ffmpeg_exe()


def main():
    box = (0, round(TOP * SCALE), 960, round(BOTTOM * SCALE))
    with tempfile.TemporaryDirectory() as tmp:
        for i in range(COUNT):
            src = FRAMES / ("%04d.png" % (FIRST + i))
            im = Image.open(src).convert("RGB").crop(box).resize((W, H), Image.LANCZOS)
            im.save(Path(tmp) / ("%04d.png" % i))
        OUT.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([ffmpeg(), "-y", "-loglevel", "error", "-framerate", "30", "-i", str(Path(tmp) / "%04d.png"),
                        "-c:v", "flv", "-q:v", "3", "-g", "30", "-an", str(OUT)], check=True)
    print("%s: %d frames, %d KB" % (OUT, COUNT, OUT.stat().st_size // 1024))


if __name__ == "__main__":
    sys.exit(main())
