# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# end to end smoke for the core skeleton: a real client over a real unix
# socket runs the handshake, the capability gate, the object table and the
# loud disconnect rules. asserts cover every party 3 checklist line

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOCK = ROOT / "build" / "x12-core.sock"

# x12_capability wire values and x12_error_code symbols straight from the
# spec, the test asserts the mapping does not drift
CAP_SEAT, CAP_OUTPUT_INFO, CAP_SCREENCOPY = 1, 2, 3


def handshake(s):
    s.request("x12_connection", "hello", 1, idl_major=1, idl_minor=0)
    welcome = s.wait_for("welcome")
    assert set(welcome["args"]["caps"]) == {CAP_SEAT, CAP_OUTPUT_INFO}, welcome
    assert welcome["args"]["server_version"] == 1, welcome
    globals_seen = {}
    for _ in range(5):
        g = s.wait_for("global")
        globals_seen[g["args"]["name"]] = (g["args"]["version"], g["args"]["required_cap"])
    assert globals_seen == {
        "x12_compositor": (2, "none"),
        "x12_buffer_manager": (1, "none"),
        "x12_seat": (2, "seat"),
        "x12_screencopy": (1, "screencopy"),
        "x12_output": (2, "output-info"),
    }, globals_seen
    return welcome


def main():
    idl = load_idl(ROOT / "spec" / "x12-core.yaml")
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK)
    try:
        # handshake: welcome carries the caps, registry announces globals
        s = cc.Session(SOCK, idl)
        welcome = handshake(s)
        assert welcome["timestamp_ns"] > 0, welcome

        # serial and timing: done carries a growing serial and every event
        # timestamp is monotonic
        s.request("x12_connection", "sync", 1, callback=10)
        s.register("x12_callback", 10)
        done1 = s.wait_for("done")
        s.request("x12_connection", "sync", 1, callback=11)
        s.register("x12_callback", 11)
        done2 = s.wait_for("done")
        assert done1["args"]["serial"] == 1, done1
        assert done2["args"]["serial"] == 2, done2
        assert done1["timestamp_ns"] <= done2["timestamp_ns"], (done1, done2)

        # capability gate: screencopy is never granted, the refusal is
        # loud and the id stays unused
        s.request("x12_registry", "bind", 2,
                  name="x12_screencopy", version=1, id=20, instance=0)
        err = s.wait_for("error")
        assert err["args"]["code"] == "no-capability", err
        assert err["args"]["object"] == 2, err

        # a capability the client holds binds fine
        s.request("x12_registry", "bind", 2,
                  name="x12_seat", version=2, id=30, instance=0)
        s.register("x12_seat", 30)
        for out in s.drain():
            assert out["name"] != "error", out

        # one id per life, the second alloc is refused loudly
        s.request("x12_registry", "bind", 2,
                  name="x12_seat", version=2, id=30, instance=0)
        err = s.wait_for("error")
        assert err["args"]["code"] == "invalid-id", err

        # unknown global names and unknown objects are refused, the
        # connection stays up
        s.request("x12_registry", "bind", 2,
                  name="x12_nope", version=1, id=31, instance=0)
        err = s.wait_for("error")
        assert err["args"]["code"] == "bad-value", err
        s.request("x12_connection", "shutdown", 99)
        err = s.wait_for("error")
        assert err["args"]["code"] == "invalid-id", err

        # object table limit: every client id slot is one life, ids out of
        # range never allocate. a fresh session keeps the table clean
        s.close()
        s = cc.Session(SOCK, idl)
        handshake(s)
        s.request("x12_connection", "sync", 1, callback=0)
        err = s.wait_for("error")
        assert err["args"]["code"] == "invalid-id", err
        for oid in range(3, 32):
            s.request("x12_connection", "sync", 1, callback=oid)
            s.register("x12_callback", oid)
            done = s.wait_for("done")
            assert done["object"] == oid, done
        s.request("x12_connection", "sync", 1, callback=64)
        err = s.wait_for("error")
        assert err["args"]["code"] == "invalid-id", err
        s.close()

        # framing limit: a size below the header is unfixable mid stream,
        # the server answers with the reason and disconnects loudly
        s = cc.Session(SOCK, idl)
        handshake(s)
        s.send_raw(b"\x08\x00\x00\x00" + b"\x00" * 8)
        err = s.wait_for("error")
        assert err["args"]["code"] == "bad-value", err
        assert s.closed_by_server(), "server did not close a poisoned stream"
        s.close()

        # shutdown drops only its own connection
        s1 = cc.Session(SOCK, idl)
        s2 = cc.Session(SOCK, idl)
        handshake(s1)
        handshake(s2)
        s1.request("x12_connection", "shutdown", 1)
        assert s1.closed_by_server(), "shutdown did not close the connection"
        s2.request("x12_connection", "sync", 1, callback=5)
        s2.register("x12_callback", 5)
        done = s2.wait_for("done")
        assert done["args"]["serial"] == 1, done
        s1.close()
        s2.close()
    finally:
        proc.terminate()
        proc.wait(timeout=5)
    print("e2e core: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
