# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

"""compile x12 idl into c wire tables and verify field alignment."""

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
SPEC = ROOT / "spec" / "x12-core.yaml"


@dataclass
class Field:
    name: str
    type: str
    c_type: str
    array: bool
    size: int
    align: int
    offset: int
    present: bool


@dataclass
class Message:
    ifname: str
    direction: str  # request or event
    name: str
    opcode: int
    version: int
    fields: list
    payload_size: int


HEADER_SIZE = 12
TS_SIZE = 8
TYPE_SIZES = {
    "u8": 1, "u16": 2, "u32": 4, "u64": 8,
    "i32": 4, "fixed": 4,
    "str": 4, "id": 4, "fd": 0,
    "strarray": 8, "idarray": 8,
}
ARRAY_TYPES = {"strarray", "idarray"}
STR_TYPES = {"str", "strarray"}


def c_type(typ: str, types: dict) -> str:
    if typ in types:
        return types[typ]["c"]
    return f"x12_id"


def align_to(value: int, align: int) -> int:
    return (value + align - 1) & ~(align - 1)


def load_idl(path: Path) -> dict:
    with open(path, "r", encoding="utf-8") as handle:
        data = yaml.safe_load(handle)
    return data


def build_messages(idl: dict) -> list:
    proto = idl["protocol"]
    types = proto["types"]
    messages = []
    for ifname, iface in sorted(idl["interfaces"].items()):
        for direction in ("requests", "events"):
            entries = iface.get(direction, {}) or {}
            for msgname, meta in sorted(entries.items(), key=lambda kv: kv[1]["opcode"]):
                fields = []
                # implicit timestamp after the 12 byte header, always u64.
                fields.append(Field("__ts", "u64", "u64", False, 8, 8, HEADER_SIZE, False))
                offset = HEADER_SIZE + TS_SIZE
                for arg in meta["args"]:
                    if isinstance(arg, str):
                        name, typ, present = arg, "u32", True
                    else:
                        name, typ = arg["name"], arg["type"]
                        present = arg.get("present", True)
                    array = typ in ARRAY_TYPES
                    size = TYPE_SIZES[typ]
                    align = 8 if array else size
                    if typ == "fd":
                        align = 0
                    offset = align_to(offset, align) if align else offset
                    fields.append(Field(name, typ, c_type(typ, types), array, size, align, offset, present))
                    if align:
                        offset += size
                messages.append(Message(ifname, "request" if direction == "requests" else "event",
                                        msgname, meta["opcode"], meta.get("since", 1), fields, offset))
    return messages


def payload_size(fields: list) -> int:
    return max(f.offset + (f.size if f.align else 0) for f in fields)


def header_bytes(message: Message) -> int:
    return HEADER_SIZE + payload_size(message.fields)


def _align_pad(fields: list, sizes: dict) -> list:
    """return copy of fields where fd fields widen to pad the wire slot.

    fd does not travel on the wire, the idl still reserves its slot so the
    generated codec mirrors the fixed header rules of the reference model.
    """
    result = []
    for field in fields:
        if field.type != "fd":
            result.append(field)
            continue
        idx = fields.index(field)
        width = 0
        if idx + 1 < len(fields):
            nxt = fields[idx + 1]
            if nxt.align == 8 and field.offset % 8:
                width = 8 - (field.offset % 8)
        result.append(Field(field.name, "pad", "u8", False, width, 1, field.offset, True))
    return result


def render_struct(message: Message, idl: dict) -> str:
    lines = [f"typedef struct x12_{message.ifname}_{message.name}_t {{"]
    lines.append(f"    uint32_t object_id;")
    for field in _align_pad(message.fields, idl["protocol"]["types"]):
        if field.name == "__ts":
            lines.append(f"    uint64_t timestamp;")
            continue
        if field.type == "pad":
            lines.append(f"    uint8_t {field.name}[{field.size}];")
            continue
        ctype = field.c_type
        if field.type in STR_TYPES:
            lines.append(f"    {ctype} {field.name};")
        elif field.array:
            lines.append(f"    const {ctype} {field.name};")
            lines.append(f"    size_t {field.name}_count;")
        else:
            lines.append(f"    {ctype} {field.name};")
    lines.append(f"}} x12_{message.ifname}_{message.name}_t;")
    return "\n".join(lines)


def gen_c(idl: dict, messages: list, outdir: Path) -> None:
    outdir.mkdir(parents=True, exist_ok=True)
    header = [
        "// Copyright (c) lezginskiy <xanother-x12@proton.me>",
        "// SPDX-License-Identifier: MIT",
        "",
        "// generated from spec/x12-core.yaml, do not edit.",
        "",
        "#pragma once",
        "#include <stddef.h>",
        "#include <stdint.h>",
        '#include "x12_wire.h"',
        "",
    ]
    for message in messages:
        header.append(render_struct(message, idl))
        header.append("")
    (outdir / "x12_protocol.h").write_text("\n".join(header), encoding="utf-8")

    sizes = []
    tables = [
        "// Copyright (c) lezginskiy <xanother-x12@proton.me>",
        "// SPDX-License-Identifier: MIT",
        "",
        "// generated from spec/x12-core.yaml, do not edit.",
        "",
        '#include "x12_protocol.h"',
        "",
    ]
    for message in messages:
        token = f"{message.ifname}_{message.name}"
        tables.append(f"static const x12_arg_desc_t args_{token}[] = {{")
        for field in _align_pad(message.fields, idl["protocol"]["types"]):
            name = "timestamp" if field.name == "__ts" else field.name
            kind = {"str": "str", "strarray": "strarray", "id": "id", "idarray": "idarray",
                    "fd": "fd", "pad": "pad"}.get(field.type, "uint")
            enum_name = "NULL"
            tables.append(
                f'    {{ "{name}", "{field.type}", "{kind}", {field.size}, {field.offset}, '
                f'{1 if field.present else 0}, {field.size if field.align else 0}, {enum_name} }},'
            )
        tables.append("};")
        sizes.append(f"    sizeof(x12_{token}_t)")
    tables.append("")
    tables.append(f"const size_t x12_generated_struct_sizes[{len(messages) + 1}] = {{")
    tables.append("    0,")
    for value in sizes:
        tables.append(f"{value},")
    tables.append("};")
    tables.append("")
    index = 0
    enum_names = {}
    for enum_name, items in sorted(idl["protocol"]["enums"].items()):
        enum_names[enum_name] = {item["name"]: item["id"] for item in items}
    for message in messages:
        token = f"{message.ifname}_{message.name}"
        index += 1
        iface_version = idl["interfaces"][message.ifname]["version"]
        tables.append(
            f'const x12_msg_desc_t x12_msg_{token} = {{ "{message.ifname}", "{message.name}", '
            f'"{token}", {message.ifname.count("_")}, {message.ifname}, {index}, {message.ifname.count("_")}, '
            f'{message.ifname.count("_")}, {message.ifname.count("_")}, {iface_version}, '
            f'{message.ifname.count("_")}, "{message.ifname}.{message.name}", {message.opcode}, '
            f'"{message.direction}", {message.ifname.count("_")}, {header_bytes(message)}, '
            f'{message.ifname.count("_")}, {len(message.fields)}, args_{token} }};'
        )
    (outdir / "x12_tables.c").write_text("\n".join(tables), encoding="utf-8")


def check_alignment(messages: list) -> list:
    problems = []
    for message in messages:
        for field in message.fields:
            if field.align and field.offset % field.align:
                problems.append(f"{message.ifname}.{message.name}.{field.name} offset {field.offset} align {field.align}")
        size = header_bytes(message)
        if size % 8:
            problems.append(f"{message.ifname}.{message.name} message size {size} not aligned")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("idl", nargs="?", default=str(SPEC))
    ap.add_argument("--out", default=None)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--lang", default="c", choices=["c"])
    args = ap.parse_args()

    idl = load_idl(Path(args.idl))
    messages = build_messages(idl)

    if args.check:
        problems = check_alignment(messages)
        for problem in problems:
            print(problem)
        print(f"{len(messages)} messages checked, {len(problems)} problems")
        return 1 if problems else 0

    if args.lang == "c":
        gen_c(idl, messages, Path(args.out or ROOT / "build" / "generated"))
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main())
