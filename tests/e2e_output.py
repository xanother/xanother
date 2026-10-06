#!/usr/bin/env python3
# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

"""end to end smoke for the output global and its presentation feedback.

the output global reports the screen and every frame that reaches the
screen comes back as present{commit_token, kind}. the headless backend
pushes frames as copies, so the feedback carries copy kind and the token
of the commit whose frame retired. the retire is double buffered: the
feedback for commit N lands after commit N+1."""

import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOCK = ROOT / "build" / "x12-e2e-output.sock"
PPM = ROOT / "build" / "x12-e2e-output.ppm"


def stage(msg):
    print(f"  - {msg}")


def handshake(s):
    s.request("x12_connection", "hello", 1, idl_major=1, idl_minor=0)
    s.wait_for("welcome")
    for _ in range(5):
        s.wait_for("global")


def bind(s, name, version, id):
    s.request("x12_registry", "bind", 2, name=name, version=version, id=id,
              instance=0)
    s.register(name, id)


def main():
    idl = load_idl(ROOT / "spec" / "x12-core.yaml")
    PPM.unlink(missing_ok=True)
    print("e2e output:")
    log = open("/tmp/x12-e2e-output.log", "wb")
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           ["--width", "320", "--height", "240",
                            "--ppm", str(PPM)])
    try:
        s = cc.Session(SOCK, idl)
        handshake(s)

        stage("output global binds and announces its screen")
        bind(s, "x12_output", 2, 3)
        mode = s.wait_for("mode")
        assert mode["object"] == 3, mode
        assert mode["args"]["width"] == 320 and mode["args"]["height"] == 240, mode
        assert mode["args"]["refresh_millihz"] == 60000, mode
        geom = s.wait_for("geometry")
        assert geom["object"] == 3, geom
        scale = s.wait_for("scale")
        assert scale["args"]["scale"] == 256, scale
        name = s.wait_for("name")
        assert name["args"]["name"], name
        print("  - mode %ux%u refresh %u name %s" % (
            mode["args"]["width"], mode["args"]["height"],
            mode["args"]["refresh_millihz"], name["args"]["name"]))

        stage("a real frame earns a present feedback with its commit token")
        bind(s, "x12_compositor", 2, 10)
        bind(s, "x12_buffer_manager", 1, 11)
        s.request("x12_compositor", "create_surface", 10, id=30)
        s.register("x12_surface", 30)
        memfd = os.memfd_create("x12-pool")
        os.ftruncate(memfd, 48 * 32 * 4)
        os.lseek(memfd, 0, 0)
        os.write(memfd, b"\x33\x22\x11\xff" * (48 * 32))
        s.request("x12_buffer_manager", "create_shm_pool", 11,
                  fds=[memfd], fd=0, size=48 * 32 * 4, id=20)
        os.close(memfd)
        s.register("x12_shm_pool", 20)
        s.request("x12_shm_pool", "create_buffer", 20, id=21,
                  offset=0, width=48, height=32, stride=48 * 4, format=0x34325258)
        s.register("x12_buffer", 21)
        s.request("x12_surface", "set_buffer", 30, buffer=21)
        # a roleless surface never reaches the canvas and earns no
        # feedback, the window needs its role before any present
        s.request("x12_compositor", "create_toplevel", 10, id=22, surface=30)
        s.register("x12_toplevel", 22)
        ev = s.wait_for("configure")
        s.request("x12_toplevel", "ack_configure", 22,
                  serial=ev["args"]["serial"])
        s.request("x12_surface", "damage", 30, x=0, y=0, width=48, height=32)
        s.request("x12_surface", "commit", 30)
        t0 = s.wait_for("committed")["args"]["token"]
        # the frame on screen retires at the next commit
        s.request("x12_surface", "commit", 30)
        s.wait_for("committed")
        pr = s.wait_for("present")
        assert pr["object"] == 3, pr
        assert pr["args"]["commit_token"] == t0, (pr, t0)
        assert pr["args"]["kind"] == "copy", pr  # the headless path copies
        assert pr["timestamp_ns"] > 0
        print("  - present token %d kind copy at ns %d" % (
            pr["args"]["commit_token"], pr["timestamp_ns"]))

        stage("each commit gets its own feedback in commit order")
        s.request("x12_surface", "damage", 30, x=0, y=0, width=48, height=32)
        s.request("x12_surface", "commit", 30)
        t1 = s.wait_for("committed")["args"]["token"]
        s.request("x12_surface", "commit", 30)
        s.wait_for("committed")
        pr2 = s.wait_for("present")
        assert pr2["args"]["commit_token"] == t1, (pr2, t1)
        assert t1 != t0
        print("  - second present token %d, tokens advance" % t1)

        stage("frame callback and present travel the same retirement")
        s.request("x12_surface", "frame", 30, callback=23)
        s.register("x12_callback", 23)
        s.request("x12_surface", "damage", 30, x=0, y=0, width=48, height=32)
        s.request("x12_surface", "commit", 30)
        t2 = s.wait_for("committed")["args"]["token"]
        s.request("x12_surface", "commit", 30)
        s.wait_for("committed")
        done = s.wait_for("done")
        assert done["object"] == 23
        pr3 = s.wait_for("present")
        assert pr3["args"]["commit_token"] == t2, (pr3, t2)
        print("  - done and present both landed for token %d" % t2)

        stage("a release drops the binding, presents stop")
        s.request("x12_output", "release", 3)
        s.request("x12_surface", "damage", 30, x=0, y=0, width=48, height=32)
        s.request("x12_surface", "commit", 30)
        s.wait_for("committed")
        s.request("x12_surface", "commit", 30)
        s.wait_for("committed")
        assert s.closed_by_server() is False
        extra = [e for e in s.drain() if e["name"] == "present"]
        assert extra == [], extra
        print("  - after release no more present events")

        stage("pixels still land in the ppm dump")
        s.request("x12_surface", "damage", 30, x=0, y=0, width=48, height=32)
        s.request("x12_surface", "commit", 30)
        time.sleep(0.3)

        def read_px():
            data = PPM.read_bytes()
            parts = data.split(b"255\n", 1)
            if len(parts) != 2:
                return None
            head, body = parts
            wh = head.split()
            w = int(wh[1])
            return (w, body)

        got = read_px()
        assert got, "ppm dump missing"
        w, body = got
        # the first toplevel cascades to 32,32, look inside the window
        o = ((32 + 4) * w + (32 + 4)) * 3
        assert body[o:o + 3] == b"\x11\x22\x33", body[o:o + 3]
        print("  - pixel check ok")

        s.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
