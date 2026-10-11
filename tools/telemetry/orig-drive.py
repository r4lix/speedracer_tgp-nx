"""Drive the original game along the track with real key presses, reading the car's position from its memory.

Usage: python orig-drive.py <out.csv> [seconds] [startpath]
Starts SR_GAME.exe from ports/.../local/gamerun (patched launcher), clicks PLAY, finds the car's position in memory
(three floats near the start line that start moving), then follows the baked path points with Left/Right/Up.
Writes t, x, y, z every ~30 ms. The window is brought to the front once and the keys go to whatever has focus,
so leave the PC alone while it runs.
"""
import ctypes
import json
import math
import os
import struct
import subprocess
import sys
import time

# Lives in <repo>/tools/telemetry; run-original.py and win-click.ps1 are in switch/tools (see NOTES.md) and are
# looked up there. Needs <repo>/local/path_points.json (path_NN markers of the level, one [x, y, z] each).
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
TOOLS = os.environ.get("SR_TOOLS", os.path.abspath(os.path.join(ROOT, "..", "..", "..", "tools")))
user32 = ctypes.WinDLL("user32")
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
sys.path.insert(0, HERE)
import floatscan  # noqa: E402

VK = {"Up": 0x26, "Left": 0x25, "Right": 0x27}


def key(name, down):
    vk = VK[name]
    user32.keybd_event(vk, user32.MapVirtualKeyW(vk, 0), 1 | (0 if down else 2), 0)


def ps(script):
    return subprocess.run(["powershell", "-NoProfile", "-File", script], capture_output=True, text=True).stdout


def main():
    out = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 60
    pts = json.load(open(os.path.join(ROOT, "local", "path_points.json")))
    path = [pts[str(i)] for i in range(1, len(pts) + 1)]

    if os.environ.get("ATTACH"):  # the game is already on its main page
        print(ps(os.path.join(HERE, "click-play.ps1")).strip())
        time.sleep(12)
    else:
        subprocess.run(["powershell", "-NoProfile", "-Command", "Get-Process SR_GAME -ErrorAction SilentlyContinue | ForEach-Object { try { $_.Kill() } catch {} }"])
        time.sleep(2)
        game = os.path.abspath(os.path.join(ROOT, "local", "gamerun"))
        print(subprocess.run([sys.executable, os.path.join(TOOLS, "run-original.py"), game], capture_output=True, text=True).stdout.strip())
        time.sleep(27)
        print(ps(os.path.join(HERE, "click-play.ps1")).strip())
        time.sleep(12)

    pid = floatscan.pid_of("SR_GAME")
    h = floatscan.k32.OpenProcess(0x0410, False, pid)
    cands = floatscan.scan(h, 42.0, 38.8, 14.0, 0.6)
    print(len(cands), "candidates")
    buf = ctypes.create_string_buffer(12)
    got = ctypes.c_size_t()

    def read(addr):
        if floatscan.k32.ReadProcessMemory(h, addr, buf, 12, ctypes.byref(got)):
            return struct.unpack("<fff", buf.raw)
        return None

    # the live copy is the one whose y grows once the throttle goes down
    before = {a: read(a) for a, _, _, _ in cands}
    key("Up", True)
    time.sleep(1.2)
    snap1 = {a: read(a) for a, _, _, _ in cands}
    time.sleep(0.4)
    addr = None
    for a, _, _, _ in cands:
        v1, v2, v0 = snap1.get(a), read(a), before.get(a)
        if not (v0 and v1 and v2):
            continue
        if all(abs(c) < 1e5 for c in v2) and v1[1] - v0[1] > 15 and 8 < v2[1] - v1[1] < 40 and abs(v2[2] - 13.8) < 0.7 and abs(v2[0] - 42) < 40:
            addr = a
            break
    if addr is None:
        key("Up", False)
        sys.exit("car position not found")
    print("car at %08x" % addr)

    t0 = time.perf_counter()
    hist = []
    idx = 0
    steer = None
    throttle = True
    f = open(out, "w")

    def aim(x, y, look):
        """Point `look` units ahead along the path polyline, starting from the segment nearest to (x, y)."""
        nonlocal idx
        best, bi = 1e18, idx
        for i in range(max(0, idx - 2), min(len(path) - 1, idx + 6)):
            ax, ay = path[i][0], path[i][1]
            bx, by = path[i + 1][0], path[i + 1][1]
            dx, dy = bx - ax, by - ay
            u = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
            d = math.hypot(ax + u * dx - x, ay + u * dy - y)
            if d < best:
                best, bi, bu = d, i, u
        idx = bi
        i, u = bi, bu
        left = look
        px = path[i][0] + u * (path[i + 1][0] - path[i][0])
        py = path[i][1] + u * (path[i + 1][1] - path[i][1])
        while i < len(path) - 2:
            seg = math.hypot(path[i + 1][0] - px, path[i + 1][1] - py)
            if seg >= left:
                k = left / seg
                return px + (path[i + 1][0] - px) * k, py + (path[i + 1][1] - py) * k, best
            left -= seg
            i += 1
            px, py = path[i][0], path[i][1]
        return path[-1][0], path[-1][1], best

    while True:
        t = time.perf_counter() - t0
        v = read(addr)
        if v is None or t > seconds:
            break
        x, y, z = v
        hist.append((t, x, y, z))
        f.write("%.3f,%.2f,%.2f,%.3f" % (t, x, y, z) + chr(10))
        ref = next((p for p in reversed(hist) if t - p[0] > 0.15), None)
        if ref and math.hypot(x - ref[1], y - ref[2]) > 0.3:
            speed = math.hypot(x - ref[1], y - ref[2]) / (t - ref[0])
            psi = math.atan2(x - ref[1], y - ref[2])
            tx, ty, off = aim(x, y, 14 + speed * 0.3)
            want = math.atan2(tx - x, ty - y)
            err = (want - psi + math.pi) % (2 * math.pi) - math.pi
            new = "Right" if err > 0.04 else "Left" if err < -0.04 else None
            if new != steer:
                if steer:
                    key(steer, False)
                if new:
                    key(new, True)
                steer = new
            # keep the speed near 45 u/s: the walls are close and the original reacts fast
            cap = 52 if y < 1520 else 140  # flat out down the long descent
            want_throttle = speed < cap and abs(err) < 0.35
            if want_throttle != throttle:
                key("Up", want_throttle)
                throttle = want_throttle
        if t > 4 and len(hist) > 60 and math.hypot(x - hist[-60][1], y - hist[-60][2]) < 4:
            print("stuck at", x, y)
            break
        if y > 2100:
            break
        time.sleep(0.02)
    for k in ("Up", "Left", "Right"):
        key(k, False)
    f.close()
    print("done, last y %.0f" % hist[-1][2])


if __name__ == "__main__":
    main()
