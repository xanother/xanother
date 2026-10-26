# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# bridge smoke e2e: the vendored xserver boots as the x12 ddx,
# completes the hello/welcome handshake against our x12 server, serves
# its x socket and a real x client talks to it end to end

import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tests import core_client as cc  # noqa: E402

SOCK = ROOT / "build" / "x12-bridge.sock"
PPM = ROOT / "build" / "framebuffer-bridge.ppm"
OUT = ROOT / "build" / "bridge.stdout"
BRIDGE = ROOT / "build" / "vendor" / "xserver" / "build" / "hw" / "x12" / "Xx12"


def free_display():
    # leftover x servers from crashed runs poison a fixed display
    # number, the test picks the first unused one instead
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


def main():
    PPM.unlink(missing_ok=True)
    OUT.unlink(missing_ok=True)
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           extra_args=("--ppm", str(PPM),
                                       "--width", "800", "--height", "480"))
    time.sleep(0.3)
    display = free_display()
    xsock = Path(f"/tmp/.X11-unix/X{display}")
    f = open(OUT, "wb")
    bridge = None
    try:
        # noreset: dix would re-run initoutput after the last client
        # leaves, the bridge keeps one session per process lifetime
        bridge = subprocess.Popen(
            [str(BRIDGE), f":{display}", "-ac", "-noreset"],
            stdout=f, stderr=f,
            env={"PATH": "/usr/bin:/bin",
                 "X12_BRIDGE_SOCKET": str(SOCK),
                 "X12_BRIDGE_WIDTH": "800",
                 "X12_BRIDGE_HEIGHT": "480"})
        wait_file(OUT, "X12-BRIDGE: screen ready", 15)

        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not xsock.exists():
            time.sleep(0.05)
        assert xsock.exists(), "x socket never appeared"

        # a real x client completes a full protocol round trip through
        # dix, over the ddx glue and out to the x12 session
        info = subprocess.run(["xdpyinfo", "-display", f":{display}"],
                              capture_output=True, timeout=10)
        out = info.stdout.decode(errors="replace")
        assert info.returncode == 0, \
            f"xdpyinfo failed: {info.stderr.decode(errors='replace')[-400:]}"
        assert "800x480 pixels" in out, \
            f"root size mismatch: {[l for l in out.split(chr(10)) if 'dimensions' in l]}"

        assert bridge.poll() is None, "bridge exited during the smoke"
        assert proc.poll() is None, "x12 server died during the smoke"
        print("e2e bridge smoke: PASS, ddx handshake, x socket, "
              "xdpyinfo round trip")
    finally:
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
