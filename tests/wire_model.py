# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# reference encoder and decoder for the x12 wire format, driven only by the
# parsed idl. the generated c codec must agree with this model byte for
# byte. interface identity is not on the wire, it comes from the connection
# object table, so decoding needs the interface passed explicitly.

import struct

HEADER_SIZE = 12
TIMESTAMP_SIZE = 8


class WireError(ValueError):
    pass


def _align4(n):
    return (n + 3) & ~3


def _enum_symbols(idl, iface, enum_name):
    # mirrors x12gen resolution: interface level first, root level second
    symbols = (iface.get("enums") or {}).get(enum_name)
    if symbols is None:
        symbols = (idl.get("enums") or {}).get(enum_name)
    return symbols


def _enum_value(idl, iface, enum_name, symbol):
    symbols = _enum_symbols(idl, iface, enum_name)
    if symbols is None or symbol not in symbols:
        raise WireError(f"{iface['name']}.{enum_name}: unknown symbol {symbol}")
    return symbols.index(symbol)


def _find_msg(iface, kind, msg_name):
    key = "requests" if kind == "request" else "events"
    for msg in iface.get(key, []) or []:
        if msg["name"] == msg_name:
            return msg
    raise WireError(f"{iface['name']}: no {kind} named {msg_name}")


def _encode_args(iface, msg, args, idl, fds):
    chunks = []
    for arg in msg.get("args", []) or []:
        name, atype = arg["name"], arg["type"]
        if name not in args:
            raise WireError(f"{iface['name']}.{msg['name']}: missing arg {name}")
        value = args[name]

        if atype in ("uint32", "uint64"):
            fmt = "<I" if atype == "uint32" else "<Q"
            chunks.append(struct.pack(fmt, value))
        elif atype in ("int32", "int64"):
            fmt = "<i" if atype == "int32" else "<q"
            chunks.append(struct.pack(fmt, value))
        elif atype == "fixed24p8":
            # raw fixed point, callers pass the raw register value
            chunks.append(struct.pack("<i", value))
        elif atype == "enum":
            chunks.append(struct.pack("<I", _enum_value(idl, iface, arg["of"], value)))
        elif atype in ("object", "new_id"):
            chunks.append(struct.pack("<I", value))
        elif atype == "fd":
            # the wire slot carries the ancillary index, not the descriptor
            chunks.append(struct.pack("<I", value))
        elif atype == "string":
            raw = value.encode("utf-8")
            if b"\x00" in raw:
                raise WireError("strings must not contain interior null bytes")
            padded = _align4(len(raw))
            chunks.append(struct.pack("<I", len(raw)))
            chunks.append(raw + b"\x00" * (padded - len(raw)))
        elif atype == "bytes":
            raw = value if isinstance(value, bytes) else value.encode("latin-1")
            padded = _align4(len(raw))
            chunks.append(struct.pack("<I", len(raw)))
            chunks.append(raw + b"\x00" * (padded - len(raw)))
        elif atype == "array_u32":
            chunks.append(struct.pack("<I", len(value)))
            chunks.extend(struct.pack("<I", v) for v in value)
        else:
            raise WireError(f"unhandled type {atype}")
    return b"".join(chunks)


def encode_request(idl, iface_name, msg_name, object_id, **args):
    iface = _iface(idl, iface_name)
    msg = _find_msg(iface, "request", msg_name)
    payload = _encode_args(iface, msg, args, idl, fds=[])
    return _frame(msg["opcode"], object_id, payload)


def encode_event(idl, iface_name, msg_name, object_id, timestamp_ns, **args):
    iface = _iface(idl, iface_name)
    msg = _find_msg(iface, "event", msg_name)
    payload = struct.pack("<Q", timestamp_ns) + _encode_args(iface, msg, args, idl, fds=[])
    return _frame(msg["opcode"], object_id, payload)


def _frame(opcode, object_id, payload):
    size = HEADER_SIZE + len(payload)
    return struct.pack("<IIHH", size, object_id, opcode, 0) + payload


def _iface(idl, iface_name):
    for iface in idl["interfaces"]:
        if iface["name"] == iface_name:
            return iface
    raise WireError(f"unknown interface {iface_name}")


def decode(idl, iface_name, kind, buf):
    # kind is either request or event, the sender side of the object decides it
    iface = _iface(idl, iface_name)
    if len(buf) < HEADER_SIZE:
        raise WireError("truncated header")
    size, object_id, opcode, reserved = struct.unpack_from("<IIHH", buf, 0)
    if size != len(buf):
        raise WireError(f"size field {size} does not match buffer {len(buf)}")
    if reserved != 0:
        raise WireError("reserved field must be zero")
    key = "requests" if kind == "request" else "events"
    msg = next((m for m in iface.get(key, []) or [] if m["opcode"] == opcode), None)
    if msg is None:
        raise WireError(f"{iface['name']}: opcode {opcode} is not a {kind} opcode")
    offset = HEADER_SIZE
    timestamp = None
    if kind == "event":
        timestamp = struct.unpack_from("<Q", buf, offset)[0]
        offset += TIMESTAMP_SIZE
    args, offset = _decode_args(idl, iface, msg, buf, offset)
    if offset != len(buf):
        raise WireError("trailing bytes after message")
    return {"interface": iface_name, "kind": kind, "name": msg["name"],
            "object": object_id, "timestamp_ns": timestamp, "args": args}


def _need(buf, offset, n):
    # every scalar read is bounds checked here, struct.error must never
    # escape as a crash, a short frame is wire garbage not a python error
    if offset + n > len(buf):
        raise WireError("argument exceeds buffer")


def _decode_args(idl, iface, msg, buf, offset):
    args = {}
    for arg in msg.get("args", []) or []:
        name, atype = arg["name"], arg["type"]
        if atype in ("uint32", "enum", "fd", "object", "new_id"):
            _need(buf, offset, 4)
            args[name] = struct.unpack_from("<I", buf, offset)[0]
            if atype == "enum":
                args[name] = _enum_symbol(idl, iface, arg["of"], args[name])
            offset += 4
        elif atype == "int32" or atype == "fixed24p8":
            _need(buf, offset, 4)
            args[name] = struct.unpack_from("<i", buf, offset)[0]
            offset += 4
        elif atype == "uint64":
            _need(buf, offset, 8)
            args[name] = struct.unpack_from("<Q", buf, offset)[0]
            offset += 8
        elif atype == "int64":
            _need(buf, offset, 8)
            args[name] = struct.unpack_from("<q", buf, offset)[0]
            offset += 8
        elif atype == "string":
            _need(buf, offset, 4)
            (length,) = struct.unpack_from("<I", buf, offset)
            offset += 4
            padded = _align4(length)
            if offset + padded > len(buf):
                raise WireError("string exceeds buffer")
            raw = buf[offset:offset + length]
            if b"\x00" in raw:
                raise WireError("strings must not contain interior null bytes")
            try:
                text = raw.decode("utf-8")
            except UnicodeDecodeError:
                raise WireError("string is not valid utf-8")
            offset += padded
            if any(buf[offset - (padded - length):offset]):
                raise WireError("string padding must be zero")
            args[name] = text
        elif atype == "bytes":
            _need(buf, offset, 4)
            (length,) = struct.unpack_from("<I", buf, offset)
            offset += 4
            padded = _align4(length)
            if offset + padded > len(buf):
                raise WireError("bytes exceed buffer")
            raw = buf[offset:offset + length]
            offset += padded
            if any(buf[offset - (padded - length):offset]):
                raise WireError("bytes padding must be zero")
            args[name] = raw
        elif atype == "array_u32":
            _need(buf, offset, 4)
            (count,) = struct.unpack_from("<I", buf, offset)
            offset += 4
            if offset + 4 * count > len(buf):
                raise WireError("array exceeds buffer")
            args[name] = list(struct.unpack_from(f"<{count}I", buf, offset))
            offset += 4 * count
        else:
            raise WireError(f"unhandled type {atype}")
    return args, offset


def _enum_symbol(idl, iface, enum_name, value):
    symbols = _enum_symbols(idl, iface, enum_name)
    if symbols is None or value >= len(symbols):
        raise WireError(f"{iface['name']}.{enum_name}: bad value {value}")
    return symbols[value]


def to_fixed24p8(f):
    return round(f * 256)
