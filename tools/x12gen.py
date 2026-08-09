# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

# codegen for the x12 protocol, reads the idl and emits c and ada
# declarations. generation is a build step, the emitted files are never
# committed. the wire layout rules here must stay in lockstep with
# tests/wire_model.py, the test suite fails on any divergence.

import argparse
import sys
from pathlib import Path

import yaml

KNOWN_ARG_TYPES = {
    "uint32", "int32", "uint64", "int64",
    "fixed24p8", "string", "fd", "object", "new_id", "array_u32", "enum",
}


class _StrictLoader(yaml.SafeLoader):
    pass


def _no_dup_keys(loader, node, deep=False):
    seen = {}
    for k_node, v_node in node.value:
        key = loader.construct_object(k_node, deep=deep)
        if key in seen:
            raise yaml.constructor.ConstructorError(
                None, None, f"duplicate key {key!r} in mapping", node.start_mark)
        seen[key] = loader.construct_object(v_node, deep=deep)
    return seen


_StrictLoader.add_constructor(
    yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, _no_dup_keys)


def load_idl(path):
    raw = yaml.load(Path(path).read_text(encoding="utf-8"), Loader=_StrictLoader)
    errors = validate(raw)
    if errors:
        raise ValueError("idl validation failed:\n" + "\n".join("- " + e for e in errors))
    return raw


def collect_enums(root):
    # flat list in declaration order, index is the wire stable enum id used
    # by generated tables; names must be globally unique (validator enforces)
    entries = list((root.get("enums") or {}).items())
    for iface in root["interfaces"]:
        entries.extend((iface.get("enums") or {}).items())
    return entries


def validate(root):
    errors = []
    proto = root.get("protocol", {})
    if proto.get("idl_format") != 1:
        errors.append("protocol.idl_format must be 1")
    wire = proto.get("wire", {})
    if wire.get("byte_order") != "little-endian" or wire.get("alignment") != 4:
        errors.append("wire.byte_order and wire.alignment are fixed by the spec")

    interfaces = root.get("interfaces", [])
    names = [i.get("name") for i in interfaces]
    if len(names) != len(set(names)):
        errors.append("duplicate interface names")

    enum_names = list((root.get("enums") or {}).keys())
    for iface in interfaces:
        enum_names.extend((iface.get("enums") or {}).keys())
    if len(enum_names) != len(set(enum_names)):
        errors.append("enum names must be unique across the protocol")

    for iface in interfaces:
        iname = iface.get("name", "<no name>")
        if not iname.startswith("x12_"):
            errors.append(f"{iname}: interface names must start with x12_")
        if iface.get("version", 0) < 1:
            errors.append(f"{iname}: version must be >= 1")
        kinds = (("requests", "opcode"), ("events", "opcode"))
        for list_key, opcode_key in kinds:
            seen = set()
            for msg in iface.get(list_key, []) or []:
                mname = msg.get("name", "<no name>")
                op = msg.get(opcode_key)
                if op is None:
                    errors.append(f"{iname}.{mname}: missing {opcode_key}")
                elif op in seen:
                    errors.append(f"{iname}.{mname}: duplicate {opcode_key} {op}")
                else:
                    seen.add(op)
                for arg in msg.get("args", []) or []:
                    errors.extend(_check_arg(iname, mname, arg, names, iface, root.get("enums") or {}))

        # enum symbols are wire values, their declaration order is the value
        for ename, symbols in (iface.get("enums") or {}).items():
            if len(symbols) != len(set(symbols)):
                errors.append(f"{iname}.{ename}: duplicate enum symbols")
            for s in symbols:
                if not s.replace("-", "").replace("_", "").isalnum():
                    errors.append(f"{iname}.{ename}.{s}: bad symbol charset")
    return errors


def _enum_declared(iface_enums, root_enums, name):
    return name in iface_enums or name in root_enums


def _check_arg(iname, mname, arg, interface_names, iface, root_enums):
    errors = []
    aname = arg.get("name", "<no name>")
    atype = arg.get("type")
    if atype not in KNOWN_ARG_TYPES:
        errors.append(f"{iname}.{mname}.{aname}: unknown type {atype}")
    if atype == "enum" and not _enum_declared(iface.get("enums") or {}, root_enums, arg.get("of")):
        errors.append(f"{iname}.{mname}.{aname}: enum.of {arg.get('of')} not declared")
    if atype == "array_u32":
        of = arg.get("of")
        if not of:
            errors.append(f"{iname}.{mname}.{aname}: array_u32 requires 'of'")
        elif of != "uint32" and not _enum_declared(iface.get("enums") or {}, root_enums, of):
            errors.append(f"{iname}.{mname}.{aname}: array of {of} not declared")
    if arg.get("nullable") and atype not in ("object", "fd"):
        errors.append(f"{iname}.{mname}.{aname}: only object and fd may be nullable")
    if atype in ("object", "new_id") and arg.get("interface") and \
            arg["interface"] not in interface_names:
        errors.append(f"{iname}.{mname}.{aname}: interface {arg['interface']} not declared")
    return errors


def _macro_prefix(iname):
    # x12_connection -> X12_CONNECTION
    return iname.upper()


def _c_enum_name(iname, ename):
    # enums already named x12_* keep their name, others get the interface prefix
    return ename if ename.startswith("x12_") else f"{iname}_{ename}"


def gen_c(root):
    out = []
    out.append("// Copyright (c) lezginskiy <xanother-x12@proton.me>")
    out.append("// SPDX-License-Identifier: MIT")
    out.append("//")
    out.append("// generated from spec/x12-core.yaml by x12gen, do not edit")
    out.append("#ifndef X12_PROTOCOL_H")
    out.append("#define X12_PROTOCOL_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    proto = root["protocol"]
    out.append(f"#define X12_IDL_FORMAT {proto['idl_format']}")
    out.append(f"#define X12_CORE_VERSION {proto['core_version']}")
    out.append("")
    out.append("// wire header, size counts the whole message including these 12 bytes")
    out.append("struct x12_msg_header {")
    out.append("    uint32_t size;")
    out.append("    uint32_t object;")
    out.append("    uint16_t opcode;")
    out.append("    uint16_t reserved;")
    out.append("};")
    out.append("_Static_assert(sizeof(struct x12_msg_header) == 12, \"wire header must be 12 bytes\");")
    out.append("")
    out.append("// sign, 24 integer bits, 8 fraction bits")
    out.append("typedef int32_t x12_fixed24p8;")
    out.append("#define X12_FIXED24P8_ONE 256")
    out.append("")

    # root level enums are shared across interfaces
    for ename, symbols in (root.get("enums") or {}).items():
        out.append(f"enum {ename.lower()} {{")
        for value, sym in enumerate(symbols):
            out.append(f"    {ename.upper()}_{sym.upper().replace('-', '_')} = {value},")
        out.append("};")
    out.append("")

    for iface in root["interfaces"]:
        p = _macro_prefix(iface["name"])
        out.append(f"// {iface['name']} v{iface['version']}")
        out.append(f"#define {p}_VERSION {iface['version']}")
        out.append(f"#define {p}_NAME \"{iface['name']}\"")
        for msg in iface.get("requests", []) or []:
            out.append(f"#define {p}_REQ_{msg['name'].upper()} {msg['opcode']}")
        for msg in iface.get("events", []) or []:
            out.append(f"#define {p}_EVT_{msg['name'].upper()} {msg['opcode']}")
        for ename, symbols in (iface.get("enums") or {}).items():
            cename = _c_enum_name(iface["name"], ename).upper()
            out.append(f"enum {cename.lower()} {{")
            for value, sym in enumerate(symbols):
                out.append(f"    {cename}_{sym.upper().replace('-', '_')} = {value},")
            out.append("};")
        out.append("")

    out.append("#endif")
    out.append("")
    return "\n".join(out)


def _ada_ident(s):
    return s.replace("-", "_").upper()


def cap(s):
    # hello-world -> HelloWorld, used for ada identifiers
    return "".join(part.capitalize() for part in s.replace("-", "_").split("_"))


ARG_TYPE_IDS = {
    "uint32": "X12_ARG_UINT32", "int32": "X12_ARG_INT32",
    "uint64": "X12_ARG_UINT64", "int64": "X12_ARG_INT64",
    "fixed24p8": "X12_ARG_FIXED24P8", "string": "X12_ARG_STRING",
    "fd": "X12_ARG_FD", "object": "X12_ARG_OBJECT", "new_id": "X12_ARG_NEW_ID",
    "array_u32": "X12_ARG_ARRAY_U32", "enum": "X12_ARG_ENUM",
}


def gen_tables_c(root):
    # codec knowledge as pure data: the c engine walks these tables, so a
    # protocol change never touches handwritten code
    out = []
    out.append("// Copyright (c) lezginskiy <xanother-x12@proton.me>")
    out.append("// SPDX-License-Identifier: MIT")
    out.append("//")
    out.append("// generated from spec/x12-core.yaml by x12gen, do not edit")
    out.append('#include "x12_wire.h"')
    out.append("")

    enums = collect_enums(root)
    enum_index = {name: i for i, (name, _) in enumerate(enums)}
    for idx, (name, symbols) in enumerate(enums):
        out.append("static const char *const "
                   f"x12_enum_syms_{idx}[] = {{ {', '.join(f'\"{s}\"' for s in symbols)} }};")
    out.append("")
    out.append("const x12_enum_desc x12_enums[] = {")
    for idx, (name, symbols) in enumerate(enums):
        out.append(f'    {{ "{name}", {len(symbols)}, x12_enum_syms_{idx} }},')
    out.append("};")
    out.append(f"const size_t x12_enums_count = {len(enums)};")
    out.append("")

    for iface in root["interfaces"]:
        iname = iface["name"]
        for kind_key in ("requests", "events"):
            for msg in iface.get(kind_key, []) or []:
                args = msg.get("args") or []
                if not args:
                    continue
                out.append(f"static const x12_arg_desc args_{iname}_{msg['name']}[] = {{")
                for arg in args:
                    eid = 255
                    if arg["type"] == "enum" or \
                            (arg["type"] == "array_u32" and arg.get("of") != "uint32"):
                        eid = enum_index.get(arg.get("of"), 255)
                    out.append(f'    {{ "{arg["name"]}", {ARG_TYPE_IDS[arg["type"]]}, {eid} }},')
                out.append("};")
        out.append(f"static const x12_msg_desc msgs_{iname}[] = {{")
        for kind_key, kind_const in (("requests", "X12_REQUEST"), ("events", "X12_EVENT")):
            for msg in iface.get(kind_key, []) or []:
                args = msg.get("args") or []
                ref = f"args_{iname}_{msg['name']}" if args else "0"
                out.append(f'    {{ "{msg["name"]}", {msg["opcode"]}, {kind_const}, '
                           f'{len(args)}, {ref} }},')
        out.append("};")
        out.append("")

    out.append("const x12_iface_desc x12_interfaces[] = {")
    for iface in root["interfaces"]:
        total = len(iface.get("requests") or []) + len(iface.get("events") or [])
        out.append(f'    {{ "{iface["name"]}", {iface["version"]}, {total}, '
                   f'msgs_{iface["name"]} }},')
    out.append("};")
    out.append(f"const size_t x12_interfaces_count = {len(root['interfaces'])};")
    out.append("")
    return "\n".join(out)


def _ada_enum_parts(iname, ename):
    # x12_ prefixed enums are global, others are scoped to the interface
    if ename.startswith("x12_"):
        return cap(ename[len("x12_"):]), ""
    base = cap(iname[len("x12_"):] + "_" + ename)
    return base, base + "_"


def _resolve_enum_symbols(root, iface, enum_name):
    # interface level enums shadow root level ones, none shadow each other today
    symbols = (iface.get("enums") or {}).get(enum_name)
    if symbols is None:
        symbols = (root.get("enums") or {}).get(enum_name)
    return symbols


def gen_ada(root):
    out = []
    out.append("-- Copyright (c) lezginskiy <xanother-x12@proton.me>")
    out.append("-- SPDX-License-Identifier: MIT")
    out.append("--")
    out.append("-- generated from spec/x12-core.yaml by x12gen, do not edit")
    out.append("package X12_Protocol is")
    out.append("")
    out.append("   pragma Pure;")
    out.append("")
    proto = root["protocol"]
    out.append(f"   Idl_Format : constant := {proto['idl_format']};")
    out.append(f"   Core_Version : constant := {proto['core_version']};")
    out.append("")

    for ename, symbols in (root.get("enums") or {}).items():
        ecap, prefix = _ada_enum_parts(None, ename)
        out.append(f"   type {ecap} is")
        literals = [prefix + cap(s) for s in symbols]
        out.append("      (" + ",\n       ".join(literals) + ");")
        pairs = ", ".join(f"{lit} => {v}" for v, lit in enumerate(literals))
        out.append(f"   for {ecap} use ({pairs});")
    out.append("")

    for iface in root["interfaces"]:
        short = iface["name"][len("x12_"):]
        iface_cap = "".join(part.capitalize() for part in short.split("_"))
        out.append(f"   -- {iface['name']} v{iface['version']}")
        out.append(f"   {iface_cap}_Version : constant := {iface['version']};")
        out.append(f"   {iface_cap}_Name : constant String := \"{iface['name']}\";")
        for msg in iface.get("requests", []) or []:
            out.append(f"   {iface_cap}_Req_{cap(msg['name'])}_Opcode : constant := {msg['opcode']};")
        for msg in iface.get("events", []) or []:
            out.append(f"   {iface_cap}_Evt_{cap(msg['name'])}_Opcode : constant := {msg['opcode']};")
        for ename, symbols in (iface.get("enums") or {}).items():
            ecap, prefix = _ada_enum_parts(iface["name"], ename)
            out.append(f"   type {ecap} is")
            literals = [prefix + cap(s) for s in symbols]
            out.append("      (" + ",\n       ".join(literals) + ");")
            pairs = ", ".join(f"{lit} => {v}" for v, lit in enumerate(literals))
            out.append(f"   for {ecap} use ({pairs});")
        out.append("")

    out.append("end X12_Protocol;")
    out.append("")
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description="x12 idl codegen")
    ap.add_argument("--idl", default="spec/x12-core.yaml")
    ap.add_argument("--out-dir", default="build/gen")
    ap.add_argument("--lang", choices=["c", "ada", "check"], default="check")
    args = ap.parse_args(argv)

    root = load_idl(args.idl)
    if args.lang == "check":
        print(f"ok: {len(root['interfaces'])} interfaces valid")
        return 0

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    if args.lang in ("c", "check"):
        (out_dir / "x12_protocol.h").write_text(gen_c(root), encoding="utf-8")
        (out_dir / "x12_tables.c").write_text(gen_tables_c(root), encoding="utf-8")
    if args.lang in ("ada", "check"):
        (out_dir / "x12_protocol.ads").write_text(gen_ada(root), encoding="utf-8")
    print(f"generated {args.lang} into {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
