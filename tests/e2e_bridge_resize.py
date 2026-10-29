# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# bridge session resize e2e: the x12 output mode drives the x11 root,
# no explicit geometry env. an output growth moves the dix screen, the
# rootful surface re-commits at the new size and both sides agree:
# xdpyinfo sees the new root, the server frame carries the clock in
# the new geometry

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tests import core_client as cc  # noqa: E402

SOCK = ROOT / "build" / "x12-bridge-resize.sock"
PPM = ROOT / "build" / "framebuffer-bridge-resize.ppm"
FIFO = ROOT / "build" / "input-bridge-resize.fifo"
OUT = ROOT / "build" / "bridge-resize.stdout"
BRIDGE = ROOT / "build" / "vendor" / "xserver" / "build" / "hw" / "x12" / "Xx12"

# the fullscreen session covers the output exactly, both edges of the
# mode change move the root one to one
W1, H1 = 800, 480
W2, H2 = 900, 600


def free_display():
    # leftover x servers from crashed runs poison a fixed display
    # number, the test picks the first unused one instead
    for n in range(90, 140):
        if not Path(f"/tmp/.X11-unix/X{n}").exists():
            return n
    raise AssertionError("no free x display in 90..139")


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


def ppm_pixels():
    # the ppm is rewritten live, a torn read just means one more beat
    try:
        raw = PPM.read_bytes()
        head, _, rest = raw.partition(b"255\n")
        dims = head.split(b"\n")[1].split()
        return (int(dims[0]), int(dims[1])), rest
    except (IndexError, ValueError):
        return None, None


def main():
    PPM.unlink(missing_ok=True)
    OUT.unlink(missing_ok=True)
    FIFO.unlink(missing_ok=True)
    os.mkfifo(FIFO)
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           extra_args=("--input", str(FIFO),
                                       "--ppm", str(PPM),
                                       "--width", str(W1),
                                       "--height", str(H1)))
    wfd = open_fifo()

    def send(line):
        os.write(wfd, (line + "\n").encode())

    display = free_display()
    xsock = Path(f"/tmp/.X11-unix/X{display}")
    f = open(OUT, "wb")
    bridge = None
    clock = None

    def xdpyinfo_dims():
        info = subprocess.run(["xdpyinfo", "-display", f":{display}"],
                              capture_output=True, timeout=10)
        assert info.returncode == 0, \
            f"xdpyinfo failed: {info.stderr.decode(errors='replace')[-400:]}"
        for line in info.stdout.decode(errors="replace").split("\n"):
            if "dimensions" in line:
                return line.split()[1]
        return "<none>"

    def wait_for(fn, what, deadline_s=8):
        end = time.monotonic() + deadline_s
        while time.monotonic() < end:
            if fn():
                return True
            time.sleep(0.1)
        raise AssertionError(f"timeout waiting for {what}")

    def content_pixels(size):
        d, pix = ppm_pixels()
        if d != size or pix is None or len(pix) != size[0] * size[1] * 3:
            # a torn read mid rewrite carries a mismatched body
            return 0
        return sum(1 for i in range(0, len(pix), 3)
                   if pix[i] or pix[i + 1] or pix[i + 2])

    try:
        # no x12_bridge_width/height: the mode event drives the size
        bridge = subprocess.Popen(
            [str(BRIDGE), f":{display}", "-ac", "-noreset"],
            stdout=f, stderr=f,
            env={"PATH": "/usr/bin:/bin", "X12_BRIDGE_SOCKET": str(SOCK)})
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not xsock.exists():
            time.sleep(0.05)
        assert xsock.exists(), "x socket never appeared"

        wait_for(lambda: xdpyinfo_dims() == f"{W1}x{H1}",
                 f"the mode driven {W1}x{H1} root")

        clock = subprocess.Popen(["xclock", "-display", f":{display}"],
                                 stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)

        # the root is black until the clock content lands, content
        # pixels in the frame prove the full output path
        hits = 0
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            hits = content_pixels((W1, H1))
            if hits >= 3000:
                break
            time.sleep(0.1)
        assert hits >= 3000, \
            f"only {hits} content pixels before the resize, the output " \
            "path is dead"

        # the output grows, the fullscreen root follows the mode one
        # to one: the clock repaints on the new geometry and the frame
        # lands at the new canvas size
        send(f"resize {W2} {H2}")
        wait_for(lambda: xdpyinfo_dims() == f"{W2}x{H2}",
                 f"the root growth to {W2}x{H2}")
        hits = 0
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            hits = content_pixels((W2, H2))
            if hits >= 3000:
                break
            time.sleep(0.1)
        assert hits >= 3000, \
            f"only {hits} content pixels after the resize, the new " \
            "geometry never got painted"

        assert clock.poll() is None, "xclock died across the resize"
        assert bridge.poll() is None, "bridge died across the resize"
        assert proc.poll() is None, "x12 server died across the resize"
        print(f"e2e bridge resize: PASS, fullscreen root {W1}x{H1} -> "
              f"{W2}x{H2}, clock alive, frame follows")
    finally:
        if clock:
            clock.terminate()
            try:
                clock.wait(timeout=3)
            except subprocess.TimeoutExpired:
                clock.kill()
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
