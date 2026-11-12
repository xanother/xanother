# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# end to end for the gpu canvas: the random scene contract binary, then
# one scripted scene through a cpu server and a gpu server that must
# land byte for byte on the same ppm dump. the script knows nothing
# about pixels - identical inputs, identical outputs - so any drift
# between the two blend paths shows up as a raw diff. a gpu node that
# does not exist must degrade to the cpu path and keep rendering

import os
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOCK = ROOT / "build" / "x12-gpu.sock"
PPM = ROOT / "build" / "x12-gpu.ppm"
SOCK2 = ROOT / "build" / "x12-gpu2.sock"
PPM2 = ROOT / "build" / "x12-gpu2.ppm"

XR24 = 0x34325258
AR24 = 0x34325241
W, H = 32, 32


def handshake(s):
    s.request("x12_connection", "hello", 1, idl_major=1, idl_minor=0)
    s.wait_for("welcome")
    for _ in range(4):
        s.wait_for("global")


def bind(s, name, version, id):
    s.request("x12_registry", "bind", 2, name=name, version=version, id=id,
              instance=0)
    s.register(name, id)


def wait_configure(s, role_id):
    while True:
        ev = s.next_event()
        if ev["name"] == "configure" and ev["object"] == role_id:
            return ev


def role_up(s, role_id, surface_id, fullscreen):
    s.request("x12_compositor", "create_toplevel", 10, id=role_id,
              surface=surface_id)
    s.register("x12_toplevel", role_id)
    ev = wait_configure(s, role_id)
    s.request("x12_toplevel", "ack_configure", role_id,
              serial=ev["args"]["serial"])
    if fullscreen:
        s.request("x12_toplevel", "set_fullscreen", role_id, state=1)
        # the fullscreen offer reconfigures the window, the commit maps
        # against the acked offer only
        ev = wait_configure(s, role_id)
        s.request("x12_toplevel", "ack_configure", role_id,
                  serial=ev["args"]["serial"])


def pattern_xrgb(w, h):
    return b"".join(
        struct.pack("<I", ((x * 8) & 0xFF) | (((y * 8) & 0xFF) << 8) |
                   ((((x + y) * 4) & 0xFF) << 16) | 0xFF000000)
        for y in range(h) for x in range(w))


def pattern_argb(w, h):
    # valid premultiplied: channels never exceed alpha
    out = bytearray()
    for y in range(h):
        for x in range(w):
            a = 128
            out += bytes((min(a, (x * 5) & 0x7F), min(a, (y * 5) & 0x7F),
                          min(a, (x * y) & 0x7F), a))
    return bytes(out)


def run_scene(sock, ppm, extra_args):
    """drives the identical scripted scene, returns the ppm body bytes"""
    ppm.unlink(missing_ok=True)
    srv = cc.spawn_server(ROOT / "build" / "x12-server", sock,
                          ["--width", str(W), "--height", str(H),
                           "--ppm", str(ppm)] + list(extra_args))
    try:
        idl = load_idl(ROOT / "spec" / "x12-core.yaml")
        s = cc.Session(sock, idl)
        handshake(s)
        bind(s, "x12_compositor", 2, 10)
        bind(s, "x12_buffer_manager", 1, 11)
        for _ in range(2):
            s.wait_for("format")

        # the fullscreen window carries the opaque pattern
        pool_fd = os.memfd_create("x12-pool", os.MFD_ALLOW_SEALING)
        os.ftruncate(pool_fd, 4 * W * H * 4)
        s.request("x12_buffer_manager", "create_shm_pool", 11,
                  fds=[pool_fd], fd=0, size=4 * W * H * 4, id=20)
        s.register("x12_shm_pool", 20)
        s.request("x12_shm_pool", "create_buffer", 20, id=21, offset=0,
                  width=W, height=H, stride=W * 4, format=XR24)
        s.register("x12_buffer", 21)
        os.pwrite(pool_fd, pattern_xrgb(W, H), 0)

        s.request("x12_compositor", "create_surface", 10, id=30)
        s.register("x12_surface", 30)
        role_up(s, 12, 30, fullscreen=True)
        s.request("x12_surface", "frame", 30, callback=4)
        s.register("x12_callback", 4)
        s.request("x12_surface", "set_buffer", 30, buffer=21)
        s.request("x12_surface", "damage", 30, x=0, y=0, width=W, height=H)
        s.request("x12_surface", "commit", 30)
        # the done rides the frame leaving its slot: the next commit
        # retires it
        s.request("x12_surface", "commit", 30)
        s.wait_for("done")

        # a translucent window with a 2x scaled buffer over it, the
        # scale and the premultiplied over ride the same raw diff
        s.request("x12_shm_pool", "create_buffer", 20, id=22,
                  offset=2 * W * H * 4,
                  width=16, height=16, stride=16 * 4, format=AR24)
        s.register("x12_buffer", 22)
        os.pwrite(pool_fd, pattern_argb(16, 16), 2 * W * H * 4)
        s.request("x12_compositor", "create_surface", 10, id=31)
        s.register("x12_surface", 31)
        role_up(s, 13, 31, fullscreen=False)
        s.request("x12_surface", "frame", 31, callback=5)
        s.register("x12_callback", 5)
        s.request("x12_surface", "set_scale", 31, scale=512)
        s.request("x12_surface", "set_buffer", 31, buffer=22)
        s.request("x12_surface", "damage", 31, x=0, y=0, width=8, height=8)
        s.request("x12_surface", "commit", 31)
        s.request("x12_surface", "commit", 31)
        s.wait_for("done")
        # the ppm must be captured while the session lives: a disconnect
        # repaints the empty canvas and rewrites the dump black. two
        # equal reads in a row beat the truncating rewrite of a present
        frame = None
        prev = None
        for _ in range(200):
            time.sleep(0.02)
            try:
                data = ppm.read_bytes()
            except FileNotFoundError:
                continue
            parts = data.split(b"255\n", 1)
            if len(parts) != 2:
                continue
            wh = parts[0].split()
            w, h = int(wh[1]), int(wh[2])
            if len(parts[1]) != w * h * 3:
                continue
            if prev == parts[1]:
                frame = (w, h, parts[1])
                break
            prev = parts[1]
        os.close(pool_fd)
        s.close()
        if frame is None:
            raise AssertionError("no ppm dump from " + str(ppm))
        return frame
    finally:
        srv.terminate()
        srv.wait(10)


def px(body, w, x, y):
    o = 3 * (y * w + x)
    return (body[o], body[o + 1], body[o + 2])


def test_contract():
    r = subprocess.run([str(ROOT / "build" / "c_gpu_contract"), "200"],
                       capture_output=True, text=True)
    out = r.stdout.strip()
    print("  contract:", out)
    assert r.returncode == 0 and ("PASS" in out or "SKIP" in out), \
        "gpu contract binary failed"


def test_pipeline():
    """the same scene on the cpu path and the gpu path, one raw diff"""
    w1, h1, cpu_body = run_scene(SOCK, PPM, [])
    w2, h2, gpu_body = run_scene(SOCK2, PPM2, ["--gpu"])
    assert (w1, h1) == (W, H), f"cpu canvas {w1}x{h1}"
    assert (w2, h2) == (W, H), f"gpu canvas {w2}x{h2}"
    if cpu_body != gpu_body:
        diffs = sum(1 for a, b in zip(cpu_body, gpu_body) if a != b)
        first = next(i for i, (a, b) in enumerate(zip(cpu_body, gpu_body))
                     if a != b)
        raise AssertionError(
            f"cpu and gpu ppms differ: {diffs} bytes, first at {first} "
            f"(x={first // 3 % w1}, y={first // 3 // w1})")
    # a spot check keeps the diff honest: the fullscreen pattern must be
    # on screen even when both paths are wrong in the same way
    assert px(cpu_body, W, 1, 1) != (0, 0, 0), "scene did not paint"
    print(f"  pipeline: cpu == gpu on {w1}x{h1} ({len(cpu_body)} bytes)")


def test_degrade():
    """a missing gpu node degrades to the cpu path, the server lives"""
    srv = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                          ["--width", str(W), "--height", str(H),
                           "--ppm", str(PPM), "--gpu-node", "/dev/null"])
    try:
        idl = load_idl(ROOT / "spec" / "x12-core.yaml")
        s = cc.Session(SOCK, idl)
        handshake(s)
        s.close()
    finally:
        srv.terminate()
        out = srv.stderr.read().decode() if srv.stderr else ""
        srv.wait(10)
    assert "cpu path" in out or "context on" in out, out
    print("  degrade: server survives a bad gpu node")


def main():
    test_contract()
    test_pipeline()
    test_degrade()
    print("e2e gpu ok")


if __name__ == "__main__":
    main()
