# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

import struct
import unittest
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import wire_model as wm  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
IDL_PATH = ROOT / "spec" / "x12-core.yaml"


class WireRoundtrip(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.idl = load_idl(IDL_PATH)

    def _roundtrip_request(self, iface, msg, **args):
        buf = wm.encode_request(self.idl, iface, msg, object_id=42, **args)
        return wm.decode(self.idl, iface, "request", buf)

    def _roundtrip_event(self, iface, msg, ts, **args):
        buf = wm.encode_event(self.idl, iface, msg, object_id=7, timestamp_ns=ts, **args)
        return wm.decode(self.idl, iface, "event", buf)

    def test_nullable_buffer_zero_is_valid_absence(self):
        out = self._roundtrip_request("x12_surface", "set_buffer", buffer=0)
        self.assertEqual(out["args"]["buffer"], 0)
        self.assertEqual(out["name"], "set_buffer")

    def test_fixed24p8_negative_scale_survives(self):
        raw = wm.to_fixed24p8(-1.5)
        self.assertEqual(raw, -384)
        out = self._roundtrip_request("x12_surface", "set_scale", scale=raw)
        self.assertEqual(out["args"]["scale"], -384)

    def test_event_carries_implicit_timestamp(self):
        out = self._roundtrip_event("x12_pointer", "motion", ts=1_700_000_000_000,
                                    surface=42,
                                    x=wm.to_fixed24p8(12.25),
                                    y=wm.to_fixed24p8(0.5))
        self.assertEqual(out["timestamp_ns"], 1_700_000_000_000)
        self.assertEqual(out["args"]["x"], 3136)

    def test_event_header_size_field_covers_timestamp_and_args(self):
        buf = wm.encode_event(self.idl, "x12_pointer", "motion", object_id=7,
                              timestamp_ns=1, surface=1, x=0, y=0)
        (size,) = struct.unpack_from("<I", buf, 0)
        self.assertEqual(size, len(buf))
        self.assertEqual(size, 12 + 8 + 12)

    def test_enum_symbols_map_by_declaration_order(self):
        out = self._roundtrip_request("x12_surface", "set_sync", policy="async")
        self.assertEqual(out["args"]["policy"], "async")
        buf = wm.encode_request(self.idl, "x12_surface", "set_sync",
                                object_id=1, policy="async")
        raw = struct.unpack_from("<I", buf, 12)[0]
        self.assertEqual(raw, 1)

    def test_fd_slot_is_index_not_descriptor(self):
        out = self._roundtrip_event("x12_keyboard", "keymap", ts=5,
                                    format="xkb-v1", fd=3, size=4096)
        self.assertEqual(out["args"]["fd"], 3)

    def test_utf8_string_multibyte_and_padding(self):
        title = "поверхность"
        out = self._roundtrip_request("x12_toplevel", "set_title", title=title)
        self.assertEqual(out["args"]["title"], title)
        buf = wm.encode_request(self.idl, "x12_toplevel", "set_title",
                                object_id=1, title=title)
        # 11 cyrillic chars are 22 bytes, padded to 24
        self.assertEqual(struct.unpack_from("<I", buf, 12)[0], 22)
        self.assertEqual(buf[12 + 4 + 22:12 + 4 + 24], b"\x00\x00")

    def test_capability_array_roundtrip(self):
        out = self._roundtrip_event("x12_connection", "welcome", ts=9,
                                    caps=[0, 1, 2], server_version=1)
        self.assertEqual(out["args"]["caps"], [0, 1, 2])

    def test_committed_event_carries_commit_token(self):
        # the token is what present echoes later, it must survive the wire
        out = self._roundtrip_event("x12_surface", "committed", ts=11, token=17)
        self.assertEqual(out["args"]["token"], 17)

    def test_present_names_the_commit_token(self):
        out = self._roundtrip_event("x12_output", "present", ts=12,
                                    commit_token=17, kind="flip")
        self.assertEqual(out["args"]["commit_token"], 17)
        self.assertEqual(out["args"]["kind"], "flip")

    def test_decoder_rejects_size_mismatch(self):
        buf = wm.encode_request(self.idl, "x12_surface", "commit", object_id=1)
        # append one byte so the size field no longer matches the buffer
        with self.assertRaises(wm.WireError):
            wm.decode(self.idl, "x12_surface", "request", buf + b"\x00")

    def test_decoder_rejects_nonzero_reserved(self):
        buf = bytearray(wm.encode_request(self.idl, "x12_surface", "commit", object_id=1))
        buf[11] = 1
        with self.assertRaises(wm.WireError):
            wm.decode(self.idl, "x12_surface", "request", bytes(buf))

    def test_decoder_rejects_unknown_opcode(self):
        buf = bytearray(wm.encode_request(self.idl, "x12_surface", "commit", object_id=1))
        buf[8] = 99
        with self.assertRaises(wm.WireError):
            wm.decode(self.idl, "x12_surface", "request", bytes(buf))

    def test_decoder_rejects_interior_null_string(self):
        with self.assertRaises(wm.WireError):
            wm.encode_request(self.idl, "x12_toplevel", "set_title",
                              object_id=1, title="bad\x00title")

    def test_decoder_rejects_truncated_array_count(self):
        # a frame cut inside the array count must raise WireError and
        # nothing else, struct.error escaping is a model crash
        buf = wm.encode_event(self.idl, "x12_connection", "welcome", object_id=1,
                              timestamp_ns=1, caps=[1, 2, 3], server_version=1)
        with self.assertRaises(wm.WireError):
            wm.decode(self.idl, "x12_connection", "event", buf[:-1])


if __name__ == "__main__":
    unittest.main()
