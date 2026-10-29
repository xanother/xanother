# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# bridge multi output e2e: the session advertises two tiled outputs,
# the bridge maps every instance onto its own rr output and sizes the
# dix root to the union canvas. xrandr must read the real monitor
# layout with origins, and a red root paints both heads of the server
# frame, the head boundary must not clip the x screen

import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tests import core_client as cc  # noqa: E402

SOCK = ROOT / "build" / "x12-bridge-multi.sock"
PPM = ROOT / "build" / "framebuffer-bridge-multi.ppm"
FIFO = ROOT / "build" / "input-bridge-multi.fifo"
OUT = ROOT / "build" / "bridge-multi.stdout"
BRIDGE = ROOT / "build" / "vendor" / "xserver" / "build" / "hw" / "x12" / "Xx12"

# the headless bench runs two heads of W x H, they tile into the
# 2W x H canvas the root covers
W, H = 400, 300


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


def ppm_pixels():
    try:
        raw = PPM.read_bytes()
        head, _, rest = raw.partition(b"255\n")
        dims = head.split(b"\n")[1].split()
        return (int(dims[0]), int(dims[1])), rest
    except (IndexError, ValueError):
        return None, None


def head_red(head):
    # red pixel count of one head strip of the server frame
    d, pix = ppm_pixels()
    if d != (2 * W, H) or pix is None or len(pix) != 2 * W * H * 3:
        return 0
    x0 = head * W
    n = 0
    for y in range(0, H, 4):
        for x in range(x0, x0 + W, 4):
            i = (y * 2 * W + x) * 3
            if pix[i] > 200 and pix[i + 1] < 60 and pix[i + 2] < 60:
                n += 1
    return n


def main():
    PPM.unlink(missing_ok=True)
    OUT.unlink(missing_ok=True)
    FIFO.unlink(missing_ok=True)
    os.mkfifo(FIFO)
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           extra_args=("--input", str(FIFO),
                                       "--ppm", str(PPM),
                                       "--width", str(W),
                                       "--height", str(H),
                                       "--heads", "2"))
    wfd = open_fifo()
    display = free_display()
    xsock = Path(f"/tmp/.X11-unix/X{display}")
    f = open(OUT, "wb")
    bridge = None
    try:
        # the vrr wish must reach every output of the session even on
        # hosts that refuse it later
        bridge = subprocess.Popen(
            [str(BRIDGE), f":{display}", "-ac", "-noreset"],
            stdout=f, stderr=f,
            env={"PATH": "/usr/bin:/bin",
                 "X12_BRIDGE_SOCKET": str(SOCK),
                 "X12_BRIDGE_VRR": "1"})
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not xsock.exists():
            time.sleep(0.05)
        assert xsock.exists(), "x socket never appeared"

        # the root spans the union canvas of the two heads
        def dims():
            info = subprocess.run(["xdpyinfo", "-display", f":{display}"],
                                  capture_output=True, timeout=10)
            for line in info.stdout.decode(errors="replace").split("\n"):
                if "dimensions" in line:
                    return line.split()[1]
            return "<none>"

        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if dims() == f"{2 * W}x{H}":
                break
            time.sleep(0.2)
        assert dims() == f"{2 * W}x{H}", dims()

        # every session monitor rides its own rr output at its origin
        def xrandr_layout():
            info = subprocess.run(["xrandr", "-display", f":{display}"],
                                  capture_output=True, timeout=10)
            assert info.returncode == 0, \
                f"xrandr failed: {info.stderr.decode(errors='replace')[-400:]}"
            outs = []
            for line in info.stdout.decode(errors="replace").split("\n"):
                m = re.search(r"^(\S+) connected (\d+)x(\d+)\+(\d+)\+(\d+)",
                              line)
                if m:
                    outs.append((m.group(1),
                                 (int(m.group(4)), int(m.group(5)))))
            return outs

        deadline = time.monotonic() + 10
        layout = []
        while time.monotonic() < deadline:
            try:
                layout = xrandr_layout()
            except AssertionError:
                layout = []
            if len(layout) == 2:
                break
            time.sleep(0.2)
        assert len(layout) == 2, layout
        names = [o[0] for o in layout]
        pos = [o[1] for o in layout]
        assert names == ["X12-1", "X12-2"], \
            f"rr output names wrong: {layout}"
        assert pos == [(0, 0), (W, 0)], f"rr output origins wrong: {layout}"

        # a red root background paints both heads of the server frame,
        # the head boundary clips nothing
        subprocess.run(["xsetroot", "-display", f":{display}",
                        "-solid", "red"],
                       check=True, capture_output=True, timeout=10)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if head_red(0) >= 100 and head_red(1) >= 100:
                break
            time.sleep(0.2)
        assert head_red(0) >= 100 and head_red(1) >= 100, \
            f"red root on both heads: head0={head_red(0)} head1={head_red(1)}"

        # the vrr wish went out on every bound output
        deadline = time.monotonic() + 5
        log = ""
        while time.monotonic() < deadline:
            log = OUT.read_text(errors="replace")
            if "vrr requested on 2 of 2 outputs" in log:
                break
            time.sleep(0.1)
        assert "vrr requested on 2 of 2 outputs" in log, log[-500:]

        assert bridge.poll() is None, "bridge died during the multiout e2e"
        assert proc.poll() is None, "x12 server died during the multiout e2e"
        print("e2e bridge multiout: PASS, xrandr reads two outputs at "
              "their origins and the canvas root paints both heads")
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
