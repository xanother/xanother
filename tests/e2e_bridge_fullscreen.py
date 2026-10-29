# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# bridge fullscreen e2e: the rootful session covers the whole output.
# the fullscreen offer moves the dix root from the cascade sized start
# to the exact mode and the server pins the view to the output origin,
# so a red root background paints every corner of the server frame.
# with the wish disabled the placement policy frame shows up as a
# black strip on the left and top edges - exactly the artifact this
# wave removes

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tests import core_client as cc  # noqa: E402

SOCK = ROOT / "build" / "x12-bridge-fs.sock"
PPM = ROOT / "build" / "framebuffer-bridge-fs.ppm"
FIFO = ROOT / "build" / "input-bridge-fs.fifo"
OUT = ROOT / "build" / "bridge-fs.stdout"
BRIDGE = ROOT / "build" / "vendor" / "xserver" / "build" / "hw" / "x12" / "Xx12"

# the output size the server runs at, and the cascade placed root the
# bridge starts with when no fullscreen wish is sent: the server
# placement origin is 32, the bridge fills the output minus that frame
W, H = 800, 480
START_W, START_H = 768, 448


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
    # the ppm is rewritten live, a torn read just means one more beat
    try:
        raw = PPM.read_bytes()
        head, _, rest = raw.partition(b"255\n")
        dims = head.split(b"\n")[1].split()
        return (int(dims[0]), int(dims[1])), rest
    except (IndexError, ValueError):
        return None, None


def red_stats(size):
    d, pix = ppm_pixels()
    if d != size or pix is None or len(pix) != size[0] * size[1] * 3:
        return 0.0, 0, 0
    red = 0
    for i in range(0, len(pix), 3):
        if pix[i] > 200 and pix[i + 1] < 60 and pix[i + 2] < 60:
            red += 1
    w, h = size

    def block(x0, y0):
        n = 0
        for y in range(y0, y0 + 8):
            for x in range(x0, x0 + 8):
                i = (y * w + x) * 3
                if pix[i] > 200 and pix[i + 1] < 60 and pix[i + 2] < 60:
                    n += 1
        return n
    return red / (w * h), block(0, 0), block(w - 8, h - 8)


def wait_for(fn, what, deadline_s=8):
    end = time.monotonic() + deadline_s
    while time.monotonic() < end:
        if fn():
            return True
        time.sleep(0.1)
    raise AssertionError(f"timeout waiting for {what}")


def run_phase(fs_env, ready_dims):
    # one server and one bridge session on a fresh display, ready_dims
    # is the root size that proves the fullscreen state of the phase
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
    display = free_display()
    xsock = Path(f"/tmp/.X11-unix/X{display}")
    env = {"PATH": "/usr/bin:/bin", "X12_BRIDGE_SOCKET": str(SOCK)}
    if fs_env is not None:
        env["X12_BRIDGE_FULLSCREEN"] = fs_env
    f = open(OUT, "wb")
    bridge = None
    try:
        bridge = subprocess.Popen(
            [str(BRIDGE), f":{display}", "-ac", "-noreset"],
            stdout=f, stderr=f, env=env)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not xsock.exists():
            time.sleep(0.05)
        assert xsock.exists(), "x socket never appeared"

        def dims():
            info = subprocess.run(["xdpyinfo", "-display", f":{display}"],
                                  capture_output=True, timeout=10)
            for line in info.stdout.decode(errors="replace").split("\n"):
                if "dimensions" in line:
                    return line.split()[1]
            return "<none>"
        wait_for(lambda: dims() == f"{ready_dims[0]}x{ready_dims[1]}",
                 f"the {ready_dims[0]}x{ready_dims[1]} root")

        # a red root background is the tracer: fullscreen shows it in
        # every corner, the placement frame keeps the top left black
        subprocess.run(["xsetroot", "-display", f":{display}",
                        "-solid", "red"],
                       check=True, capture_output=True, timeout=10)
        wait_for(lambda: red_stats((W, H))[0] >= 0.5, "the red repaint")
        time.sleep(0.5)
        frac, tl, br = red_stats((W, H))
        return proc, bridge, f, wfd, display, xsock, frac, tl, br
    except BaseException:
        for obj in (bridge, proc):
            if obj:
                obj.terminate()
        f.close()
        os.close(wfd)
        for leftover in (Path(f"/tmp/.X{display}-lock"), xsock, FIFO):
            leftover.unlink(missing_ok=True)
        raise


def cleanup(proc, bridge, f, wfd, display, xsock):
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


def main():
    # phase one: the wish rides after the map, the root grows from the
    # cascade start to the whole output and paints the corners
    (proc, bridge, f, wfd, display, xsock,
     frac, tl, br) = run_phase(None, (W, H))
    cleanup(proc, bridge, f, wfd, display, xsock)
    assert frac >= 0.97, \
        f"fullscreen frame only {frac:.3f} red, the surface leaks"
    assert tl >= 64, f"top left corner has {tl} red pixels, the view is not pinned"
    assert br >= 64, f"bottom right corner has {br} red pixels"
    print("e2e bridge fullscreen: PASS, root covers the whole output, "
          f"{frac:.3f} of the frame is surface")

    # phase two: the wish off restores the old geometry, the placement
    # frame is visible again as a black strip - the documented opt out
    (proc, bridge, f, wfd, display, xsock,
     frac, tl, br) = run_phase("0", (START_W, START_H))
    cleanup(proc, bridge, f, wfd, display, xsock)
    assert frac >= 0.5, f"opted out frame only {frac:.3f} red"
    assert tl < 64, \
        f"opted out top left has {tl} red pixels, the frame is gone"
    assert br >= 64, "opted out bottom right never got painted"
    print("e2e bridge fullscreen: PASS, opt out keeps the placement frame")


if __name__ == "__main__":
    main()
