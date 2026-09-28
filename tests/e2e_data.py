# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# end to end for the selection protocol: two sessions, one owns the
# clipboard and the primary, the other pulls offers and receives the
# content through a pipe fd. generations, finished offers, cancelled
# sources and the teardown paths are asserted in between

import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import core_client as cc  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOCK = ROOT / "build" / "x12-data.sock"


def handshake(s):
    s.request("x12_connection", "hello", 1, idl_major=1, idl_minor=0)
    s.wait_for("welcome")
    for _ in range(4):
        s.wait_for("global")


def bind_device(s, idl, dev_id):
    s.request("x12_registry", "bind", 2, name="x12_seat", version=2,
              id=10, instance=0)
    s.register("x12_seat", 10)
    s.request("x12_seat", "get_data_device", 10, id=dev_id)
    s.register("x12_data_device", dev_id)
    # the startup signals name the generation the device joins
    a = s.wait_for("selection")
    b = s.wait_for("primary")
    return a["args"]["generation"], b["args"]["generation"]


def expect(s, name, obj=None):
    ev = s.next_event()
    assert ev["name"] == name, ev
    if obj is not None:
        assert ev["object"] == obj, ev
    return ev


def main():
    idl = load_idl(ROOT / "spec" / "x12-core.yaml")
    proc = cc.spawn_server(ROOT / "build" / "x12-server", SOCK)
    try:
        owner = cc.Session(SOCK, idl)
        reader = cc.Session(SOCK, idl)
        handshake(owner)
        handshake(reader)
        o_sel, o_pri = bind_device(owner, idl, 20)
        r_sel, r_pri = bind_device(reader, idl, 30)
        assert (o_sel, o_pri, r_sel, r_pri) == (0, 0, 0, 0)

        # phase 1: the owner offers two mimes and claims both slots
        owner.request("x12_data_device", "create_source", 20, id=21)
        owner.register("x12_data_source", 21)
        owner.request("x12_data_source", "offer", 21, mime="text/plain")
        owner.request("x12_data_source", "offer", 21, mime="text/html")
        owner.request("x12_data_source", "offer", 21, mime="text/plain")  # dup
        owner.request("x12_data_device", "set_primary", 20, source=21)
        owner.request("x12_data_device", "set_selection", 20, source=21)
        # every bound device learns generation 2 (both slots moved)
        for s in (owner, reader):
            for name in ("primary", "selection"):
                ev = expect(s, name)
                assert ev["args"]["generation"] == 1, ev

        # phase 2: the reader pulls the mime list into its own offer
        reader.request("x12_data_device", "get_primary", 30, id=11)
        reader.register("x12_data_offer", 11)
        mimes = set()
        for _ in range(2):
            ev = expect(reader, "offer", 11)
            mimes.add(ev["args"]["mime"])
        assert mimes == {"text/plain", "text/html"}, mimes

        # phase 3: the content travels reader pipe write end to owner
        # and back as bytes, the reader keeps the read end
        pr, pw = os.pipe()
        reader.request("x12_data_offer", "receive", 11, fds=[pw],
                       mime="text/plain", fd=0)
        os.close(pw)
        ev = expect(owner, "send", 21)
        assert ev["args"]["mime"] == "text/plain", ev
        ofd = owner.recv_fds[-1]
        assert ofd >= 0, ev
        os.write(ofd, b"hello world")
        os.close(ofd)
        got = b""
        while True:
            chunk = os.read(pr, 64)
            if not chunk:
                break
            got += chunk
        os.close(pr)
        assert got == b"hello world", got

        # phase 4: a fresh claim cancels the old source and finishes the
        # stale offers, the generation reaches everyone
        owner.request("x12_data_device", "create_source", 20, id=22)
        owner.register("x12_data_source", 22)
        owner.request("x12_data_source", "offer", 22, mime="text/plain")
        owner.request("x12_data_device", "set_primary", 20, source=22)
        expect(owner, "cancelled", 21)
        expect(reader, "finished", 11)
        for s in (owner, reader):
            ev = expect(s, "primary")
            assert ev["args"]["generation"] == 2, ev
        pr, pw = os.pipe()
        reader.request("x12_data_offer", "receive", 11, fds=[pw],
                       mime="text/plain", fd=0)
        os.close(pw)
        os.close(pr)
        # the finished event took the object with it, a late receive
        # refuses loudly through the dispatch layer
        err = expect(reader, "error", 1)
        assert err["args"]["code"] == "invalid-id", err

        # phase 5: a null source clears the slot, the replaced owner is
        # cancelled and the empty slot refuses get loudly
        owner.request("x12_data_device", "set_primary", 20, source=0)
        expect(owner, "cancelled", 22)
        for s in (owner, reader):
            ev = expect(s, "primary")
            assert ev["args"]["generation"] == 3, ev
        reader.request("x12_data_device", "get_primary", 30, id=12)
        err = expect(reader, "error", 1)
        assert err["args"]["code"] == "bad-value", err
        # the clipboard slot still holds its generation 1 content
        reader.request("x12_data_device", "get_selection", 30, id=13)
        reader.register("x12_data_offer", 13)
        seen = set()
        for _ in range(2):
            ev = expect(reader, "offer", 13)
            seen.add(ev["args"]["mime"])
        assert seen == {"text/plain", "text/html"}, seen

        # phase 6: destroying the owning source drops its slot and the
        # offer pulled from it dies with a finished event
        owner.request("x12_data_source", "destroy", 21)
        expect(reader, "finished", 13)
        for s in (owner, reader):
            ev = expect(s, "selection")
            assert ev["args"]["generation"] == 2, ev
        reader.request("x12_data_device", "get_selection", 30, id=14)
        err = expect(reader, "error", 1)
        assert err["args"]["code"] == "bad-value", err
        # a source that owns nothing just goes away, no signals
        owner.request("x12_data_source", "destroy", 22)

        # phase 7: destroying a fresh owner drops the slot again
        owner.request("x12_data_device", "create_source", 20, id=23)
        owner.register("x12_data_source", 23)
        owner.request("x12_data_source", "offer", 23, mime="text/plain")
        owner.request("x12_data_device", "set_primary", 20, source=23)
        for s in (owner, reader):
            ev = expect(s, "primary")
            assert ev["args"]["generation"] == 4, ev
        owner.request("x12_data_source", "destroy", 23)
        for s in (owner, reader):
            ev = expect(s, "primary")
            assert ev["args"]["generation"] == 5, ev

        # phase 8: a dying owner drops what it held, the reader learns
        owner.request("x12_data_device", "create_source", 20, id=24)
        owner.register("x12_data_source", 24)
        owner.request("x12_data_source", "offer", 24, mime="text/plain")
        owner.request("x12_data_device", "set_primary", 20, source=24)
        for s in (owner, reader):
            ev = expect(s, "primary")
            assert ev["args"]["generation"] == 6, ev
        owner.close()
        ev = expect(reader, "primary")
        assert ev["args"]["generation"] == 7, ev
        reader.request("x12_data_device", "get_primary", 30, id=15)
        err = expect(reader, "error", 1)
        assert err["args"]["code"] == "bad-value", err

        reader.close()
    finally:
        proc.terminate()
        proc.wait(timeout=5)
    print("e2e data: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
