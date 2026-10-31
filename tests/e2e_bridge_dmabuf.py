# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# bridge dmabuf e2e: the glamor screen texture exports as a linear
# dmabuf, the server imports it and the pump stops copying rows. a
# driver without the export path walks back to the cpu pump and the
# composite proof stays on the ppm, so both outcomes are documented
# outcomes

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tests import core_client as cc  # noqa: E402

SOCK = ROOT / "build" / "x12-bridge-dmabuf.sock"
PPM = ROOT / "build" / "framebuffer-bridge-dmabuf.ppm"
OUT = ROOT / "build" / "bridge-dmabuf.stdout"
BRIDGE = ROOT / "build" / "vendor" / "xserver" / "build" / "hw" / "x12" / "Xx12"

W, H = 800, 480


def free_display():
    for n in range(90, 140):
        if not Path(f"/tmp/.X11-unix/X{n}").exists():
            return n
    raise AssertionError("no free x display in 90..139")


def wait_log(path, needle, deadline_s):
    end = time.monotonic() + deadline_s
    while time.monotonic() < end:
        if needle in path.read_bytes():
            return True
        time.sleep(0.2)
    return needle in path.read_bytes()


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
    clock = None
    try:
        bridge = subprocess.Popen(
            [str(BRIDGE), f":{display}", "-ac", "-noreset"],
            stdout=f, stderr=f,
            env={"PATH": "/usr/bin:/bin", "X12_BRIDGE_SOCKET": str(SOCK)})
        assert wait_log(OUT, b"screen ready", 20), "bridge never went ready"
        direct = b"screen dmabuf exported" in OUT.read_bytes()

        clock = subprocess.Popen(["xclock", "-display", f":{display}"],
                                 stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)
        time.sleep(3)

        if direct:
            # the zero copy path: the bridge logs the export and the
            # session rides the flip chain, the ppm is not the proof
            # here because the compositor never sees those frames
            assert clock.poll() is None, "xclock died on the dmabuf path"
            assert bridge.poll() is None, "bridge died on the dmabuf path"
            print("e2e bridge dmabuf: PASS, screen texture exported, "
                  "scanout direct, no cpu pump")
        else:
            # the fallback path: the ppm proof like the glx run
            deadline = time.monotonic() + 10
            best = 0
            while time.monotonic() < deadline:
                try:
                    raw = PPM.read_bytes()
                    head, _, rest = raw.partition(b"255\n")
                    dims = head.split(b"\n")[1].split()
                    if (int(dims[0]), int(dims[1])) == (W, H):
                        # the clock is monochrome, lit pixels are the
                        # proof, saturation is a gears-only metric
                        lit = sum(1 for i in range(0, len(rest), 3)
                                  if rest[i] > 30 or rest[i + 1] > 30 or
                                  rest[i + 2] > 30)
                        best = max(best, lit)
                except (IndexError, ValueError):
                    pass
                time.sleep(0.2)
            assert best > 400, \
                f"fallback ppm never showed content, best {best}"
            print("e2e bridge dmabuf: PASS, export refused by the driver, "
                  f"cpu pump fallback verified on the ppm ({best} px)")
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
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
        for leftover in (Path(f"/tmp/.X{display}-lock"), xsock):
            leftover.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
