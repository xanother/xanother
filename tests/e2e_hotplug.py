# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# hotplug and multi head e2e: builds a vkms display with two virtual
# connectors through configfs and moves the cables under the running
# server. the outputs must announce themselves, keep their instance
# identity across unplug and replug, present only on the screens that
# carried the pixels and refuse vrr on hardware without it. without
# configfs vkms the whole run prints SKIP and keeps the gate green

import glob
import os
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402
from tests.e2e_scanout import make_dmabuf, make_window  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOCK = ROOT / "build" / "x12-e2e-hotplug.sock"
CFG = Path("/sys/kernel/config/vkms")

CONNECTED, DISCONNECTED = "1", "2"


def stage(msg):
    print(f"  - {msg}")


BACKLOG = []


def readback(s):
    BACKLOG.extend(s.drain())


def wait_ev(s, name, obj=None, timeout=8.0):
    # like the session wait_for but every other event stays in the
    # backlog: presents of the other output must never get dropped
    deadline = time.time() + timeout
    while True:
        for e in BACKLOG:
            if e["name"] == name and (obj is None or e["object"] == obj):
                BACKLOG.remove(e)
                return e
        if time.time() > deadline:
            raise AssertionError(
                f"timeout waiting for {name} obj={obj}, backlog={BACKLOG}")
        readback(s)
        time.sleep(0.02)


def expect_no_token(s, token, timeout=0.5):
    # the commit token names one frame, no other screen may present it
    time.sleep(timeout)
    readback(s)
    for e in BACKLOG:
        assert not (e["name"] == "present" and
                    e["args"]["commit_token"] == token), e


def expect_no(s, name, obj, timeout=0.5):
    time.sleep(timeout)
    readback(s)
    for e in BACKLOG:
        assert not (e["name"] == name and e["object"] == obj), e


# --- the vkms configfs display under test -----------------------------

def vkms_build(name, statuses):
    # one pipeline item set per connector: primary plane, crtc, encoder
    # and the connector itself, linked and enabled at the end
    root = CFG / name
    root.mkdir()
    n = len(statuses)
    for i in range(n):
        (root / "planes" / f"plane{i}").mkdir()
        (root / "crtcs" / f"crtc{i}").mkdir()
        (root / "encoders" / f"encoder{i}").mkdir()
        (root / "connectors" / f"connector{i}").mkdir()
    for i in range(n):
        os.symlink(f"{root}/crtcs/crtc{i}",
                   f"{root}/planes/plane{i}/possible_crtcs/crtc{i}")
        os.symlink(f"{root}/crtcs/crtc{i}",
                   f"{root}/encoders/encoder{i}/possible_crtcs/crtc{i}")
        os.symlink(f"{root}/encoders/encoder{i}",
                   f"{root}/connectors/connector{i}"
                   f"/possible_encoders/encoder{i}")
    for i in range(n):
        (root / "planes" / f"plane{i}" / "type").write_text("1\n")
        (root / "connectors" / f"connector{i}" / "status").write_text(
            statuses[i] + "\n")
    (root / "enabled").write_text("1\n")


def vkms_status(name, i, status):
    (CFG / name / "connectors" / f"connector{i}" / "status").write_text(
        status + "\n")


def vkms_destroy(name):
    root = CFG / name
    if not root.exists():
        return
    (root / "enabled").write_text("0\n")
    # the default groups inside each item die with their parent, only
    # the links and the items themselves are torn down by hand
    # every link first: the items stay busy while anything points at
    # them, then the items in dependency order
    for sub in ("planes", "crtcs", "encoders", "connectors"):
        for link in (root / sub).rglob("*"):
            if link.is_symlink():
                link.unlink()
    for sub in ("connectors", "encoders", "crtcs", "planes"):
        for p in sorted((root / sub).glob("*"), reverse=True):
            p.rmdir()
    root.rmdir()


def find_new_card(before):
    for _ in range(50):
        after = {Path(p).name for p in glob.glob("/sys/class/drm/card*")}
        fresh = sorted(
            n for n in after - before
            if re.fullmatch(r"card\d+", n) and
            Path(f"/dev/dri/{n}").exists())
        if fresh:
            return f"/dev/dri/{fresh[-1]}"
        time.sleep(0.1)
    return None


def bind(s, name, version, id, instance=0):
    s.request("x12_registry", "bind", 2, name=name, version=version, id=id,
              instance=instance)
    s.register(name, id)


def handshake(s):
    s.request("x12_connection", "hello", 1, idl_major=1, idl_minor=0)
    s.wait_for("welcome")
    return [s.wait_for("global") for _ in range(5)]


def map_toplevel(s, surface, role):
    s.request("x12_compositor", "create_toplevel", 10, id=role,
              surface=surface)
    s.register("x12_toplevel", role)
    ev = wait_ev(s, "configure", role)
    s.request("x12_toplevel", "ack_configure", role,
              serial=ev["args"]["serial"])


def commit_twice(s, surface):
    # the second commit retires the first frame, its present is what the
    # test reads. the pause lets the composite pass paint the first
    # frame, two commits in one loop turn replace it unpainted and
    # frames never painted stay silent by design
    s.request("x12_surface", "commit", surface)
    time.sleep(0.05)
    s.request("x12_surface", "commit", surface)


def main():
    if not CFG.exists():
        print("e2e hotplug: no vkms configfs, SKIP")
        return 0
    print("e2e hotplug:")
    name = "x12t"
    vkms_destroy(name)
    before = {Path(p).name for p in glob.glob("/sys/class/drm/card*")}
    vkms_build(name, [CONNECTED, DISCONNECTED])
    node = find_new_card(before)
    if not node:
        print("  - no vkms card appeared, SKIP")
        vkms_destroy(name)
        return 0
    print(f"  - vkms display up on {node}")

    idl = load_idl(ROOT / "spec" / "x12-core.yaml")
    log = open("/tmp/x12-e2e-hotplug.log", "wb")
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           ["--kms", node], stderr=log)
    try:
        s = cc.Session(SOCK, idl)
        globals_ = handshake(s)

        stage("one connected head announces one output instance")
        outs = [g for g in globals_ if g["args"]["name"] == "x12_output"]
        assert [g["args"]["instance"] for g in outs] == [0], outs
        bind(s, "x12_compositor", 2, 10)
        bind(s, "x12_buffer_manager", 1, 12)
        bind(s, "x12_output", 2, 3, instance=0)
        m0 = wait_ev(s, "mode", 3)
        g0 = wait_ev(s, "geometry", 3)
        wait_ev(s, "scale", 3)
        wait_ev(s, "name", 3)
        assert g0["args"]["x"] == 0 and g0["args"]["y"] == 0, g0
        w0, h0 = m0["args"]["width"], m0["args"]["height"]
        print(f"  - output 0: {w0}x{h0} at {g0['args']['x']},{g0['args']['y']}")

        stage("a down head refuses binds")
        s.request("x12_registry", "bind", 2, name="x12_output", version=2,
                  id=4, instance=1)
        err = wait_ev(s, "error")
        assert err["args"]["code"] == "bad-value", err

        stage("a fresh cable announces its instance to everybody")
        vkms_status(name, 1, CONNECTED)
        g = wait_ev(s, "global")
        while g["args"]["name"] != "x12_output":
            g = wait_ev(s, "global")
        assert g["args"]["instance"] == 1, g
        bind(s, "x12_output", 2, 4, instance=1)
        m1 = wait_ev(s, "mode", 4)
        g1 = wait_ev(s, "geometry", 4)
        wait_ev(s, "scale", 4)
        wait_ev(s, "name", 4)
        assert g1["args"]["x"] == w0, g1
        print(f"  - output 1: {m1['args']['width']}x{m1['args']['height']} "
              f"at {g1['args']['x']},{g1['args']['y']}")

        stage("presents follow the screen that carried the pixels")
        # a window cascades near the origin, its frames present only on
        # the output whose rect shows them
        make_window(s, 20, 22, 21, 200, 150, memfd=os.memfd_create("w"))
        map_toplevel(s, 20, 23)
        commit_twice(s, 20)
        pr = wait_ev(s, "present", 3)
        assert pr["args"]["kind"] == "copy", pr
        expect_no_token(s, pr["args"]["commit_token"])
        tok_w = pr["args"]["commit_token"]

        # a popup pinned past the first head shows on the second one.
        # the popup centers on anchor center plus the offset, so the
        # offset aims its middle at the right head
        make_window(s, 24, 26, 25, 100, 100, memfd=os.memfd_create("p"))
        s.request("x12_compositor", "create_positioner", 10, id=28)
        s.register("x12_positioner", 28)
        s.request("x12_positioner", "set_anchor_rect", 28,
                  x=0, y=0, width=1, height=1)
        s.request("x12_positioner", "set_anchor", 28, anchor="center")
        s.request("x12_positioner", "set_gravity", 28, gravity="center")
        s.request("x12_positioner", "set_offset", 28, x=w0 + 28, y=50)
        s.request("x12_compositor", "create_popup", 10, id=27,
                  surface=24, parent=20, positioner=28)
        s.register("x12_popup", 27)
        ev = wait_ev(s, "configure", 27)
        s.request("x12_popup", "ack_configure", 27,
                  serial=ev["args"]["serial"])
        commit_twice(s, 24)
        pr = wait_ev(s, "present", 4)
        assert pr["args"]["kind"] == "copy", pr
        expect_no_token(s, pr["args"]["commit_token"])
        print(f"  - window token {tok_w} on output 0, popup token "
              f"{pr['args']['commit_token']} on output 1")

        stage("pulling the cable says removed and keeps the session")
        vkms_status(name, 1, DISCONNECTED)
        rem = wait_ev(s, "removed")
        assert rem["object"] == 4, rem
        expect_no(s, "removed", 3)
        commit_twice(s, 20)
        pr = wait_ev(s, "present", 3)
        assert pr["args"]["kind"] == "copy", pr
        expect_no_token(s, pr["args"]["commit_token"])

        stage("the replugged head keeps its instance and re-describes")
        vkms_status(name, 1, CONNECTED)
        g = wait_ev(s, "global")
        while g["args"]["name"] != "x12_output":
            g = wait_ev(s, "global")
        assert g["args"]["instance"] == 1, g
        m = wait_ev(s, "mode", 4)
        assert m["args"]["width"] == m1["args"]["width"], m
        wait_ev(s, "geometry", 4)
        commit_twice(s, 20)
        pr = wait_ev(s, "present", 3)
        assert pr["args"]["kind"] == "copy", pr

        stage("vrr refuses loudly on hosts without it")
        s.request("x12_output", "set_vrr", 3, enable=2)
        err = wait_ev(s, "error")
        assert err["args"]["code"] == "bad-value", err
        s.request("x12_output", "set_vrr", 3, enable=1)
        err = wait_ev(s, "error")
        assert err["args"]["code"] == "bad-value", err

        stage("the fullscreen dma frame flips to its own head")
        # clear the stack first, scanout wants exactly one window
        s.request("x12_toplevel", "destroy", 23)
        s.request("x12_popup", "destroy", 27)
        dmabuf = make_dmabuf(node, w0, h0, b"\x22\x11\xcc\xff")
        make_window(s, 5, 0, 6, w0, h0, dmabuf=dmabuf)
        os.close(dmabuf)
        map_toplevel(s, 5, 7)
        s.request("x12_toplevel", "set_fullscreen", 7, state=1)
        ev = wait_ev(s, "configure", 7)
        s.request("x12_toplevel", "ack_configure", 7,
                  serial=ev["args"]["serial"])
        # scanout rides fresh damage, the flip present lands on its own
        s.request("x12_surface", "damage", 5, x=0, y=0, width=w0, height=h0)
        s.request("x12_surface", "commit", 5)
        pr = wait_ev(s, "present", 3)
        assert pr["args"]["kind"] == "flip", pr
        expect_no_token(s, pr["args"]["commit_token"])
        print("  - present token %d kind flip on output 0" %
              pr["args"]["commit_token"])

        s.close()
        print("e2e hotplug: PASS")
        return 0
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        log.close()
        vkms_destroy(name)


if __name__ == "__main__":
    sys.exit(main())
