# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# end to end for buffers and surfaces: shm pools with carves and budgets,
# the commit state machine and the release credit model. the release
# asserts are the resize_storm lesson: a credit belongs to the buffer that
# earned it, never to whatever the surface shows later

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOCK = ROOT / "build" / "x12-buffers.sock"

XR24 = 0x34325258
AR24 = 0x34325241
POOL_LIMIT = 256 * 1024 * 1024


def handshake(s):
    s.request("x12_connection", "hello", 1, idl_major=1, idl_minor=0)
    s.wait_for("welcome")
    for _ in range(4):
        s.wait_for("global")


def bind(s, name, version, id):
    s.request("x12_registry", "bind", 2, name=name, version=version, id=id, instance=0)
    s.register(name, id)


def make_pool(s, size, id):
    memfd = os.memfd_create("x12-pool")
    os.ftruncate(memfd, size)
    s.request("x12_buffer_manager", "create_shm_pool", 11,
              fds=[memfd], fd=0, size=size, id=id)
    os.close(memfd)
    s.register("x12_shm_pool", id)


def make_buffer(s, pool, id, offset, w, h, stride, fmt):
    s.request("x12_shm_pool", "create_buffer", pool, id=id,
              offset=offset, width=w, height=h, stride=stride, format=fmt)
    s.register("x12_buffer", id)


def no_errors(s):
    for out in s.drain():
        assert out["name"] != "error", out


def main():
    idl = load_idl(ROOT / "spec" / "x12-core.yaml")
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK)
    try:
        s = cc.Session(SOCK, idl)
        handshake(s)
        bind(s, "x12_compositor", 2, 10)
        bind(s, "x12_buffer_manager", 1, 11)

        # the buffer manager announces the formats it composites on
        fmts = set()
        for _ in range(2):
            fmts.add(s.wait_for("format")["args"]["fourcc"])
        assert fmts == {XR24, AR24}, fmts

        # pools and carves: bounds and format are checked wide, a bad
        # carve leaves the pool untouched
        make_pool(s, 16384, 20)
        make_buffer(s, 20, 21, 0, 4, 4, 16, XR24)
        no_errors(s)
        s.request("x12_shm_pool", "create_buffer", 20, id=25,
                  offset=16384, width=4, height=4, stride=16, format=XR24)
        err = s.wait_for("error")
        assert err["args"]["code"] == "bad-value", err
        s.request("x12_shm_pool", "create_buffer", 20, id=25,
                  offset=0, width=4, height=4, stride=16, format=0x11223344)
        err = s.wait_for("error")
        assert err["args"]["code"] == "bad-value", err
        s.request("x12_shm_pool", "create_buffer", 20, id=25,
                  offset=0, width=4, height=4, stride=2, format=XR24)
        err = s.wait_for("error")
        assert err["args"]["code"] == "bad-value", err

        # the memory budget refuses pools past the limit and a destroy
        # gives the budget back
        make_pool(s, POOL_LIMIT - 16384, 22)
        no_errors(s)
        make_pool(s, 4096, 23)
        err = s.wait_for("error")
        assert err["args"]["code"] == "out-of-memory", err
        s.request("x12_shm_pool", "destroy", 22)
        make_pool(s, 4096, 23)
        no_errors(s)

        # dmabuf import needs a real dma buffer, anything else is turned
        # away loudly instead of becoming a buffer behind the scenes
        s.request("x12_buffer_manager", "import_dmabuf", 11, fd=0, width=4,
                  height=4, format=XR24, modifier=0, stride=16, id=26)
        err = s.wait_for("error")
        assert err["args"]["code"] == "bad-value", err

        # commit machine and the release credit model: the credit names
        # the buffer that earned it
        make_buffer(s, 20, 24, 256, 4, 4, 16, XR24)
        s.request("x12_compositor", "create_surface", 10, id=30)
        s.register("x12_surface", 30)
        s.request("x12_surface", "set_buffer", 30, buffer=21)
        s.request("x12_surface", "damage", 30, x=0, y=0, width=4, height=4)
        s.request("x12_surface", "commit", 30)
        com = s.wait_for("committed")
        assert com["args"]["token"] == 1, com
        assert com["object"] == 30, com
        for out in s.drain():
            assert out["name"] != "release", out

        s.request("x12_surface", "set_buffer", 30, buffer=24)
        s.request("x12_surface", "commit", 30)
        assert s.wait_for("committed")["args"]["token"] == 2
        rel = s.wait_for("release")
        assert rel["object"] == 21, rel

        s.request("x12_surface", "set_buffer", 30, buffer=21)
        s.request("x12_surface", "commit", 30)
        assert s.wait_for("committed")["args"]["token"] == 3
        rel = s.wait_for("release")
        assert rel["object"] == 24, rel

        s.request("x12_surface", "set_buffer", 30, buffer=0)
        s.request("x12_surface", "commit", 30)
        assert s.wait_for("committed")["args"]["token"] == 4
        rel = s.wait_for("release")
        assert rel["object"] == 21, rel

        # explicit sync: the frame's own fence gates its own credit, an
        # unsignaled fence holds the release back
        fr, fw = os.pipe()
        s.request("x12_surface", "set_buffer", 30, buffer=24)
        s.request("x12_surface", "set_commit_fence", 30, fds=[fr], fence=0)
        s.request("x12_surface", "commit", 30)
        assert s.wait_for("committed")["args"]["token"] == 5
        s.request("x12_surface", "set_buffer", 30, buffer=21)
        s.request("x12_surface", "commit", 30)
        assert s.wait_for("committed")["args"]["token"] == 6
        for out in s.drain():
            assert out["name"] != "release", out
        os.write(fw, b"\x00")
        rel = s.wait_for("release")
        assert rel["object"] == 24, rel
        os.close(fr)
        os.close(fw)

        # frame callbacks ride their own frame and die with it
        s.request("x12_surface", "frame", 30, callback=5)
        s.register("x12_callback", 5)
        s.request("x12_surface", "set_buffer", 30, buffer=24)
        s.request("x12_surface", "commit", 30)
        s.request("x12_surface", "set_buffer", 30, buffer=21)
        s.request("x12_surface", "commit", 30)
        done = s.wait_for("done")
        assert done["object"] == 5, done

        # a surface that dies gives every buffer credit back at once
        s.request("x12_surface", "set_buffer", 30, buffer=24)
        s.request("x12_surface", "commit", 30)
        s.request("x12_surface", "destroy", 30)
        seen = set()
        while 24 not in seen:
            rel = s.wait_for("release")
            seen.add(rel["object"])
        s.close()

        # the retire queue is a limit: commits faster than fences signal
        # are a dead fence in disguise and die loudly
        s = cc.Session(SOCK, idl)
        handshake(s)
        bind(s, "x12_compositor", 2, 10)
        s.request("x12_compositor", "create_surface", 10, id=30)
        s.register("x12_surface", 30)
        for i in range(10):
            fr, fw = os.pipe()
            s.request("x12_surface", "set_commit_fence", 30, fds=[fr], fence=0)
            os.close(fr)
            os.close(fw)
            s.request("x12_surface", "commit", 30)
        assert s.closed_by_server(), "retire backlog did not disconnect"
        s.close()
    finally:
        proc.terminate()
        proc.wait(timeout=5)
    print("e2e buffers: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
