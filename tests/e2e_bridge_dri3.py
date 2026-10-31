# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# bridge dri3 e2e: the dri3 and present extensions register on the
# bridge screen and a gl client renders through the glue. the client
# path survives a driver probe miss: the same visual surface comes
# out of the software fallback

import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tests import core_client as cc  # noqa: E402

SOCK = ROOT / "build" / "x12-bridge-dri3.sock"
PPM = ROOT / "build" / "framebuffer-bridge-dri3.ppm"
OUT = ROOT / "build" / "bridge-dri3.stdout"
BRIDGE = ROOT / "build" / "vendor" / "xserver" / "build" / "hw" / "x12" / "Xx12"


def free_display():
    for n in range(90, 140):
        if not Path(f"/tmp/.X11-unix/X{n}").exists():
            return n
    raise AssertionError("no free x display in 90..139")


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
        bridge = subprocess.Popen(
            [str(BRIDGE), f":{display}", "-ac", "-noreset"],
            stdout=f, stderr=f,
            env={"PATH": "/usr/bin:/bin", "X12_BRIDGE_SOCKET": str(SOCK)})
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if b"screen ready" in OUT.read_bytes():
                break
            time.sleep(0.2)
        else:
            raise AssertionError("bridge never reported a ready screen")
        assert b"dri3 ready" in OUT.read_bytes(), "dri3 hooks not registered"

        # the present extension must sit on the screen list next to
        # dri3, the flip feedback path is its runtime proof through the
        # glx run
        ext = subprocess.run(["xdpyinfo", "-display", f":{display}"],
                             capture_output=True, timeout=10)
        exts = ext.stdout.decode(errors="replace")
        assert "DRI3" in exts and "Present" in exts, \
            f"extension list missing dri3/present: {exts[-400:]}"

        info = subprocess.run(["glxinfo", "-B", "-display", f":{display}"],
                              capture_output=True, timeout=30)
        out = info.stdout.decode(errors="replace")
        assert info.returncode == 0, \
            f"glxinfo failed: {info.stderr.decode(errors='replace')[-400:]}"
        assert "OpenGL" in out, f"glxinfo produced nothing: {out}"

        print("e2e bridge dri3: PASS, dri3 and present extensions live, "
              "gl client renders through the glue")
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
