# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# end to end for the render path: xrgb copies, premultiplied argb over,
# nearest neighbour scale and damage driven staleness, all asserted
# pixel by pixel against the ppm dump. the blend formula here mirrors
# src/render/cpu.c on purpose, the two are one contract

import os
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOCK = ROOT / "build" / "x12-render.sock"
PPM = ROOT / "build" / "x12-render.ppm"

XR24 = 0x34325258
AR24 = 0x34325241
W, H = 16, 16
SLOT = 32 * 32 * 4


def handshake(s):
    s.request("x12_connection", "hello", 1, idl_major=1, idl_minor=0)
    s.wait_for("welcome")
    for _ in range(4):
        s.wait_for("global")


def bind(s, name, version, id):
    s.request("x12_registry", "bind", 2, name=name, version=version, id=id, instance=0)
    s.register(name, id)


def wait_configure(s, role_id):
    # configure events for other roles are not under test here
    while True:
        ev = s.next_event()
        if ev["name"] == "configure" and ev["object"] == role_id:
            return ev


def role_up(s, role_id, surface_id):
    # a surface composites only through a window role: the toplevel
    # offer comes back as configure, the ack maps the window on commit.
    # fullscreen pins the view at the origin so the pixel checks run on
    # a small canvas without the cascade placement in the way
    s.request("x12_compositor", "create_toplevel", 10, id=role_id,
              surface=surface_id)
    s.register("x12_toplevel", role_id)
    ev = wait_configure(s, role_id)
    s.request("x12_toplevel", "ack_configure", role_id,
              serial=ev["args"]["serial"])
    s.request("x12_toplevel", "set_fullscreen", role_id, state=1)
    ev = wait_configure(s, role_id)
    s.request("x12_toplevel", "ack_configure", role_id,
              serial=ev["args"]["serial"])


def read_ppm():
    # the server rewrites the dump every present, a partial read shows up
    # as a short split or a size mismatch and the caller retries
    try:
        data = PPM.read_bytes()
    except FileNotFoundError:
        return None
    parts = data.split(b"255\n", 1)
    if len(parts) != 2:
        return None
    head, body = parts
    wh = head.split()
    w, h = int(wh[1]), int(wh[2])
    if len(body) != w * h * 3:
        return None
    return w, h, body


def px(body, w, x, y):
    o = 3 * (y * w + x)
    return (body[o], body[o + 1], body[o + 2])


def wait_pixels(check, what):
    # polls the dump until the assert passes, the pass runs every loop
    # round so a fresh present is never far away
    deadline = time.time() + 10
    while time.time() < deadline:
        frame = read_ppm()
        if frame and check(frame):
            return
        time.sleep(0.02)
    raise AssertionError(f"pixels never matched: {what}")


def blend_ref(dst, sa, sb, sg, sr):
    # the premultiplied over contract, integer math exactly like the cpu
    # reference engine
    inv = 255 - sa
    b = sb + (dst & 0xFF) * inv // 255
    g = sg + ((dst >> 8) & 0xFF) * inv // 255
    r = sr + ((dst >> 16) & 0xFF) * inv // 255
    a = sa + (dst >> 24) * inv // 255
    return b | (g << 8) | (r << 16) | (a << 24)


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def pattern(x, y):
    return 0xFF000000 | (x << 16) | (y << 8) | 0x40


def main():
    idl = load_idl(ROOT / "spec" / "x12-core.yaml")
    PPM.unlink(missing_ok=True)
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           ["--width", W, "--height", H, "--ppm", str(PPM)])
    try:
        s = cc.Session(SOCK, idl)
        handshake(s)
        bind(s, "x12_compositor", 2, 10)
        bind(s, "x12_buffer_manager", 1, 11)
        for _ in range(2):
            s.wait_for("format")

        # one pool, five slots of pixels
        pool_fd = os.memfd_create("x12-pool")
        os.ftruncate(pool_fd, 5 * SLOT)
        s.request("x12_buffer_manager", "create_shm_pool", 11,
                  fds=[pool_fd], fd=0, size=5 * SLOT, id=20)
        s.register("x12_shm_pool", 20)

        def put(buf_id, slot, fmt, w, h, pixels):
            s.request("x12_shm_pool", "create_buffer", 20, id=buf_id,
                      offset=slot * SLOT, width=w, height=h,
                      stride=w * 4, format=fmt)
            s.register("x12_buffer", buf_id)
            os.pwrite(pool_fd, pixels, slot * SLOT)

        def commit(surface, buf):
            s.request("x12_surface", "set_buffer", surface, buffer=buf)
            s.request("x12_surface", "damage", surface,
                      x=0, y=0, width=W, height=H)
            s.request("x12_surface", "commit", surface)

        # phase 1: xrgb identity, the pattern lands pixel for pixel. the
        # window is fullscreen pinned so it sits at the origin
        pat = b"".join(struct.pack("<I", pattern(x, y))
                       for y in range(H) for x in range(W))
        put(21, 0, XR24, W, H, pat)
        s.request("x12_compositor", "create_surface", 10, id=30)
        s.register("x12_surface", 30)
        role_up(s, 12, 30)
        commit(30, 21)

        def check_pattern(frame):
            w, h, body = frame
            return all(px(body, w, x, y) == rgb(pattern(x, y))
                       for y in range(h) for x in range(w))
        wait_pixels(check_pattern, "xrgb identity")

        # phase 2: premultiplied argb over the pattern, the expected
        # value comes from the same formula the engine uses
        half_blue = struct.pack("<I", 0x80000080) * (W * H)
        put(22, 1, AR24, W, H, half_blue)
        s.request("x12_compositor", "create_surface", 10, id=31)
        s.register("x12_surface", 31)
        role_up(s, 13, 31)
        commit(31, 22)

        def check_blend(frame):
            w, h, body = frame
            for y in range(h):
                for x in range(w):
                    out = blend_ref(pattern(x, y), 128, 128, 0, 0)
                    if px(body, w, x, y) != rgb(out):
                        return False
            return True
        wait_pixels(check_blend, "argb premultiplied over")

        # phase 3: scale 2.0, a 32x32 buffer of four quadrant colors maps
        # down to the canvas with nearest neighbour
        s.request("x12_surface", "destroy", 31)
        bufpix = b"".join(
            struct.pack("<I", 0xFF000000 |
                        ((0 if y < 16 else 1) * 2 + (0 if x < 16 else 1)) * 0x203040)
            for y in range(32) for x in range(32))
        put(23, 2, XR24, 32, 32, bufpix)
        s.request("x12_surface", "set_scale", 30, scale=512)
        commit(30, 23)

        def check_scale(frame):
            w, h, body = frame
            for y in range(h):
                for x in range(w):
                    q = (0 if y < H // 2 else 1) * 2 + (0 if x < W // 2 else 1)
                    if px(body, w, x, y) != rgb(0xFF000000 | q * 0x203040):
                        return False
            return True
        wait_pixels(check_scale, "nearest neighbour scale")

        # phase 4: damage driven staleness, the second commit damages only
        # the top half so the bottom half keeps the old pixels
        red = struct.pack("<I", 0xFFFF0000) * (W * H)
        green = struct.pack("<I", 0xFF00FF00) * (W * H)
        put(24, 3, XR24, W, H, red)
        s.request("x12_surface", "set_scale", 30, scale=256)
        commit(30, 24)

        def check_red(frame):
            w, h, body = frame
            return all(px(body, w, x, y) == (255, 0, 0)
                       for y in range(h) for x in range(w))
        wait_pixels(check_red, "full red")

        put(25, 4, XR24, W, H, green)
        s.request("x12_surface", "set_buffer", 30, buffer=25)
        s.request("x12_surface", "damage", 30,
                  x=0, y=0, width=W, height=H // 2)
        s.request("x12_surface", "commit", 30)

        def check_stale(frame):
            w, h, body = frame
            top = all(px(body, w, x, y) == (0, 255, 0)
                      for y in range(H // 2) for x in range(w))
            bottom = all(px(body, w, x, y) == (255, 0, 0)
                         for y in range(H // 2, h) for x in range(w))
            return top and bottom
        wait_pixels(check_stale, "damage staleness")

        # phase 5: destroying the surface returns the background
        s.request("x12_surface", "destroy", 30)

        def check_background(frame):
            w, h, body = frame
            return all(px(body, w, x, y) == (0, 0, 0)
                       for y in range(h) for x in range(w))
        wait_pixels(check_background, "background after destroy")
        os.close(pool_fd)
        s.close()
    finally:
        proc.terminate()
        proc.wait(timeout=5)
    print("e2e render: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
