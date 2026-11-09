# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# network transport e2e: the one wire runs over tcp and tls like it
# does over unix sockets. the handshake, the capability gate and the
# object dance look exactly the same on all three transports, and the
# fd carrying requests refuse cleanly on the ones without scm rights

import os
import socket
import struct
import ssl
import time
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.x12gen import load_idl  # noqa: E402
from tests import wire_model as wm  # noqa: E402
from tests import core_client as cc  # noqa: E402

BUILD = ROOT / "build"

CAP_SEAT, CAP_OUTPUT_INFO, CAP_SCREENCOPY = 1, 2, 3
XR24 = 0x34325258


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def spawn(addr, extra_env=None, extra_args=(), size=(640, 400)):
    err = open(BUILD / "net-server.stderr", "wb")
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    proc = subprocess.Popen(
        [str(BUILD / "x12-server"), addr,
         "--width", str(size[0]), "--height", str(size[1])]
        + [str(a) for a in extra_args],
        stdout=subprocess.DEVNULL, stderr=err, env=env)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if "listening on" in (BUILD / "net-server.stderr").read_text(
                errors="replace"):
            return proc, err
        if proc.poll() is not None:
            break
        time.sleep(0.05)
    body = (BUILD / "net-server.stderr").read_text(errors="replace")
    raise AssertionError(f"server did not listen on {addr}:\n{body[-800:]}")


def dial(addr, idl, tls=False, insecure=False, ca=None):
    port = int(addr.rsplit(":", 1)[1])
    raw = socket.create_connection(("127.0.0.1", port), timeout=5)
    if tls:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        if insecure:
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
        else:
            ctx.load_verify_locations(ca)
        raw = ctx.wrap_socket(raw, server_hostname="127.0.0.1")
    return cc.Session(addr, idl, sock=raw)


def handshake(s):
    s.request("x12_connection", "hello", 1, idl_major=1, idl_minor=0)
    welcome = s.wait_for("welcome")
    assert set(welcome["args"]["caps"]) == {CAP_SEAT, CAP_OUTPUT_INFO}, welcome
    globals_seen = {}
    for _ in range(5):
        g = s.wait_for("global")
        globals_seen[g["args"]["name"]] = g["args"]["version"]
    assert "x12_compositor" in globals_seen, globals_seen
    return welcome


def session_dance(s):
    # the same protocol dance the unix tests run: sync round trips and
    # the capability gate answer the same over every transport
    s.request("x12_connection", "sync", 1, callback=10)
    s.register("x12_callback", 10)
    done = s.wait_for("done")
    assert done["args"]["serial"] == 1, done

    s.request("x12_registry", "bind", 2,
              name="x12_screencopy", version=1, id=20, instance=0)
    err = s.wait_for("error")
    assert err["args"]["code"] == "no-capability", err


def big_frame_roundtrip(s):
    # a payload past the compression threshold rides the zlib path and
    # still lands as one request, the answer comes back the same way
    s.request("x12_registry", "bind", 2,
              name="no-such-global-" + "x" * 60000,
              version=1, id=21, instance=0)
    err = s.wait_for("error")
    assert err["args"]["code"] == "bad-value", err


def slow_client_kill(idl, addr):
    # the backpressure policy from the old server: a client that stops
    # reading its events is disconnected loudly, events are never
    # dropped to fake progress. the flood fills the kernel buffer and
    # then the 64k out buffer, the reap follows
    port = int(addr.rsplit(":", 1)[1])
    raw = socket.create_connection(("127.0.0.1", port), timeout=5)
    s = cc.Session(addr, idl, sock=raw)
    handshake(s)
    batch = b""
    frames = 0
    killed = False
    try:
        for i in range(20000):
            batch += wm.encode_request(idl, "x12_registry", "bind", 2,
                                       name="nope", version=1,
                                       id=25 + (i % 3), instance=0)
            frames += 1
            if len(batch) >= 65536:
                s.sock.sendall(batch)
                batch = b""
        if batch:
            s.sock.sendall(batch)
    except (BrokenPipeError, ConnectionResetError):
        # the loud disconnect reached us mid flood
        killed = True
    # no reads at all above: the server eventually gives up on us
    if not killed:
        deadline = time.monotonic() + 10
        s.sock.settimeout(1)
        while time.monotonic() < deadline:
            try:
                if not s.sock.recv(4096):
                    killed = True
                    break
            except (socket.timeout, ConnectionResetError):
                continue
            except OSError:
                killed = True
                break
    assert killed, "slow client was never disconnected"
    s.sock.close()
    return frames


def fd_refusal(s):
    # a shm pool needs a passed descriptor, the network transports have
    # none and the refusal is a clean bad-value, not a hang
    s.request("x12_registry", "bind", 2,
              name="x12_buffer_manager", version=1, id=30, instance=0)
    s.register("x12_buffer_manager", 30)
    s.request("x12_buffer_manager", "create_shm_pool", 30,
              fd=0, size=4096, id=31)
    err = s.wait_for("error")
    assert err["args"]["code"] == "bad-value", err


def remote_buffer_render(idl, addr):
    # the full picture over the network: pixels uploaded into a net pool
    # with pool writes, composited and dumped. this is the path a remote
    # client draws through, no file descriptor ever changes hands
    w = h = 16
    ppm = BUILD / "net-render.ppm"
    ppm.unlink(missing_ok=True)
    proc, err = spawn(addr, extra_args=["--ppm", str(ppm)], size=(w, h))
    try:
        s = dial(addr, idl)
        handshake(s)
        s.request("x12_registry", "bind", 2,
                  name="x12_compositor", version=2, id=10, instance=0)
        s.register("x12_compositor", 10)
        s.request("x12_registry", "bind", 2,
                  name="x12_buffer_manager", version=1, id=11, instance=0)
        s.register("x12_buffer_manager", 11)
        for _ in range(2):
            s.wait_for("format")

        slot = w * h * 4
        s.request("x12_buffer_manager", "create_net_pool", 11,
                  size=slot, id=20)
        s.register("x12_shm_pool", 20)

        # one solid color chunked across many writes, the chunking
        # exercises offset bookkeeping in the upload path
        px = struct.pack("<I", 0xFFCC4422)
        chunk = px * 64
        for off in range(0, slot, len(chunk)):
            s.request("x12_shm_pool", "write", 20, offset=off,
                      data=chunk[:slot - off])

        s.request("x12_shm_pool", "create_buffer", 20, id=21, offset=0,
                  width=w, height=h, stride=w * 4, format=XR24)
        s.register("x12_buffer", 21)
        s.request("x12_compositor", "create_surface", 10, id=30)
        s.register("x12_surface", 30)
        s.request("x12_compositor", "create_toplevel", 10, id=31,
                  surface=30)
        s.register("x12_toplevel", 31)
        while True:
            ev = s.next_event()
            if ev["name"] == "configure" and ev["object"] == 31:
                break
        s.request("x12_toplevel", "ack_configure", 31,
                  serial=ev["args"]["serial"])
        s.request("x12_toplevel", "set_fullscreen", 31, state=1)
        while True:
            ev = s.next_event()
            if ev["name"] == "configure" and ev["object"] == 31:
                break
        s.request("x12_toplevel", "ack_configure", 31,
                  serial=ev["args"]["serial"])
        s.request("x12_surface", "set_buffer", 30, buffer=21)
        s.request("x12_surface", "damage", 30, x=0, y=0, width=w, height=h)
        s.request("x12_surface", "commit", 30)

        deadline = time.monotonic() + 10
        # ppm is rgb, the xrgb byte order is b,g,r,x
        want = (0xCC, 0x44, 0x22)
        while time.monotonic() < deadline:
            try:
                data = ppm.read_bytes()
            except FileNotFoundError:
                time.sleep(0.02)
                continue
            parts = data.split(b"255\n", 1)
            if len(parts) != 2 or len(parts[1]) != w * h * 3:
                time.sleep(0.02)
                continue
            body = parts[1]
            o = 3 * (8 * w + 8)
            if (body[o], body[o + 1], body[o + 2]) == want:
                s.close()
                return
            time.sleep(0.02)
        raise AssertionError("uploaded pixels never reached the framebuffer")
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        err.close()


def main():
    idl = load_idl(ROOT / "spec" / "x12-core.yaml")

    # plain tcp: the full dance
    port = free_port()
    proc, err = spawn(f"tcp:127.0.0.1:{port}")
    try:
        s = dial(f"tcp:127.0.0.1:{port}", idl)
        handshake(s)
        session_dance(s)
        fd_refusal(s)
        big_frame_roundtrip(s)
        s.close()
        slow_client_kill(idl, f"tcp:127.0.0.1:{port}")
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        err.close()

    # tls with a self signed certificate: verified and insecure both work
    cert = BUILD / "net-cert.pem"
    key = BUILD / "net-key.pem"
    subprocess.run(
        ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
         "-keyout", str(key), "-out", str(cert), "-days", "2",
         "-subj", "/CN=127.0.0.1",
         "-addext", "subjectAltName = IP:127.0.0.1"],
        check=True, capture_output=True)

    port = free_port()
    proc, err = spawn(f"tls:127.0.0.1:{port}",
                      {"X12_TLS_CERT": str(cert), "X12_TLS_KEY": str(key)})
    try:
        s = dial(f"tls:127.0.0.1:{port}", idl, tls=True, ca=str(cert))
        handshake(s)
        session_dance(s)
        fd_refusal(s)
        s.close()

        s = dial(f"tls:127.0.0.1:{port}", idl, tls=True, insecure=True)
        handshake(s)
        s.close()
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        err.close()

    # remote pixels over the network: the upload path through net pools
    remote_buffer_render(idl, f"tcp:127.0.0.1:{free_port()}")

    print("e2e net: PASS, tcp and tls carry the same session, "
          "fd passing refuses cleanly on the network transports, "
          "uploaded pixels reach the framebuffer")


if __name__ == "__main__":
    main()
