# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# end to end for windows and roles: the roleless rule, configure and
# ack, cascade placement, z order, popup positioner math, reoffers for
# unacknowledged commits and the error paths around roles

import os
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOCK = ROOT / "build" / "x12-roles.sock"
PPM = ROOT / "build" / "x12-roles.ppm"

XR24 = 0x34325258
W, H = 96, 96
SLOT = 32 * 32 * 4

RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
BG = (0, 0, 0)


def handshake(s):
    s.request("x12_connection", "hello", 1, idl_major=1, idl_minor=0)
    s.wait_for("welcome")
    for _ in range(4):
        s.wait_for("global")


def bind(s, name, version, id):
    s.request("x12_registry", "bind", 2, name=name, version=version, id=id, instance=0)
    s.register(name, id)


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
    deadline = time.time() + 10
    while time.time() < deadline:
        frame = read_ppm()
        if frame and check(frame):
            return
        time.sleep(0.02)
    raise AssertionError(f"pixels never matched: {what}")


def wait_configure(s, role_id):
    # configure events for other roles are not under test here
    while True:
        ev = s.next_event()
        if ev["name"] == "configure" and ev["object"] == role_id:
            return ev


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

        # one pool, three slots of pixels
        pool_fd = os.memfd_create("x12-pool")
        os.ftruncate(pool_fd, 3 * SLOT)
        s.request("x12_buffer_manager", "create_shm_pool", 11,
                  fds=[pool_fd], fd=0, size=3 * SLOT, id=20)
        s.register("x12_shm_pool", 20)

        def put(buf_id, slot, w, h, argb):
            pix = struct.pack("<I", argb) * (w * h)
            s.request("x12_shm_pool", "create_buffer", 20, id=buf_id,
                      offset=slot * SLOT, width=w, height=h,
                      stride=w * 4, format=XR24)
            s.register("x12_buffer", buf_id)
            os.pwrite(pool_fd, pix, slot * SLOT)

        def commit(surface, buf):
            s.request("x12_surface", "set_buffer", surface, buffer=buf)
            s.request("x12_surface", "damage", surface,
                      x=0, y=0, width=64, height=64)
            s.request("x12_surface", "commit", surface)

        def role_up(role_id, surface_id):
            s.request("x12_compositor", "create_toplevel", 10,
                      id=role_id, surface=surface_id)
            s.register("x12_toplevel", role_id)
            ev = wait_configure(s, role_id)
            s.request("x12_toplevel", "ack_configure", role_id,
                      serial=ev["args"]["serial"])

        put(21, 0, 32, 32, 0xFFFF0000)  # red
        put(22, 1, 32, 32, 0xFF00FF00)  # green
        put(23, 2, 16, 16, 0xFF0000FF)  # blue

        # phase 1: a roleless surface never composites, its red would
        # cover the origin but the canvas stays empty there. the first
        # toplevel lands on the cascade at 32,32
        s.request("x12_compositor", "create_surface", 10, id=25)
        s.register("x12_surface", 25)
        commit(25, 21)
        s.request("x12_compositor", "create_surface", 10, id=26)
        s.register("x12_surface", 26)
        role_up(4, 26)
        commit(26, 21)

        def check_first(frame):
            w, h, body = frame
            return px(body, w, 10, 10) == BG and px(body, w, 40, 40) == RED
        wait_pixels(check_first, "roleless stays empty, toplevel at 32,32")

        # phase 2: the second toplevel cascades to 56,56 and sits on
        # top where they overlap: z order is the stack order
        s.request("x12_compositor", "create_surface", 10, id=27)
        s.register("x12_surface", 27)
        role_up(5, 27)
        commit(27, 22)

        def check_cascade(frame):
            w, h, body = frame
            return (px(body, w, 33, 33) == RED and
                    px(body, w, 70, 70) == GREEN and
                    px(body, w, 60, 60) == GREEN)
        wait_pixels(check_cascade, "cascade placement and z order")

        # phase 3: a popup is placed from the positioner copy: the
        # center of the 16x16 anchor rect plus the offset, in parent
        # local coordinates. the parent maps at 32,32 so the popup
        # lands at 36,32 and paints over the red
        s.request("x12_compositor", "create_positioner", 10, id=12)
        s.register("x12_positioner", 12)
        s.request("x12_positioner", "set_anchor_rect", 12,
                  x=0, y=0, width=16, height=16)
        s.request("x12_positioner", "set_anchor", 12, anchor="center")
        s.request("x12_positioner", "set_gravity", 12, gravity="center")
        s.request("x12_positioner", "set_offset", 12, x=4, y=0)
        s.request("x12_compositor", "create_surface", 10, id=28)
        s.register("x12_surface", 28)
        s.request("x12_compositor", "create_popup", 10, id=6,
                  surface=28, parent=26, positioner=12)
        s.register("x12_popup", 6)
        ev = wait_configure(s, 6)
        s.request("x12_popup", "ack_configure", 6,
                  serial=ev["args"]["serial"])
        commit(28, 23)

        def check_popup(frame):
            w, h, body = frame
            return (px(body, w, 40, 40) == BLUE and
                    px(body, w, 33, 33) == RED and
                    px(body, w, 70, 70) == GREEN)
        wait_pixels(check_popup, "popup placed from the positioner")

        # phase 4: a commit without an ack reoffers the configure. the
        # fullscreen offer carries the whole mode, the reoffer repeats
        # it until the client accepts. the pin lands at the origin
        s.request("x12_toplevel", "set_fullscreen", 4, state=1)
        ev = wait_configure(s, 4)
        assert ev["args"]["width"] == W and ev["args"]["height"] == H, ev
        assert ev["args"]["states"] == [2], ev  # window_state.fullscreen
        commit(26, 21)  # no ack yet, the offer repeats
        ev = wait_configure(s, 4)  # the reoffer
        s.request("x12_toplevel", "ack_configure", 4,
                  serial=ev["args"]["serial"])
        commit(26, 21)

        def check_fullscreen(frame):
            w, h, body = frame
            return (px(body, w, 5, 5) == RED and
                    px(body, w, 33, 50) == BG and
                    px(body, w, 40, 40) == BLUE)
        wait_pixels(check_fullscreen, "fullscreen pin and reoffer")

        # phase 5: the error paths. state above 1 is bad-value, a
        # second role on one surface and a popup under a roleless
        # parent are both invalid-id, and a grab on an invented serial
        # never arms
        s.request("x12_toplevel", "set_fullscreen", 4, state=2)
        err = s.wait_for("error")
        assert err["args"]["code"] == "bad-value", err
        s.request("x12_compositor", "create_toplevel", 10, id=8, surface=26)
        err = s.wait_for("error")
        assert err["args"]["code"] == "invalid-id", err
        s.request("x12_compositor", "create_surface", 10, id=29)
        s.register("x12_surface", 29)
        s.request("x12_compositor", "create_popup", 10, id=9,
                  surface=29, parent=25, positioner=12)
        err = s.wait_for("error")
        assert err["args"]["code"] == "invalid-id", err
        s.request("x12_popup", "grab", 6, seat=11, serial=4242)
        err = s.wait_for("error")
        assert err["args"]["code"] == "bad-value", err

        # phase 6: destroying the popup role drops its window, killing
        # the surface behind a role empties its slot and an unmap
        # commit takes the window off the stack
        s.request("x12_popup", "destroy", 6)

        def check_no_popup(frame):
            w, h, body = frame
            return px(body, w, 40, 40) == BG
        wait_pixels(check_no_popup, "popup gone after destroy")

        s.request("x12_surface", "destroy", 26)

        def check_no_toplevel(frame):
            w, h, body = frame
            return (px(body, w, 5, 5) == BG and
                    px(body, w, 70, 70) == GREEN)
        wait_pixels(check_no_toplevel, "toplevel gone, second window stays")

        s.request("x12_surface", "set_buffer", 27, buffer=0)  # empty slot
        s.request("x12_surface", "commit", 27)  # the next commit unmaps

        def check_unmap(frame):
            w, h, body = frame
            return px(body, w, 70, 70) == BG
        wait_pixels(check_unmap, "unmap commit empties the stack")

        os.close(pool_fd)
        s.close()
    finally:
        proc.terminate()
        proc.wait(timeout=5)
    print("e2e roles: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
