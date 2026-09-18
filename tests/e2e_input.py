# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# end to end for input: the file event source drives the router and the
# seat protocol events are asserted in order - hover, click to focus
# with the raise, popup grabs with real serials, modifiers, axis
# batching, and the cursor sprite drawn pixel by pixel

import os
import struct
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOCK = ROOT / "build" / "x12-input.sock"
PPM = ROOT / "build" / "x12-input.ppm"
FIFO = ROOT / "build" / "x12-input.fifo"

XR24 = 0x34325258
W, H = 96, 96
SLOT = 32 * 32 * 4

RED = (255, 0, 0)
GREEN = (0, 255, 0)
BLUE = (0, 0, 255)
MAGENTA = (255, 0, 255)


def handshake(s):
    s.request("x12_connection", "hello", 1, idl_major=1, idl_minor=0)
    s.wait_for("welcome")
    for _ in range(4):
        s.wait_for("global")


def bind(s, name, version, id):
    s.request("x12_registry", "bind", 2, name=name, version=version, id=id, instance=0)
    s.register(name, id)


def expect(s, name, obj=None):
    # strict order for the events under test, committed token traffic is
    # covered by the buffer suite and skips past here
    while True:
        ev = s.next_event()
        if ev["name"] != "committed":
            break
    assert ev["name"] == name, ev
    if obj is not None:
        assert ev["object"] == obj, ev
    return ev


def read_ppm():
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


def open_fifo():
    # the server opens the read end during startup, a write open on a
    # readerless fifo fails with enxio and just retries
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            return os.open(str(FIFO), os.O_WRONLY | os.O_NONBLOCK)
        except OSError:
            time.sleep(0.02)
    raise AssertionError("input fifo never opened")


def main():
    idl = load_idl(ROOT / "spec" / "x12-core.yaml")
    PPM.unlink(missing_ok=True)
    FIFO.unlink(missing_ok=True)
    os.mkfifo(FIFO)
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           ["--input", str(FIFO), "--width", W, "--height", H,
                            "--ppm", str(PPM)])
    wfd = open_fifo()

    def send(line):
        os.write(wfd, (line + "\n").encode())

    try:
        s = cc.Session(SOCK, idl)
        handshake(s)
        bind(s, "x12_compositor", 2, 10)
        bind(s, "x12_buffer_manager", 1, 11)
        bind(s, "x12_seat", 2, 13)
        for _ in range(2):
            s.wait_for("format")

        # phase 0: the seat announces itself, the keyboard ships the
        # compiled us keymap as a memfd and the repeat policy
        cap = expect(s, "capabilities", 13)
        assert cap["args"]["value"] == 3, cap  # pointer and keyboard bits
        nm = expect(s, "name", 13)
        assert nm["args"]["name"] == "x12-seat", nm
        s.request("x12_seat", "get_pointer", 13, id=14)
        s.register("x12_pointer", 14)
        s.request("x12_seat", "get_keyboard", 13, id=15)
        s.register("x12_keyboard", 15)
        km = expect(s, "keymap", 15)
        assert km["args"]["format"] == "xkb-v1", km
        assert km["args"]["fd"] == 0, km
        assert len(s.recv_fds) == 1, s.recv_fds
        text = os.read(s.recv_fds[0], 1 << 20)
        assert text.startswith(b"xkb_keymap {"), text[:20]
        assert len(text) == km["args"]["size"], (len(text), km["args"])
        ri = expect(s, "repeat_info", 15)
        assert ri["args"] == {"rate": 25, "delay": 600}, ri

        # windows: red at cascade 32,32 and green at 56,56
        pool_fd = os.memfd_create("x12-pool")
        os.ftruncate(pool_fd, 4 * SLOT)
        s.request("x12_buffer_manager", "create_shm_pool", 11,
                  fds=[pool_fd], fd=0, size=4 * SLOT, id=20)
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
            ev = expect(s, "configure", role_id)
            s.request("x12_toplevel", "ack_configure", role_id,
                      serial=ev["args"]["serial"])

        put(21, 0, 32, 32, 0xFFFF0000)  # red
        put(22, 1, 32, 32, 0xFF00FF00)  # green
        put(23, 2, 16, 16, 0xFF0000FF)  # blue
        put(24, 3, 8, 8, 0xFFFF00FF)    # magenta
        s.request("x12_compositor", "create_surface", 10, id=25)
        s.register("x12_surface", 25)
        role_up(4, 25)
        commit(25, 21)
        s.request("x12_compositor", "create_surface", 10, id=26)
        s.register("x12_surface", 26)
        role_up(5, 26)
        commit(26, 22)

        def check_windows(frame):
            w, h, body = frame
            return (px(body, w, 40, 40) == RED and
                    px(body, w, 70, 70) == GREEN and
                    px(body, w, 60, 60) == GREEN)
        wait_pixels(check_windows, "two cascade windows")

        # phase 1: hover, enter and leave carry surface local fixed24p8
        send("motion 40 40")
        ev = expect(s, "enter", 14)
        assert ev["args"]["surface"] == 25, ev
        assert ev["args"]["x"] == 8 * 256 and ev["args"]["y"] == 8 * 256, ev
        ev = expect(s, "motion", 14)
        assert ev["args"]["surface"] == 25, ev
        send("motion 70 70")
        ev = expect(s, "leave", 14)
        assert ev["args"]["surface"] == 25, ev
        ev = expect(s, "enter", 14)
        assert ev["args"]["surface"] == 26, ev
        assert ev["args"]["x"] == 14 * 256, ev
        expect(s, "motion", 14)
        send("motion 10 10")
        ev = expect(s, "leave", 14)
        assert ev["args"]["surface"] == 26, ev

        # phase 2: click to focus on the toplevel and raise it above its
        # peer, the overlap turns red
        send("motion 40 40")
        expect(s, "enter", 14)
        expect(s, "motion", 14)
        send("button 1 press")
        ev = expect(s, "button", 14)
        assert ev["args"]["button"] == 1, ev
        assert ev["args"]["state"] == "pressed", ev
        press_serial = ev["args"]["serial"]
        ev = expect(s, "enter", 15)
        assert ev["args"]["surface"] == 25, ev
        assert ev["args"]["keys"] == [], ev
        mods = expect(s, "modifiers", 15)
        assert mods["args"]["mods_depressed"] == 0, mods
        send("button 1 release")
        ev = expect(s, "button", 14)
        assert ev["args"]["state"] == "released", ev

        def check_raise(frame):
            w, h, body = frame
            return (px(body, w, 60, 60) == RED and
                    px(body, w, 70, 70) == GREEN)
        wait_pixels(check_raise, "raise on focus")

        # phase 3: a popup grabs with the press serial the client really
        # received, a press outside dismisses it and reroutes
        s.request("x12_compositor", "create_positioner", 10, id=12)
        s.register("x12_positioner", 12)
        s.request("x12_positioner", "set_anchor_rect", 12,
                  x=0, y=0, width=16, height=16)
        s.request("x12_positioner", "set_anchor", 12, anchor="center")
        s.request("x12_positioner", "set_gravity", 12, gravity="center")
        s.request("x12_positioner", "set_offset", 12, x=4, y=0)
        s.request("x12_compositor", "create_surface", 10, id=27)
        s.register("x12_surface", 27)
        s.request("x12_compositor", "create_popup", 10, id=6,
                  surface=27, parent=25, positioner=12)
        s.register("x12_popup", 6)
        ev = expect(s, "configure", 6)
        s.request("x12_popup", "ack_configure", 6,
                  serial=ev["args"]["serial"])
        commit(27, 23)  # maps at 36,32 over the red parent

        def check_popup(frame):
            w, h, body = frame
            return px(body, w, 40, 40) == BLUE
        wait_pixels(check_popup, "popup on the red parent")

        s.request("x12_popup", "grab", 6, seat=13, serial=press_serial)
        send("motion 48 48")  # the grab holds even past the bounds
        expect(s, "leave", 14)
        ev = expect(s, "enter", 14)
        assert ev["args"]["surface"] == 27, ev
        ev = expect(s, "motion", 14)
        assert ev["args"]["surface"] == 27, ev
        assert ev["args"]["x"] == 12 * 256 and ev["args"]["y"] == 16 * 256, ev
        send("button 1 press")  # outside the popup, inside the parent
        expect(s, "dismiss", 6)
        expect(s, "button", 14)
        send("button 1 release")
        expect(s, "button", 14)

        # phase 4: keys carry the modifier tracking, shift is bit zero
        send("key 42 press")
        ev = expect(s, "key", 15)
        assert ev["args"]["key"] == 42 and ev["args"]["state"] == "pressed", ev
        mods = expect(s, "modifiers", 15)
        assert mods["args"]["mods_depressed"] == 1, mods
        send("key 42 release")
        expect(s, "key", 15)
        mods = expect(s, "modifiers", 15)
        assert mods["args"]["mods_depressed"] == 0, mods

        # phase 5: a wheel notch is one axis event and an immediate
        # frame, the frame delimits the scroll group
        send("axis vertical 512")
        ev = expect(s, "axis", 14)
        assert ev["args"] == {"axis": "vertical-scroll", "value": 512}, ev
        expect(s, "frame", 14)

        # phase 6: the cursor sprite rides the hovered client cursor
        # surface at the pointer minus the hotspot, and moving it leaves
        # no smear behind
        s.request("x12_compositor", "create_surface", 10, id=28)
        s.register("x12_surface", 28)
        commit(28, 24)  # roleless, invisible until set_cursor
        s.request("x12_pointer", "set_cursor", 14, serial=0, surface=28,
                  hotspot_x=2, hotspot_y=3)
        send("motion 40 40")
        expect(s, "enter", 14)  # hover re-enters after the grab released it
        expect(s, "motion", 14)

        def check_sprite(frame):
            w, h, body = frame
            for y in range(37, 45):
                for x in range(38, 46):
                    if px(body, w, x, y) != MAGENTA:
                        return False
            return True
        wait_pixels(check_sprite, "sprite at pointer minus hotspot")

        send("motion 60 60")
        expect(s, "motion", 14)

        def check_sprite_moved(frame):
            w, h, body = frame
            for y in range(57, 65):
                for x in range(58, 66):
                    if px(body, w, x, y) != MAGENTA:
                        return False
            # the old sprite area shows the red window again
            return px(body, w, 40, 40) == RED
        wait_pixels(check_sprite_moved, "sprite moved without a smear")

        # phase 7: one device per kind, a second one refuses loudly and
        # touch stays wired protocol without a runtime
        s.request("x12_seat", "get_pointer", 13, id=7)
        err = expect(s, "error", 1)
        assert err["args"]["code"] == "bad-value", err
        assert err["args"]["object"] == 13, err
        s.request("x12_seat", "get_touch", 13, id=8)
        err = expect(s, "error", 1)
        assert err["args"]["code"] == "bad-value", err
        s.request("x12_pointer", "release", 14)
        s.request("x12_keyboard", "release", 15)

        os.close(pool_fd)
        os.close(wfd)
        s.close()
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        FIFO.unlink(missing_ok=True)
    print("e2e input: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
