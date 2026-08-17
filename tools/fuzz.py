# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# wire fuzzer, the phase gate. invariants over the python reference model:
# decoding raises nothing but WireError, every accepted frame re-encodes to
# the exact same bytes, and every message of the idl is exercised at least
# once. any violation exits nonzero, which fails the build

import argparse
import random
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl  # noqa: E402
from tests import wire_model as wm  # noqa: E402


def load_corpus(path):
    corpus = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        iface, msg, kind, hexdata = line.split("\t")
        corpus.append((iface, msg, kind, bytes.fromhex(hexdata)))
    return corpus


def mutate(rng, data):
    kind = rng.randrange(0, 4)
    buf = bytearray(data)
    if kind == 0:  # bit flips
        for _ in range(rng.randrange(1, 8)):
            i = rng.randrange(0, len(buf))
            buf[i] ^= 1 << rng.randrange(0, 8)
    elif kind == 1:  # truncation
        cut = rng.randrange(0, len(buf))
        del buf[cut:]
    elif kind == 2:  # garbage extension
        for _ in range(rng.randrange(1, 16)):
            buf.append(rng.randrange(0, 256))
    else:  # splice with another frame
        cut = rng.randrange(0, len(buf))
        del buf[cut:]
    return bytes(buf)


def random_arg(rng, arg, enum_lookup, iface_name):
    t = arg["type"]
    nullable = arg.get("nullable", False)
    if t in ("uint32", "fd", "object", "new_id"):
        return 0 if nullable and rng.random() < 0.5 else rng.randrange(0, 2 ** 32)
    if t == "int32":
        return rng.randrange(-2 ** 31, 2 ** 31)
    if t == "uint64":
        return rng.randrange(0, 2 ** 64)
    if t == "int64":
        return rng.randrange(-2 ** 63, 2 ** 63)
    if t == "fixed24p8":
        return rng.randrange(-2 ** 31, 2 ** 31)
    if t == "enum":
        symbols = enum_lookup[(iface_name, arg["of"])]
        return symbols[rng.randrange(0, len(symbols))]
    if t == "string":
        alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789абвгд"
        n = rng.randrange(0, 16)
        return "".join(rng.choice(alphabet) for _ in range(n))
    if t == "array_u32":
        return [rng.randrange(0, 2 ** 32) for _ in range(rng.randrange(0, 5))]
    raise ValueError(f"unhandled type {t}")


def encode_one(idl, iface, msg_name, kind, object_id, ts, args):
    if kind == "request":
        return wm.encode_request(idl, iface, msg_name, object_id=object_id, **args)
    return wm.encode_event(idl, iface, msg_name, object_id=object_id,
                           timestamp_ns=ts, **args)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("--idl", default="spec/x12-core.yaml")
    ap.add_argument("--corpus", default="build/gen/vectors.hex")
    ap.add_argument("--iterations", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=1337)
    args = ap.parse_args(argv)

    rng = random.Random(args.seed)
    idl = load_idl(args.idl)
    corpus = load_corpus(args.corpus)
    if not corpus:
        print("fuzz gate: empty corpus")
        return 1

    enum_lookup = {}
    for iface in idl["interfaces"]:
        for name, symbols in (idl.get("enums") or {}).items():
            enum_lookup[(iface["name"], name)] = symbols
        for name, symbols in (iface.get("enums") or {}).items():
            enum_lookup[(iface["name"], name)] = symbols

    total_msgs = sum(
        len(i.get("requests") or []) + len(i.get("events") or [])
        for i in idl["interfaces"]
    )

    failures = []

    # directed phase: every message must encode, decode and re-encode cleanly
    covered = set()
    for iface, msg, kind, data in corpus:
        try:
            out = wm.decode(idl, iface, kind, data)
        except wm.WireError as e:
            failures.append(f"golden vector rejected: {iface}.{msg}: {e}")
            continue
        covered.add((iface, msg))
        try:
            re_bytes = encode_one(idl, iface, out["name"], kind,
                                  out["object"], out["timestamp_ns"], out["args"])
        except wm.WireError as e:
            failures.append(f"golden vector re-encode refused: {iface}.{msg}: {e}")
            continue
        if re_bytes != data:
            failures.append(f"golden vector not canonical: {iface}.{msg}")
    if len(covered) != total_msgs:
        failures.append(f"coverage {len(covered)}/{total_msgs}, every message must be exercised")

    # mutation phase: corrupted frames must be rejected or accepted canonically
    accepted = rejected = 0
    for _ in range(args.iterations):
        iface, msg, kind, data = corpus[rng.randrange(0, len(corpus))]
        buf = mutate(rng, data)
        try:
            out = wm.decode(idl, iface, kind, buf)
        except wm.WireError:
            rejected += 1
        except Exception as e:  # anything but WireError is a model crash
            failures.append(f"decoder crash on {iface}.{kind}: {type(e).__name__}: {e}")
            break
        else:
            accepted += 1
            try:
                re_bytes = encode_one(idl, iface, out["name"], kind,
                                      out["object"], out["timestamp_ns"], out["args"])
            except wm.WireError as e:
                failures.append(f"accepted frame cannot re-encode: {iface}.{out['name']}: {e}")
                break
            if re_bytes != buf:
                failures.append(f"accepted frame not canonical: {iface}.{out['name']}")
                break

    # arg fuzz phase: valid structure, wild values, deep into string and
    # array paths
    for _ in range(args.iterations):
        iface_name = rng.choice([i["name"] for i in idl["interfaces"]])
        iface = next(i for i in idl["interfaces"] if i["name"] == iface_name)
        kind_key, kind = (("requests", "request"), ("events", "event"))[rng.randrange(0, 2)]
        msgs = iface.get(kind_key) or []
        if not msgs:
            continue
        msg = rng.choice(msgs)
        sargs = {a["name"]: random_arg(rng, a, enum_lookup, iface_name)
                 for a in msg.get("args") or []}
        ts = rng.randrange(0, 2 ** 63)
        try:
            buf = encode_one(idl, iface_name, msg["name"], kind, rng.randrange(0, 2 ** 32), ts, sargs)
        except wm.WireError:
            rejected += 1
            continue
        except Exception as e:
            failures.append(f"encoder crash on {iface_name}.{msg['name']}: {type(e).__name__}: {e}")
            break
        # mangle payload bytes but keep framing plausible
        mangled = bytearray(buf)
        if len(mangled) > 16:
            for _ in range(rng.randrange(1, 6)):
                mangled[rng.randrange(12, len(mangled))] = rng.randrange(0, 256)
        try:
            out = wm.decode(idl, iface_name, kind, bytes(mangled))
        except wm.WireError:
            rejected += 1
        except Exception as e:
            failures.append(f"decoder crash on {iface_name}.{kind}: {type(e).__name__}: {e}")
            break
        else:
            accepted += 1
            try:
                re_bytes = encode_one(idl, iface_name, out["name"], kind,
                                      out["object"], out["timestamp_ns"], out["args"])
            except wm.WireError as e:
                failures.append(f"accepted frame cannot re-encode: {iface_name}.{out['name']}: {e}")
                break
            if re_bytes != bytes(mangled):
                failures.append(f"accepted frame not canonical: {iface_name}.{out['name']}")
                break

    print(f"fuzz gate: {args.iterations}x2 iterations, accepted {accepted}, "
          f"rejected {rejected}, coverage {len(covered)}/{total_msgs}")
    if failures:
        for f in failures:
            print("FAIL: " + f)
        return 1
    print("fuzz gate: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
