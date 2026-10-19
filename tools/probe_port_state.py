# -*- coding: utf-8 -*-
# probe_port_state.py - read the live MikuMikuStudio port's MMDApp camera /
# mouse state out of a running instance, optionally while posting a drag into
# its window, so viewport view operations can be verified without foreground
# activation.
#
# The port is built from the project's own sources, so its state struct is
# found by the same camera-track signature the reference probe uses.
import ctypes
import ctypes.wintypes as wt
import os
import struct
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding="utf-8")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_mmd_state import (k32, read, regions,  # noqa: E402
                             PROCESS_QUERY_INFORMATION, PROCESS_VM_READ)
from shot_window import find_window  # noqa: E402

u = ctypes.windll.user32
WM_MOUSEMOVE = 0x0200
WM_RBUTTONDOWN, WM_RBUTTONUP = 0x0204, 0x0205
WM_MBUTTONDOWN, WM_MBUTTONUP = 0x0207, 0x0208
MK_RBUTTON, MK_MBUTTON = 0x0002, 0x0010

# Port (x64) offsets, from include/mikumikustudio/app_layout.hpp pins.
FIELDS = [
    (0x2F8 + 0x30, "cameraMode(optflag0)", "byte"),
    (0x308 + 0x38, "viewOffsetX", "float"),
    (0x30C + 0x38, "viewOffsetY", "float"),
    (0x310 + 0x38, "cameraPitch", "float"),
    (0x314 + 0x38, "cameraYaw", "float"),
    (0x318 + 0x38, "cameraRoll", "float"),
    (0x334 + 0x38, "cameraPosX", "float"),
    (0x338 + 0x38, "cameraPosY", "float"),
    (0x33C + 0x38, "cameraPosZ", "float"),
    (0xA08DC + 0xA01C, "cameraDistance", "float"),
]
MOUSE_FIELDS = [
    (0x04, "mouseX", "int"),
    (0x08, "mouseY", "int"),
    (0x0C, "prevMouseX", "int"),
    (0x10, "prevMouseY", "int"),
]


def find_app_by_view(h):
    """Locate the port's app object by the default view block at +0x340:
    viewOffsetX=0, viewOffsetY=10, cameraPitch=0, cameraYaw=0, cameraRoll=0,
    cameraPosX=0, cameraPosY=10, cameraPosZ=0, cameraDistance=-45, fov=30.
    """
    def f(v):
        return struct.pack("<f", v)

    needle = (f(0.0) + f(10.0) + f(0.0) + f(0.0) + f(0.0) + f(0.0) +
              f(10.0) + f(0.0))
    for base, size in regions(h):
        chunk = read(h, base, size)
        if not chunk:
            continue
        i = chunk.find(needle)
        while i >= 0:
            cand = base + i - 0x340
            if cand > 0:
                dist = read(h, cand + 0xA18F8, 4)
                fov = read(h, cand + 0x9F0B4, 4)
                if (dist and abs(struct.unpack("<f", dist)[0] + 45.0) < 1e-3
                        and fov and abs(struct.unpack("<f", fov)[0] - 30.0) < 1e-3):
                    return cand
            i = chunk.find(needle, i + 1)
    return 0


def scan_track(handle):
    def key(fov):
        k = bytearray(84)
        struct.pack_into("<f", k, 0x0C, -45.0)
        struct.pack_into("<f", k, 0x14, 10.0)
        struct.pack_into("<i", k, 0x44, fov)
        return bytes(k)

    needle = key(30) + key(0)
    for base, size in regions(handle):
        chunk = read(handle, base, size)
        if not chunk:
            continue
        idx = chunk.find(needle)
        if idx >= 0:
            return base + idx
    return 0


def find_app(h, track):
    for base, size in regions(h):
        chunk = read(h, base, size)
        if not chunk:
            continue
        i = 0
        while True:
            i = chunk.find(struct.pack("<f", 30.0), i)
            if i < 0:
                break
            cand = base + i - 0x9F0B4
            if cand > 0:
                ptr = read(h, cand + 0x3D0, 8)
                if ptr and struct.unpack("<Q", ptr)[0] == track:
                    return cand
            i += 1
    return 0


def dump(h, app, tag):
    print("--- %s ---" % tag)
    for off, name, kind in FIELDS + MOUSE_FIELDS:
        raw = read(h, app + off, 4)
        if raw is None:
            print("  +0x%06X %-22s <unreadable>" % (off, name))
            continue
        if kind == "float":
            v = "%.4f" % struct.unpack("<f", raw)[0]
        elif kind == "byte":
            v = "%d" % raw[0]
        else:
            v = "%d" % struct.unpack("<i", raw)[0]
        print("  +0x%06X %-22s %s" % (off, name, v))


def main():
    exe = os.path.abspath(sys.argv[1])
    gesture = sys.argv[2] if len(sys.argv) > 2 else "rmb"
    dx = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    dy = int(sys.argv[4]) if len(sys.argv) > 4 else -160
    wait = float(sys.argv[5]) if len(sys.argv) > 5 else 9.0
    proc = subprocess.Popen([exe], cwd=os.path.dirname(exe))
    print("pid", proc.pid)
    time.sleep(wait)
    hits = find_window(proc.pid, None)
    hits.sort(key=lambda t: -t[2])
    hwnd = hits[0][0]
    r = wt.RECT()
    u.GetClientRect(hwnd, ctypes.byref(r))
    x0, y0 = int(r.right * 0.62), int(r.bottom * 0.30)
    print("client %dx%d drag origin (%d,%d)" % (r.right, r.bottom, x0, y0))

    h = k32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, proc.pid)
    track = scan_track(h)
    app = find_app_by_view(h)
    print("track 0x%X app 0x%X (view-block scan)" % (track, app))
    dump(h, app, "before")

    def lp(x, y):
        return (y << 16) | (x & 0xFFFF)

    u.PostMessageW(hwnd, WM_MOUSEMOVE, 0, lp(x0, y0))
    time.sleep(0.5)
    dump(h, app, "after first move")
    down, up, flag = ((WM_RBUTTONDOWN, WM_RBUTTONUP, MK_RBUTTON) if gesture == "rmb"
                      else (WM_MBUTTONDOWN, WM_MBUTTONUP, MK_MBUTTON))
    u.PostMessageW(hwnd, down, flag, lp(x0, y0))
    time.sleep(0.3)
    for i in range(1, 25):
        u.PostMessageW(hwnd, WM_MOUSEMOVE, flag, lp(x0 + dx * i // 24, y0 + dy * i // 24))
        time.sleep(0.05)
    time.sleep(0.3)
    dump(h, app, "during drag")
    u.PostMessageW(hwnd, up, 0, lp(x0 + dx, y0 + dy))
    time.sleep(1.2)
    dump(h, app, "after release")

    proc.terminate()
    return 0


if __name__ == "__main__":
    sys.exit(main())
