# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# bridge output e2e: a real x client draws into the rootful bridge,
# dix renders into the shadow framebuffer and the dirty strips ride
# out as x12 buffer commits. the proof is the x12 server frame: the
# bridge surface starts black and the clock face shows up in the
# server framebuffer once the window maps

import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tests import core_client as cc  # noqa: E402

SOCK = ROOT / "build" / "x12-bridge-out.sock"
PPM = ROOT / "build" / "framebuffer-bridge-out.ppm"
OUT = ROOT / "build" / "bridge-out.stdout"
BRIDGE = ROOT / "build" / "vendor" / "xserver" / "build" / "hw" / "x12" / "Xx12"

# the fullscreen offer rides the map, the root follows the output
# mode the server announces
W, H = 800, 480


def free_display():
    # leftover x servers from crashed runs poison a fixed display
    # number, the test picks the first unused one instead
    for n in range(90, 140):
        if not Path(f"/tmp/.X11-unix/X{n}").exists():
            return n
    raise AssertionError("no free x display in 90..139")


def ppm_pixels(path):
    raw = path.read_bytes()
    head, _, rest = raw.partition(b"255\n")
    dims = head.split(b"\n")[1].split()
    w, h = int(dims[0]), int(dims[1])
    return (w, h), rest


def main():
    PPM.unlink(missing_ok=True)
    OUT.unlink(missing_ok=True)
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           extra_args=("--ppm", str(PPM),
                                       "--width", str(W), "--height", str(H)))
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
            env={"PATH": "/usr/bin:/bin",
                 "X12_BRIDGE_SOCKET": str(SOCK),
                 "X12_BRIDGE_WIDTH": str(W),
                 "X12_BRIDGE_HEIGHT": str(H)})
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not xsock.exists():
            time.sleep(0.05)
        assert xsock.exists(), "x socket never appeared"
        assert bridge.poll() is None, "bridge died before the x socket"

        info = subprocess.run(["xdpyinfo", "-display", f":{display}"],
                              capture_output=True, timeout=10)
        assert info.returncode == 0, \
            f"xdpyinfo failed: {info.stderr.decode(errors='replace')[-400:]}"
        out = info.stdout.decode(errors="replace")
        assert f"{W}x{H} pixels" in out, \
            f"root size mismatch: {[l for l in out.split(chr(10)) if 'dimensions' in l]}"

        clock = subprocess.Popen(["xclock", "-display", f":{display}"],
                                 stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL)

        # the empty root is pure black, so non black pixels in the
        # server frame can only be dix rendered clock content that rode
        # the shadow damage all the way into an x12 commit; a diff of
        # two snapshots would anchor its base in the middle of the
        # startup race and flake
        deadline = time.monotonic() + 10

        def content_pixels():
            d, pix = ppm_pixels(PPM)
            if d != (W, H) or pix is None or len(pix) != W * H * 3:
                # a torn read mid rewrite carries a mismatched body
                return 0
            return sum(1 for i in range(0, len(pix), 3)
                       if pix[i] or pix[i + 1] or pix[i + 2])

        hits = 0
        while time.monotonic() < deadline:
            hits = content_pixels()
            if hits >= 3000:
                break
            time.sleep(0.1)
        assert hits >= 3000, \
            f"only {hits} content pixels reached the server frame " \
            "within 10s, the output path is dead"

        assert bridge.poll() is None, "bridge died during the output e2e"
        assert clock.poll() is None, "xclock died during the output e2e"
        assert proc.poll() is None, "x12 server died during the output e2e"
        print("e2e bridge output: PASS, xclock pixels through dix shadow "
              "damage to x12 commits")
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
