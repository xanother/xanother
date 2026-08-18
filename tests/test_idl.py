# Copyright (c) lezginskiy <xanother-x12@proton.me>
# SPDX-License-Identifier: MIT

import unittest
from pathlib import Path

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from tools.x12gen import load_idl, gen_c, gen_ada  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
IDL_PATH = ROOT / "spec" / "x12-core.yaml"


class IdlValidation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.idl = load_idl(IDL_PATH)

    def test_every_core_interface_present(self):
        names = [i["name"] for i in self.idl["interfaces"]]
        for expected in ("x12_connection", "x12_registry", "x12_surface",
                         "x12_buffer", "x12_buffer_manager", "x12_seat",
                         "x12_output", "x12_data_device", "x12_screencopy"):
            self.assertIn(expected, names)

    def test_opcodes_unique_inside_interface(self):
        # validation in load_idl already enforces this, here it is asserted
        # against the parsed form so a silent validator change fails loudly
        for iface in self.idl["interfaces"]:
            for key in ("requests", "events"):
                ops = [m["opcode"] for m in iface.get(key) or []]
                self.assertEqual(len(ops), len(set(ops)), iface["name"])

    def test_new_ids_reference_declared_interfaces(self):
        names = {i["name"] for i in self.idl["interfaces"]}
        for iface in self.idl["interfaces"]:
            for key in ("requests", "events"):
                for msg in iface.get(key) or []:
                    for arg in msg.get("args") or []:
                        ref = arg.get("interface")
                        if ref:
                            self.assertIn(ref, names,
                                          f"{iface['name']}.{msg['name']}.{arg['name']}")

    def test_capability_enum_is_root_level(self):
        # capabilities gate registry.bind across interfaces, so the enum
        # lives at the root, not inside one interface
        self.assertIn("x12_capability", self.idl.get("enums") or {})

    def test_generated_c_header_has_no_interface_name_collision(self):
        header = gen_c(self.idl)
        defines = [line.split()[1] for line in header.splitlines()
                   if line.startswith("#define")]
        self.assertEqual(len(defines), len(set(defines)))

    def test_generated_ada_enum_literals_unique(self):
        # ada literals share one namespace inside the package
        text = gen_ada(self.idl)
        literals = []
        in_type = False
        for line in text.splitlines():
            stripped = line.strip()
            if stripped.startswith("type "):
                in_type = True
                continue
            if in_type:
                if stripped.startswith("      (") or stripped.startswith("       "):
                    literals.extend(
                        lit.split(" =>")[0].strip(" (),")
                        for lit in stripped.strip(" ();").split(",") if lit.strip())
                else:
                    in_type = False
        self.assertEqual(len(literals), len(set(literals)),
                         "duplicate ada literals: " + str(sorted(literals)))


if __name__ == "__main__":
    unittest.main()
