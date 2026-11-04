# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# bridge input e2e: the x12 seat events of the session land in dix
# through the bridge input glue. the host events ride the input fifo
# of the server, xterm is the consumer that proves the whole chain

import os
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tests import core_client as cc  # noqa: E402

SOCK = ROOT / "build" / "x12-bridge-input.sock"
PPM = ROOT / "build" / "framebuffer-bridge-input.ppm"
FIFO = ROOT / "build" / "input-bridge-input.fifo"
OUT = ROOT / "build" / "bridge-input.stdout"
XTYPE_OUT = ROOT / "build" / "bridge-xterm.txt"
BRIDGE = ROOT / "build" / "vendor" / "xserver" / "build" / "hw" / "x12" / "Xx12"

# evdev codes, the storm table subset
KEY = {"h": 35, "e": 18, "l": 38, "o": 24, "\n": 28}


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
    XTYPE_OUT.unlink(missing_ok=True)
    FIFO.unlink(missing_ok=True)
    os.mkfifo(FIFO)
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK,
                           extra_args=("--input", str(FIFO),
                                       "--ppm", str(PPM),
                                       "--width", "800", "--height", "480"))
    wfd = open_fifo()

    def send(line):
        os.write(wfd, (line + "\n").encode())

    display = free_display()
    xsock = Path(f"/tmp/.X11-unix/X{display}")
    f = open(OUT, "wb")
    bridge = None
    xterm = None
    try:
        bridge = subprocess.Popen(
            [str(BRIDGE), f":{display}", "-ac", "-noreset"],
            stdout=f, stderr=f,
            env={"PATH": "/usr/bin:/bin",
                 "X12_BRIDGE_SOCKET": str(SOCK),
                 "X12_BRIDGE_WIDTH": "768",
                 "X12_BRIDGE_HEIGHT": "448",
                 # the test polls the window tree with xwininfo to know
                 # when xterm mapped - under the isolation cloak that
                 # poll needs the tool grant
                 "X12_BRIDGE_GRANTS": "xwininfo"})
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not xsock.exists():
            time.sleep(0.05)
        assert xsock.exists(), "x socket never appeared"

        xterm = subprocess.Popen(
            ["xterm", "-display", f":{display}", "-e", "sh", "-c",
             "read line; echo got:$line > " + str(XTYPE_OUT)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        # the window must be mapped before anything is typed, keys
        # aimed at a not yet mapped window die at the root; xdotool is
        # blind here, the window tree query is the honest poll
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            tree = subprocess.run(["xwininfo", "-display", f":{display}",
                                   "-root", "-tree"], capture_output=True)
            if tree.returncode == 0 and b'"xterm"' in tree.stdout:
                break
            time.sleep(0.2)
        else:
            raise AssertionError("xterm window never appeared")
        time.sleep(0.8)

        # pointer into the window, the click focuses the bridge surface
        # on the server side and aims dix pointer focus at xterm
        send("motion 120 120")
        time.sleep(0.25)
        send("button 1 press\nbutton 1 release")
        time.sleep(0.25)
        # hello plus enter as one batch, the pump reads the file whole
        lines = []
        for ch in "hello":
            lines.append(f"key {KEY[ch]} press")
            lines.append(f"key {KEY[ch]} release")
        lines.append(f"key {KEY[chr(10)]} press")
        lines.append(f"key {KEY[chr(10)]} release")
        send("\n".join(lines))

        deadline = time.monotonic() + 15
        body = ""
        while time.monotonic() < deadline:
            if XTYPE_OUT.exists():
                body = XTYPE_OUT.read_text(errors="replace").strip()
                if "got:hello" in body:
                    break
            time.sleep(0.2)
        assert "got:hello" in body, \
            f"xterm never printed the typed line, got: {body!r}"

        # the typed line returned from read, the shell finished and
        # took xterm with it: a clean exit is the success marker
        rc = xterm.wait(timeout=5)
        assert rc == 0, f"xterm exited with {rc}"
        assert bridge.poll() is None, "bridge died during the input e2e"
        assert proc.poll() is None, "x12 server died during the input e2e"
        print("e2e bridge input: PASS, x12 input fifo to dix to xterm, "
              "typed line verified")
    finally:
        if xterm:
            xterm.terminate()
            try:
                xterm.wait(timeout=3)
            except subprocess.TimeoutExpired:
                xterm.kill()
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
