# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# bridge randr e2e: the x12 output table of the session becomes the rr
# output table of the x screen. xrandr reads the connection state and
# the mode, a mode growth shows up as a fresh mode on the same output
# and a manual mode set from a client is accepted

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tests import core_client as cc  # noqa: E402

SOCK = ROOT / "build" / "x12-bridge-randr.sock"
PPM = ROOT / "build" / "framebuffer-bridge-randr.ppm"
FIFO = ROOT / "build" / "input-bridge-randr.fifo"
OUT = ROOT / "build" / "bridge-randr.stdout"
BRIDGE = ROOT / "build" / "vendor" / "xserver" / "build" / "hw" / "x12" / "Xx12"

W, H = 800, 480
W2, H2 = 900, 600


def free_display():
    for n in range(90, 140):
        if not Path(f"/tmp/.X11-unix/X{n}").exists():
            return n
    raise AssertionError("no free x display in 90..139")


def open_fifo():
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            return os.open(str(FIFO), os.O_WRONLY | os.O_NONBLOCK)
        except OSError:
            time.sleep(0.02)
    raise AssertionError("input fifo never opened")


def main():
    PPM.unlink(missing_ok=True)
    OUT.unlink(missing_ok=True)
    FIFO.unlink(missing_ok=True)
    os.mkfifo(FIFO)
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           extra_args=("--input", str(FIFO),
                                       "--ppm", str(PPM),
                                       "--width", str(W),
                                       "--height", str(H)))
    wfd = open_fifo()

    def send(line):
        os.write(wfd, (line + "\n").encode())

    display = free_display()
    xsock = Path(f"/tmp/.X11-unix/X{display}")
    f = open(OUT, "wb")
    bridge = None

    def xrandr():
        r = subprocess.run(["xrandr", "-display", f":{display}", "--query"],
                           capture_output=True, timeout=10)
        assert r.returncode == 0, \
            f"xrandr failed: {r.stderr.decode(errors='replace')[-300:]}"
        return r.stdout.decode(errors="replace")

    try:
        # no geometry env: the fullscreen offer follows the mode, the
        # root tracks the output one to one
        bridge = subprocess.Popen(
            [str(BRIDGE), f":{display}", "-ac", "-noreset"],
            stdout=f, stderr=f,
            env={"PATH": "/usr/bin:/bin", "X12_BRIDGE_SOCKET": str(SOCK),
                 # the manual mode set grabs the server, that is a
                 # manager move - the tool grant covers it
                 "X12_BRIDGE_GRANTS": "xrandr"})
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not xsock.exists():
            time.sleep(0.05)
        assert xsock.exists(), "x socket never appeared"

        deadline = time.monotonic() + 10
        out = ""
        while time.monotonic() < deadline:
            out = xrandr()
            if f"{W} x {H}" in out:
                break
            time.sleep(0.2)
        assert "X12-1 connected" in out, out
        assert f"current {W} x {H}" in out, out
        assert f"{W}x{H}" in out, out

        # the output grows, the mode follows the session
        send(f"resize {W2} {H2}")
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            out = xrandr()
            if f"{W2}x{H2}" in out:
                break
            time.sleep(0.2)
        assert f"current {W2} x {H2}" in out, out
        assert f"{W2}x{H2}" in out, out

        # a manual mode set by a client is accepted
        r = subprocess.run(["xrandr", "-display", f":{display}", "--output",
                            "X12-1", "--mode", f"{W2}x{H2}"],
                           capture_output=True, timeout=10)
        assert r.returncode == 0, \
            f"manual mode set refused: {r.stderr.decode(errors='replace')}"

        assert bridge.poll() is None, "bridge died during the randr e2e"
        assert proc.poll() is None, "x12 server died during the randr e2e"
        print("e2e bridge randr: PASS, X12-1 connected, mode "
              f"{W}x{H} -> {W2}x{H2}, manual set accepted")
    finally:
        if bridge:
            bridge.terminate()
            try:
                bridge.wait(timeout=3)
            except subprocess.TimeoutExpired:
                bridge.kill()
        f.close()
        os.close(wfd)
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        for leftover in (Path(f"/tmp/.X{display}-lock"), xsock, FIFO):
            leftover.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
