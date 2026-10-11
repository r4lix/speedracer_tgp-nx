"""Find three consecutive floats (x, y, z) near given values in a running process, and sample them.

Usage:
  python floatscan.py <process> scan  <x> <y> <z> [tol]        -> prints candidate addresses
  python floatscan.py <process> watch <addr> <seconds> [hz]    -> prints t, x, y, z (hex address from scan)
Read-only (PROCESS_VM_READ). 32-bit processes only (addresses below 2 GB).
"""
import ctypes
import ctypes.wintypes as wt
import struct
import subprocess
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_ulonglong), ("AllocationBase", ctypes.c_ulonglong), ("AllocationProtect", wt.DWORD),
                ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_ulonglong), ("State", wt.DWORD), ("Protect", wt.DWORD),
                ("Type", wt.DWORD)]


k32.OpenProcess.restype = wt.HANDLE
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_ulonglong, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


def pid_of(name):
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq %s.exe" % name, "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) > 1 and parts[0].strip('"').lower() == name.lower() + ".exe":
            return int(parts[1])
    sys.exit("process %s not found" % name)


def scan(h, x, y, z, tol=0.3):
    addr, mbi, hits = 0, MBI(), []
    while addr < 0x7FFF0000 and k32.VirtualQueryEx(h, addr, ctypes.byref(mbi), ctypes.sizeof(mbi)):
        size = mbi.RegionSize
        if mbi.State == 0x1000 and not (mbi.Protect & 0x101) and size <= 512 * 1024 * 1024:
            buf = ctypes.create_string_buffer(size)
            got = ctypes.c_size_t()
            if k32.ReadProcessMemory(h, addr, buf, size, ctypes.byref(got)):
                d = buf.raw[:got.value]
                for off in range(0, len(d) - 12, 4):
                    a = struct.unpack_from("<f", d, off)[0]
                    if abs(a - x) > tol:
                        continue
                    b, c = struct.unpack_from("<ff", d, off + 4)
                    if abs(b - y) <= tol and abs(c - z) <= tol:
                        hits.append((addr + off, a, b, c))
        addr += size
    return hits


def main():
    h = k32.OpenProcess(0x0410, False, pid_of(sys.argv[1]))  # query + read
    if not h:
        sys.exit("cannot open process")
    if sys.argv[2] == "scan":
        x, y, z = (float(v) for v in sys.argv[3:6])
        tol = float(sys.argv[6]) if len(sys.argv) > 6 else 0.3
        addr, mbi, hits = 0, MBI(), []
        while addr < 0x7FFF0000 and k32.VirtualQueryEx(h, addr, ctypes.byref(mbi), ctypes.sizeof(mbi)):
            size = mbi.RegionSize
            if mbi.State == 0x1000 and not (mbi.Protect & 0x101) and size <= 512 * 1024 * 1024:
                buf = ctypes.create_string_buffer(size)
                got = ctypes.c_size_t()
                if k32.ReadProcessMemory(h, addr, buf, size, ctypes.byref(got)):
                    d = buf.raw[:got.value]
                    # candidates: float x at 4-byte alignment, then y, z immediately after
                    for off in range(0, len(d) - 12, 4):
                        a = struct.unpack_from("<f", d, off)[0]
                        if abs(a - x) > tol:
                            continue
                        b, c = struct.unpack_from("<ff", d, off + 4)
                        if abs(b - y) <= tol and abs(c - z) <= tol:
                            hits.append((addr + off, a, b, c))
            addr += size
        for a in hits:
            print("%08x  %.3f %.3f %.3f" % a)
        print(len(hits), "hit(s)")
    else:
        addrs = [int(v, 16) for v in sys.argv[3].split(",")]
        secs = float(sys.argv[4])
        hz = float(sys.argv[5]) if len(sys.argv) > 5 else 30
        buf = ctypes.create_string_buffer(12)
        got = ctypes.c_size_t()
        t0 = time.perf_counter()
        nxt = t0
        while time.perf_counter() - t0 < secs:
            row = ["%.3f" % (time.perf_counter() - t0)]
            for a in addrs:
                if k32.ReadProcessMemory(h, a, buf, 12, ctypes.byref(got)):
                    row += ["%.3f %.3f %.3f" % struct.unpack("<fff", buf.raw)]
                else:
                    row += ["?"]
            print(" | ".join(row), flush=True)
            nxt += 1.0 / hz
            time.sleep(max(0, nxt - time.perf_counter()))


if __name__ == "__main__":
    main()
