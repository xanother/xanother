# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# generates one golden vector per idl message with deterministic sample args.
# vectors drive the c roundtrip runner and seed the fuzzer, so the python
# model, the c engine and the fuzz corpus all start from the same bytes

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import wire_model as wm  # noqa: E402


def sample_args(msg):
    args = {}
    for arg in msg.get("args") or []:
        name, t = arg["name"], arg["type"]
        nullable = arg.get("nullable", False)
        if t == "uint32":
            args[name] = 0 if nullable else 42
        elif t == "fd":
            args[name] = 0 if nullable else 3
        elif t in ("object", "new_id"):
            args[name] = 0 if nullable and t == "object" else 42
        elif t == "int32":
            args[name] = -12345
        elif t == "uint64":
            args[name] = 1700000000123
        elif t == "int64":
            args[name] = -5
        elif t == "fixed24p8":
            args[name] = -384
        elif t == "enum":
            # prepared by the caller in arg["_first_symbol"]
            args[name] = arg["_first_symbol"]
        elif t == "string":
            args[name] = arg["_sample_string"]
        elif t == "array_u32":
            args[name] = list(range(4))
        else:
            raise ValueError(f"unhandled type {t}")
    return args


def resolve_enum_lookup(idl):
    # mirrors wire_model resolution: interface level first, root second
    lookup = {}
    for iface in idl["interfaces"]:
        for name, symbols in (idl.get("enums") or {}).items():
            lookup[(iface["name"], name)] = symbols
        for name, symbols in (iface.get("enums") or {}).items():
            lookup[(iface["name"], name)] = symbols
    return lookup


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--idl", default="spec/x12-core.yaml")
    ap.add_argument("--out", default="build/gen/vectors.hex")
    args = ap.parse_args(argv)

    idl = load_idl(args.idl)
    lookup = resolve_enum_lookup(idl)
    strings = ["xanother", "поверхность", "a longer title for coverage"]

    lines = []
    for iface in idl["interfaces"]:
        for kind_key, kind in (("requests", "request"), ("events", "event")):
            for msg in iface.get(kind_key, []) or []:
                for arg in msg.get("args") or []:
                    if arg["type"] == "enum":
                        arg["_first_symbol"] = lookup[(iface["name"], arg["of"])][0]
                    elif arg["type"] == "string":
                        arg["_sample_string"] = strings[(len(lines)) % len(strings)]
                sargs = sample_args(msg)
                ts = 1700000000000 + len(lines)
                if kind == "request":
                    buf = wm.encode_request(idl, iface["name"], msg["name"], object_id=42, **sargs)
                else:
                    buf = wm.encode_event(idl, iface["name"], msg["name"], object_id=42,
                                          timestamp_ns=ts, **sargs)
                lines.append(f"{iface['name']}\t{msg['name']}\t{kind}\t{buf.hex()}")

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"wrote {len(lines)} vectors to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
