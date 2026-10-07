#!/usr/bin/env python3
# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

"""end to end smoke for the kms backend on a real drm node.

needs a drm node without a session on it (a spare connector or a vkms
module), X12_KMS_NODE points at it. the smoke drives real page flips
through the dumb chain: map a window, pump commits, and watch the
present feedback and the flip pipeline stay honest under load. without
a node the smoke skips like every hardware test here."""

import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOCK = ROOT / "build" / "x12-e2e-kms.sock"
NODE = os.environ.get("X12_KMS_NODE", "")


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
    if not NODE or not Path(NODE).exists():
        print(f"e2e kms: SKIP (no drm node, set X12_KMS_NODE)")
        return 0

    idl = load_idl(ROOT / "spec" / "x12-core.yaml")
    print(f"e2e kms on {NODE}:")
    log = open(ROOT / "build" / "x12-e2e-kms.log", "wb")
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           ["--kms", NODE])
    try:
        s = cc.Session(SOCK, idl)
        handshake(s)

        stage("the output reports the real screen kms drives")
        bind(s, "x12_output", 2, 3)
        mode = s.wait_for("mode")
        w, h = mode["args"]["width"], mode["args"]["height"]
        assert w > 0 and h > 0, mode
        assert mode["args"]["refresh_millihz"] > 0, mode
        s.wait_for("geometry")
        s.wait_for("scale")
        name = s.wait_for("name")["args"]["name"]
        print("  - mode %ux%u refresh %u name %s" % (
            w, h, mode["args"]["refresh_millihz"], name))

        stage("a mapped window earns present feedback through the chain")
        bind(s, "x12_compositor", 2, 10)
        bind(s, "x12_buffer_manager", 1, 12)
        s.request("x12_compositor", "create_surface", 10, id=11)
        s.register("x12_surface", 11)
        memfd = os.memfd_create("x12-kms")
        os.ftruncate(memfd, 64 * 64 * 4)
        os.lseek(memfd, 0, 0)
        os.write(memfd, b"\x44\x33\x22\xff" * (64 * 64))
        s.request("x12_buffer_manager", "create_shm_pool", 12,
                  fds=[memfd], fd=0, size=64 * 64 * 4, id=20)
        os.close(memfd)
        s.register("x12_shm_pool", 20)
        s.request("x12_shm_pool", "create_buffer", 20, id=21, offset=0,
                  width=64, height=64, stride=64 * 4, format=0x34325258)
        s.register("x12_buffer", 21)
        s.request("x12_surface", "set_buffer", 11, buffer=21)
        s.request("x12_compositor", "create_toplevel", 10, id=22, surface=11)
        s.register("x12_toplevel", 22)
        ev = s.wait_for("configure")
        s.request("x12_toplevel", "ack_configure", 22,
                  serial=ev["args"]["serial"])
        s.request("x12_surface", "damage", 11, x=0, y=0, width=64, height=64)
        s.request("x12_surface", "commit", 11)
        t0 = s.wait_for("committed")["args"]["token"]
        s.request("x12_surface", "commit", 11)
        s.wait_for("committed")
        pr = s.wait_for("present")
        assert pr["args"]["commit_token"] == t0, (pr, t0)
        assert pr["args"]["kind"] == "copy", pr
        print("  - present token %d through the dumb chain" % t0)

        stage("the chain survives a paced commit storm with flips")
        # one commit per loop round: every frame reaches the screen and
        # every commit earns its present in commit order
        for i in range(20):
            s.request("x12_surface", "damage", 11, x=0, y=0, width=64,
                      height=64)
            s.request("x12_surface", "commit", 11)
            time.sleep(0.02)
        s.request("x12_surface", "commit", 11)
        time.sleep(0.5)
        evs = s.drain()
        tokens = [e["args"]["token"] for e in evs if e["name"] == "committed"]
        seen = [e["args"]["commit_token"] for e in evs
                if e["name"] == "present"]
        assert len(tokens) == 21, len(tokens)
        assert seen == tokens[:20], (seen, tokens)
        print("  - %d presents over %d commits, order held" % (
            len(seen), len(tokens) - 1))

        stage("a dropped frame stays silent, its present never lies")
        # commits batched into one loop round composite only the last
        # state, the frames in between never reach the screen and must
        # not report a scanout moment they never had
        for i in range(10):
            s.request("x12_surface", "damage", 11, x=0, y=0, width=64,
                      height=64)
            s.request("x12_surface", "commit", 11)
        s.request("x12_surface", "commit", 11)
        time.sleep(0.5)
        evs = s.drain()
        batch = [e["args"]["token"] for e in evs if e["name"] == "committed"]
        shown = [e["args"]["commit_token"] for e in evs
                 if e["name"] == "present"]
        assert len(batch) == 11, len(batch)
        assert shown == [t for t in batch if t in shown], (shown, batch)
        assert set(shown) <= set(batch), (shown, batch)
        assert len(shown) <= 3, shown  # the batch drops most frames
        print("  - %d honest presents for %d batched commits" % (
            len(shown), len(batch) - 1))

        stage("the server is still alive and sane after the storm")
        s.request("x12_connection", "sync", 1, callback=23)
        s.register("x12_callback", 23)
        s.wait_for("done")
        assert s.closed_by_server() is False
        print("  - alive")

        s.close()
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        log.close()
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
