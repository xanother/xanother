# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# sessions e2e: a disconnected client finds its windows, its objects
# and its scene presence waiting behind the resume token. the resume
# window has a deadline, and when a whole server dies the client
# rebuilds its state on a new one - client side migration

import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402
from tests.e2e_net import spawn, dial, handshake, free_port  # noqa: E402

BUILD = ROOT / "build"
XR24 = 0x34325258
W = H = 16


def session_handshake(s):
    # like handshake but keeps the resume token
    s.request("x12_connection", "hello", 1, idl_major=1, idl_minor=0)
    welcome = s.wait_for("welcome")
    assert welcome["args"]["server_version"] == 1, welcome
    token = s.wait_for("session")["args"]["token"]
    assert token != 0
    for _ in range(5):
        s.wait_for("global")
    return token


def scene_up(s, pixel):
    # one net pool, one fullscreen window, pixels on screen. ids are
    # fixed so a resumed session can name them again
    s.request("x12_registry", "bind", 2,
              name="x12_compositor", version=2, id=10, instance=0)
    s.register("x12_compositor", 10)
    s.request("x12_registry", "bind", 2,
              name="x12_buffer_manager", version=1, id=11, instance=0)
    s.register("x12_buffer_manager", 11)
    for _ in range(2):
        s.wait_for("format")

    slot = W * H * 4
    s.request("x12_buffer_manager", "create_net_pool", 11,
              size=slot, id=20)
    s.register("x12_shm_pool", 20)
    s.request("x12_shm_pool", "write", 20, offset=0,
              data=struct.pack("<I", pixel) * (W * H))

    s.request("x12_shm_pool", "create_buffer", 20, id=21, offset=0,
              width=W, height=H, stride=W * 4, format=XR24)
    s.register("x12_buffer", 21)
    s.request("x12_compositor", "create_surface", 10, id=30)
    s.register("x12_surface", 30)
    s.request("x12_compositor", "create_toplevel", 10, id=31, surface=30)
    s.register("x12_toplevel", 31)
    role_up(s)
    s.request("x12_surface", "set_buffer", 30, buffer=21)
    s.request("x12_surface", "damage", 30, x=0, y=0, width=W, height=H)
    s.request("x12_surface", "commit", 30)


def role_up(s):
    for _ in range(2):
        while True:
            ev = s.next_event()
            if ev["name"] == "configure" and ev["object"] == 31:
                break
        s.request("x12_toplevel", "ack_configure", 31,
                  serial=ev["args"]["serial"])
        if _ == 0:
            s.request("x12_toplevel", "set_fullscreen", 31, state=1)


def repaint(s, pixel):
    # the session keeps working through its old ids
    s.request("x12_shm_pool", "write", 20, offset=0,
              data=struct.pack("<I", pixel) * (W * H))
    s.request("x12_surface", "damage", 30, x=0, y=0, width=W, height=H)
    s.request("x12_surface", "commit", 30)


def wait_pixels(ppm, want, what):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            data = ppm.read_bytes()
        except FileNotFoundError:
            time.sleep(0.02)
            continue
        parts = data.split(b"255\n", 1)
        if len(parts) != 2 or len(parts[1]) != W * H * 3:
            time.sleep(0.02)
            continue
        body = parts[1]
        o = 3 * (8 * W + 8)
        if (body[o], body[o + 1], body[o + 2]) == want:
            return
        time.sleep(0.02)
    raise AssertionError(f"pixels never matched: {what}")


def rgb(v):
    return ((v >> 16) & 255, (v >> 8) & 255, v & 255)


def resume_keeps_the_session(idl):
    ppm = BUILD / "session.ppm"
    ppm.unlink(missing_ok=True)
    addr = f"tcp:127.0.0.1:{free_port()}"
    proc, err = spawn(addr, extra_args=["--ppm", str(ppm)], size=(W, H))
    try:
        s = dial(addr, idl)
        token = session_handshake(s)
        scene_up(s, 0xFF2244CC)
        wait_pixels(ppm, rgb(0xFF2244CC), "first scene")

        # the transport dies, the session does not
        s.sock.close()
        time.sleep(0.3)

        s2 = dial(addr, idl)
        fresh = session_handshake(s2)
        assert fresh != token
        s2.request("x12_connection", "resume", 1, token=token)
        back = s2.wait_for("session")["args"]["token"]
        assert back == token, (back, token)

        # the old objects answer on the new transport
        s2.request("x12_connection", "sync", 1, callback=5)
        s2.register("x12_callback", 5)
        s2.wait_for("done")

        # and the old scene still moves: a repaint through the parked
        # pool and surface lands on screen
        repaint(s2, 0xFF88CC22)
        wait_pixels(ppm, rgb(0xFF88CC22), "repaint after resume")
        s2.close()
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        err.close()


def resume_window_closes(idl):
    addr = f"tcp:127.0.0.1:{free_port()}"
    proc, err = spawn(addr, extra_env={"X12_SESSION_TTL_MS": "500"})
    try:
        s = dial(addr, idl)
        token = session_handshake(s)
        s.sock.close()
        time.sleep(1.2)

        s2 = dial(addr, idl)
        session_handshake(s2)
        s2.request("x12_connection", "resume", 1, token=token)
        err_ev = s2.wait_for("error")
        assert err_ev["args"]["code"] == "bad-value", err_ev
        s2.close()
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        err.close()


def client_side_migration(idl):
    # when the whole server dies there is nothing to resume: the client
    # holds its own state and rebuilds the same scene on a new server
    ppm = BUILD / "migrate.ppm"
    ppm.unlink(missing_ok=True)

    addr_a = f"tcp:127.0.0.1:{free_port()}"
    proc_a, err_a = spawn(addr_a, extra_args=["--ppm", str(ppm)],
                          size=(W, H))
    try:
        s = dial(addr_a, idl)
        session_handshake(s)
        scene_up(s, 0xFFCC2222)
        wait_pixels(ppm, rgb(0xFFCC2222), "scene on server a")
        s.sock.close()
    finally:
        proc_a.terminate()
        proc_a.wait(timeout=5)
        err_a.close()

    ppm.unlink(missing_ok=True)
    addr_b = f"tcp:127.0.0.1:{free_port()}"
    proc_b, err_b = spawn(addr_b, extra_args=["--ppm", str(ppm)],
                          size=(W, H))
    try:
        s = dial(addr_b, idl)
        session_handshake(s)
        # the client rebuilds what it knows: same state, new server
        scene_up(s, 0xFFCC2222)
        wait_pixels(ppm, rgb(0xFFCC2222), "scene rebuilt on server b")
        s.close()
    finally:
        proc_b.terminate()
        proc_b.wait(timeout=5)
        err_b.close()


def main():
    idl = load_idl(ROOT / "spec" / "x12-core.yaml")
    resume_keeps_the_session(idl)
    resume_window_closes(idl)
    client_side_migration(idl)
    print("e2e sessions: PASS, resume keeps the scene, the window "
          "closes on deadline, the client migrates its state")


if __name__ == "__main__":
    main()
