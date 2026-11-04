# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# bridge isolation e2e: x11 windows cannot see each other. a foreign
# client listing the root children gets nothing, a granted process
# sees the whole tree, and xeyes stops following the cursor the moment
# the pointer leaves its window - the query pointer cloak freezes it

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tests import core_client as cc  # noqa: E402

SOCK = ROOT / "build" / "x12-isolate.sock"
PPM = ROOT / "build" / "framebuffer-isolate.ppm"
OUT = ROOT / "build" / "isolate.stdout"
FIFO = Path("/tmp/x12-isolate-input")
BRIDGE = ROOT / "build" / "vendor" / "xserver" / "build" / "hw" / "x12" / "Xx12"

W, H = 800, 480
# the xeyes window: 150x100+300+200, the region below covers it whole
X, Y, CW, CH = 292, 192, 176, 128


def free_display():
    for n in range(90, 140):
        if not Path(f"/tmp/.X11-unix/X{n}").exists():
            return n
    raise AssertionError("no free x display in 90..139")


def wait_file(path, pattern, deadline_s):
    end = time.monotonic() + deadline_s
    while time.monotonic() < end:
        try:
            if pattern in path.read_text(errors="replace"):
                return
        except FileNotFoundError:
            pass
        time.sleep(0.05)
    body = path.read_text(errors="replace") if path.exists() else "<missing>"
    raise AssertionError(f"timeout waiting for {pattern!r}, got:\n{body[-2000:]}")


def snap():
    # the ppm is rewritten in place, a read must land on a complete
    # frame or retry
    for _ in range(50):
        try:
            raw = PPM.read_bytes()
        except FileNotFoundError:
            time.sleep(0.05)
            continue
        head, sep, body = raw.partition(b"255\n")
        if sep and len(body) == W * H * 3:
            rows = []
            for row in range(CH):
                off = ((Y + row) * W + X) * 3
                rows.append(body[off:off + CW * 3])
            return b"".join(rows)
        time.sleep(0.05)
    raise AssertionError("ppm never held a complete frame")


def open_fifo():
    for _ in range(250):
        try:
            return os.open(str(FIFO), os.O_WRONLY | os.O_NONBLOCK)
        except OSError:
            time.sleep(0.02)
    raise AssertionError("input fifo never opened")


def run_session(grants):
    PPM.unlink(missing_ok=True)
    OUT.unlink(missing_ok=True)
    FIFO.unlink(missing_ok=True)
    os.mkfifo(FIFO)
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           extra_args=("--input", str(FIFO),
                                       "--ppm", str(PPM),
                                       "--width", str(W), "--height", str(H)))
    wfd = open_fifo()

    def send(line):
        os.write(wfd, (line + "\n").encode())

    display = free_display()
    xsock = Path(f"/tmp/.X11-unix/X{display}")
    f = open(OUT, "wb")
    bridge = None
    apps = []
    env = {"PATH": "/usr/bin:/bin",
           "X12_BRIDGE_SOCKET": str(SOCK),
           "X12_BRIDGE_WIDTH": str(W),
           "X12_BRIDGE_HEIGHT": str(H)}
    if grants:
        env["X12_BRIDGE_GRANTS"] = grants
    try:
        bridge = subprocess.Popen(
            [str(BRIDGE), f":{display}", "-ac", "-noreset"],
            stdout=f, stderr=f, env=env)
        wait_file(OUT, "X12-BRIDGE: screen ready", 15)

        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not xsock.exists():
            time.sleep(0.05)
        assert xsock.exists(), "x socket never appeared"

        xd = dict(os.environ, DISPLAY=f":{display}")
        apps.append(subprocess.Popen(["xlogo", "-geometry", "80x80+20+20"],
                                     env=xd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL))
        apps.append(subprocess.Popen(["xclock", "-geometry", "80x80+130+20"],
                                     env=xd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL))
        apps.append(subprocess.Popen(["xeyes", "-geometry", "150x100+300+200"],
                                     env=xd, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.DEVNULL))
        time.sleep(1.5)
        yield display, xd, send
    finally:
        for a in apps:
            a.terminate()
        if bridge:
            bridge.terminate()
            try:
                bridge.wait(timeout=5)
            except subprocess.TimeoutExpired:
                bridge.kill()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        f.close()
        FIFO.unlink(missing_ok=True)


def main():
    SOCK.unlink(missing_ok=True)

    # session one: no grants. the foreign view is empty and xeyes is
    # frozen by the query pointer cloak
    for display, xd, send in run_session(None):
        tree = subprocess.run(["xwininfo", "-display", f":{display}",
                               "-root", "-children"],
                              capture_output=True, text=True, timeout=10)
        assert tree.returncode == 0, f"xwininfo failed: {tree.stderr[-300:]}"
        assert "0 children" in tree.stdout, \
            f"the root listing leaked foreign windows:\n{tree.stdout[:800]}"

        # xeyes is alive: two cursor spots inside the eye window, both
        # clear of the right eye box - the pupil inside that box must
        # swing between them. the cursor sprite never enters the box
        send("motion 305 205")
        time.sleep(0.6)
        leftlook = snap()
        send("motion 445 295")
        time.sleep(0.6)
        rightlook = snap()
        # the right eye box in region coordinates
        eye = b"".join(leftlook[(23 + r) * CW * 3 + 93 * 3:
                                (23 + r) * CW * 3 + 143 * 3]
                       for r in range(68))
        eye2 = b"".join(rightlook[(23 + r) * CW * 3 + 93 * 3:
                                  (23 + r) * CW * 3 + 143 * 3]
                        for r in range(68))
        assert eye != eye2, "xeyes drew nothing, the input path is dead"

        # the cursor leaves for two far corners: the pupils must
        # freeze, neither corner sprite is inside the snap region so
        # the two frames compare clean
        send("motion 30 30")
        time.sleep(1.6)
        frozen_a = snap()
        send("motion 760 440")
        time.sleep(1.6)
        frozen_b = snap()
        assert frozen_a == frozen_b, \
            "xeyes still tracks the cursor outside its window"

    # session two: xwininfo is granted, the same query sees the world
    for display, xd, send in run_session("xwininfo"):
        tree = subprocess.run(["xwininfo", "-display", f":{display}",
                               "-root", "-children"],
                              capture_output=True, text=True, timeout=10)
        assert tree.returncode == 0, f"xwininfo failed: {tree.stderr[-300:]}"
        assert "xlogo" in tree.stdout, \
            f"the granted listing missed the app windows:\n{tree.stdout[:800]}"

    print("e2e bridge isolation: PASS, foreign view cloaked, "
          "grant opens the tree, xeyes frozen outside")


if __name__ == "__main__":
    main()
