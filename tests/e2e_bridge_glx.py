# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# bridge glx e2e: gl clients render on the bridge glamor context and
# the frames ride the pump into the x12 session. glxinfo must see
# direct rendering through the bridge, glxgears must paint saturated
# color into the server frame: only real opengl content scores

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tests import core_client as cc  # noqa: E402

SOCK = ROOT / "build" / "x12-bridge-glx.sock"
PPM = ROOT / "build" / "framebuffer-bridge-glx.ppm"
OUT = ROOT / "build" / "bridge-glx.stdout"
BRIDGE = ROOT / "build" / "vendor" / "xserver" / "build" / "hw" / "x12" / "Xx12"

W, H = 800, 480


def free_display():
    for n in range(90, 140):
        if not Path(f"/tmp/.X11-unix/X{n}").exists():
            return n
    raise AssertionError("no free x display in 90..139")


def ppm_pixels():
    # the ppm is rewritten live, a torn read just means one more beat
    try:
        raw = PPM.read_bytes()
        head, _, rest = raw.partition(b"255\n")
        dims = head.split(b"\n")[1].split()
        return (int(dims[0]), int(dims[1])), rest
    except (IndexError, ValueError):
        return None, None


def colored_pixels():
    # gears are saturated reds and greens over dark metal, the empty
    # root is neutral black and the window chrome is grayscale, so
    # only real opengl content scores here
    d, pix = ppm_pixels()
    if d != (W, H) or pix is None or len(pix) != W * H * 3:
        return 0
    n = 0
    for i in range(0, len(pix), 3):
        r, g, b = pix[i], pix[i + 1], pix[i + 2]
        if max(r, g, b) - min(r, g, b) > 60:
            n += 1
    return n


def main():
    PPM.unlink(missing_ok=True)
    OUT.unlink(missing_ok=True)
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           extra_args=("--ppm", str(PPM),
                                       "--width", str(W),
                                       "--height", str(H)))
    time.sleep(0.3)
    display = free_display()
    xsock = Path(f"/tmp/.X11-unix/X{display}")
    f = open(OUT, "wb")
    bridge = None
    gears = None
    try:
        bridge = subprocess.Popen(
            [str(BRIDGE), f":{display}", "-ac", "-noreset"],
            stdout=f, stderr=f,
            env={"PATH": "/usr/bin:/bin", "X12_BRIDGE_SOCKET": str(SOCK)})
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not xsock.exists():
            time.sleep(0.05)
        assert xsock.exists(), "x socket never appeared"

        info = subprocess.run(["glxinfo", "-B", "-display", f":{display}"],
                              capture_output=True, timeout=20)
        out = info.stdout.decode(errors="replace")
        assert info.returncode == 0, \
            f"glxinfo failed: {info.stderr.decode(errors='replace')[-400:]}"
        assert "direct rendering: Yes" in out, \
            f"no direct rendering: {out}"

        gears = subprocess.Popen(
            ["glxgears", "-display", f":{display}", "-info"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env={**os.environ, "LIBGL_ALWAYS_SOFTWARE": "1"})
        deadline = time.monotonic() + 20
        hits = 0
        while time.monotonic() < deadline:
            if gears.poll() is not None:
                body = gears.stdout.read().decode(errors="replace")
                raise AssertionError(f"glxgears died early: {body[-400:]}")
            hits = colored_pixels()
            if hits >= 400:
                break
            time.sleep(0.1)
        assert hits >= 400, \
            f"only {hits} colored pixels in the server frame, the glx " \
            "drawables never reached the x12 surface"

        assert bridge.poll() is None, "bridge died during the glx e2e"
        assert proc.poll() is None, "x12 server died during the glx e2e"
        print("e2e bridge glx: PASS, direct rendering through the bridge, "
              f"{hits} gl colored pixels in the server frame")
    finally:
        if gears:
            gears.terminate()
            try:
                gears.wait(timeout=3)
            except subprocess.TimeoutExpired:
                gears.kill()
        if bridge:
            bridge.terminate()
            try:
                bridge.wait(timeout=3)
            except subprocess.TimeoutExpired:
                bridge.kill()
        f.close()
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        for leftover in (Path(f"/tmp/.X{display}-lock"), xsock):
            leftover.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
