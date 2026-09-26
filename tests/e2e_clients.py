# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# end to end for the shipped clients: welcome paints its gradient and
# the terminal runs a scripted pty session asserted through the sigusr1
# grid dumps and the ppm pixels - typing through the xkb keymap, drag
# selection with the middle click paste round trip, wheel scrollback

import os
import select
import signal
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"

# window placement: the first toplevel cascades to 32,32
WX, WY = 32, 32
FONT_W, FONT_H = 8, 16


def read_ppm(path):
    try:
        data = path.read_bytes()
    except FileNotFoundError:
        return None
    parts = data.split(b"255\n", 1)
    if len(parts) != 2:
        return None
    head, body = parts
    wh = head.split()
    w, h = int(wh[1]), int(wh[2])
    if len(body) != w * h * 3:
        return None
    return w, h, body


def px(body, w, x, y):
    o = 3 * (y * w + x)
    return (body[o], body[o + 1], body[o + 2])


def wait_pixels(path, check, what):
    deadline = time.time() + 10
    while time.time() < deadline:
        frame = read_ppm(path)
        if frame and check(frame):
            return
        time.sleep(0.02)
    raise AssertionError(f"pixels never matched: {what}")


class Term:
    # terminal subprocess with a buffered stdout reader

    def __init__(self, sock, cmd):
        self.proc = subprocess.Popen(
            [str(BUILD / "x12-term"), str(sock), "--exec", cmd],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.buf = b""

    def read_until(self, marker, timeout=10):
        deadline = time.time() + timeout
        while marker not in self.buf:
            remain = deadline - time.time()
            if remain <= 0:
                raise AssertionError(
                    f"marker {marker!r} never arrived, got {self.buf[-200:]!r}")
            ready, _, _ = select.select([self.proc.stdout], [], [], remain)
            if not ready:
                continue
            chunk = os.read(self.proc.stdout.fileno(), 4096)
            if not chunk:
                raise AssertionError("terminal closed its stdout")
            self.buf += chunk
        idx = self.buf.index(marker) + len(marker)
        out, self.buf = self.buf[:idx], self.buf[idx:]
        return out

    def dump(self):
        # the terminal answers sigusr1 with the viewport text
        self.proc.send_signal(signal.SIGUSR1)
        block = self.read_until(b"TERM-DUMP-END\n")
        text = block.split(b"TERM-DUMP-BEGIN\n", 1)[1]
        text = text[: text.rindex(b"\nTERM-DUMP-END")]
        return text.decode("utf-8", "replace").split("\n")

    def kill(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def open_fifo(path):
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            return os.open(str(path), os.O_WRONLY | os.O_NONBLOCK)
        except OSError:
            time.sleep(0.02)
    raise AssertionError("input fifo never opened")


def welcome_check(w, h):
    # the welcome gradient formula mirrored for the assert, the two are
    # one contract exactly like the blend mirror in the render suite
    def ring(x, y):
        return min(x, y, w - 1 - x, h - 1 - y)

    def expected(x, y):
        if ring(x, y) < 3:
            return (230, 230, 235)
        return (40 + (x - 3) * 180 // (w - 7),
                40 + (y - 3) * 180 // (h - 7), 200)

    def check(frame):
        fw, fh, body = frame
        # the window sits at the cascade origin, the rest is background
        for y in range(h):
            for x in range(w):
                if px(body, fw, WX + x, WY + y) != expected(x, y):
                    return False
        return px(body, fw, 0, 0) == (0, 0, 0)
    return check


def main():
    idl = load_idl(ROOT / "spec" / "x12-core.yaml")

    # phase 1: welcome maps a gradient window and exits on --once, the
    # ppm holds the painted frame afterwards
    ppm = BUILD / "x12-welcome.ppm"
    ppm.unlink(missing_ok=True)
    proc = cc.spawn_server(BUILD / "x12-server", BUILD / "x12-welcome.sock",
                           ["--width", "256", "--height", "192",
                            "--ppm", str(ppm)])
    welcome = subprocess.Popen(
        [str(BUILD / "welcome"), str(BUILD / "x12-welcome.sock"),
         "--size", "192x128"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        # the window lives as long as the client does, the ppm assert
        # must run before the disconnect takes it off the canvas
        w = Term.__new__(Term)
        w.proc, w.buf = welcome, b""
        w.read_until(b"WELCOME: mapped", timeout=15)
        wait_pixels(ppm, welcome_check(192, 128), "welcome gradient")
    finally:
        welcome.terminate()
        try:
            welcome.wait(timeout=5)
        except subprocess.TimeoutExpired:
            welcome.kill()
        proc.terminate()
        proc.wait(timeout=5)

    # phase 2: the terminal drives a scripted session. seq prints more
    # lines than the grid holds, cat stays for the interactive part
    ppm = BUILD / "x12-term.ppm"
    ppm.unlink(missing_ok=True)
    fifo = BUILD / "x12-term-input.fifo"
    fifo.unlink(missing_ok=True)
    os.mkfifo(fifo)
    proc = cc.spawn_server(BUILD / "x12-server", BUILD / "x12-term.sock",
                           ["--width", "704", "--height", "448",
                            "--ppm", str(ppm), "--input", str(fifo)])
    wfd = open_fifo(fifo)
    term = Term(BUILD / "x12-term.sock",
                "/bin/sh -c 'seq 1 30; exec /bin/cat'")
    # the shell prompt row: the seq output filled the grid and the
    # interactive session continues on the bottom row
    ROW = 23

    def send(line):
        os.write(wfd, (line + "\n").encode())

    def cell(x, y):
        # cell coordinates to a global pixel inside the cell
        return (WX + x * FONT_W + 2, WY + y * FONT_H + 2)

    try:
        term.read_until(b"TERM: mapped cols=80 rows=24\n")

        # click to focus on the window before any key can reach it
        cx, cy = cell(0, 0)
        send(f"motion {cx} {cy}")
        send("button 1 press")
        send("button 1 release")

        # the seq output scrolled the grid, seven lines are archived
        deadline = time.time() + 10
        lines = []
        while time.time() < deadline:
            lines = [ln.rstrip() for ln in term.dump()]
            if "30" in lines:
                break
            time.sleep(0.1)
        assert "30" in lines, lines
        assert "8" in lines and "7" not in lines and "1" not in lines, lines

        # typing through the xkb keymap lands in cat and echoes back.
        # h and i are evdev codes 35 and 23
        for code in (35, 23):
            send(f"key {code} press")
            send(f"key {code} release")
        deadline = time.time() + 10
        top = ""
        while time.time() < deadline:
            top = term.dump()[ROW].rstrip()
            if top == "hi":
                break
            time.sleep(0.1)
        assert top == "hi", top

        def check_glyphs(frame):
            w, h, body = frame
            # the h and i glyphs light pixels in their cells
            lit = 0
            for gy in range(FONT_H):
                for gx in range(2 * FONT_W):
                    p = px(body, w, WX + gx, WY + ROW * FONT_H + gy)
                    if p != (16, 16, 32):
                        lit += 1
            return lit > 20
        wait_pixels(ppm, check_glyphs, "typed glyphs on screen")

        # drag selection over the two cells: the highlight is the plain
        # color swap, a corner pixel of the first cell flips to the fg
        send(f"motion {cell(0, ROW)[0]} {cell(0, ROW)[1]}")
        send("button 1 press")
        send(f"motion {cell(1, ROW)[0]} {cell(1, ROW)[1]}")
        send("button 1 release")

        def check_sel(frame):
            w, h, body = frame
            return px(body, w, WX + 7,
                      WY + ROW * FONT_H + FONT_H - 1) == (200, 200, 210)
        wait_pixels(ppm, check_sel, "selection highlight")

        # middle click pastes the selection back into cat, the echo
        # continues the line: hi becomes hihi
        send("button 2 press")
        send("button 2 release")
        deadline = time.time() + 10
        top = ""
        while time.time() < deadline:
            top = term.dump()[ROW].rstrip()
            if top == "hihi":
                break
            time.sleep(0.1)
        assert top == "hihi", top

        # three wheel notches up lift the viewport into the scrollback
        # and the archived head line comes back
        send(f"motion {cell(40, 10)[0]} {cell(40, 10)[1]}")
        for _ in range(3):
            send("axis vertical -256")
        deadline = time.time() + 10
        lines = []
        while time.time() < deadline:
            lines = [ln.rstrip() for ln in term.dump()]
            if "1" in lines and "7" in lines:
                break
            time.sleep(0.1)
        assert "1" in lines and "7" in lines, lines
    finally:
        term.kill()
        os.close(wfd)
        proc.terminate()
        proc.wait(timeout=5)
        fifo.unlink(missing_ok=True)

    print("e2e clients: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
