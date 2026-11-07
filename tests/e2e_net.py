# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# network transport e2e: the one wire runs over tcp and tls like it
# does over unix sockets. the handshake, the capability gate and the
# object dance look exactly the same on all three transports, and the
# fd carrying requests refuse cleanly on the ones without scm rights

import os
import socket
import ssl
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402

BUILD = ROOT / "build"

CAP_SEAT, CAP_OUTPUT_INFO, CAP_SCREENCOPY = 1, 2, 3


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def spawn(addr, extra_env=None):
    err = open(BUILD / "net-server.stderr", "wb")
    env = dict(os.environ)
    if extra_env:
        env.update(extra_env)
    proc = subprocess.Popen(
        [str(BUILD / "x12-server"), addr, "--width", "640", "--height", "400"],
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
        s.close()
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

    print("e2e net: PASS, tcp and tls carry the same session, "
          "fd passing refuses cleanly on the network transports")


if __name__ == "__main__":
    main()
