#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Command-line regression tests for gen_compile_commands.py."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("gen_compile_commands.py")


class TestCompileCommands(unittest.TestCase):
    def test_make_escaping(self):
        cases = [
            ("plain", "plain"),
            ("$$value", "$value"),
            ("$$$$", "$$"),
            ("$(pound)value", "#value"),
            ("$$(pound)", "$(pound)"),
            ("$$$(pound)", "$#"),
        ]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "test.c").write_text("int test;\n", encoding="utf-8")
            for escaped, original in cases:
                with self.subTest(escaped=escaped):
                    prefix = "gcc -DVALUE='\"{}\"' -c -o test.o "
                    (root / ".test.o.cmd").write_text(
                        "savedcmd_test.o := " + prefix.format(escaped)
                        + "test.c\n", encoding="utf-8")
                    output = root / "compile_commands.json"
                    subprocess.run(
                        [sys.executable, str(SCRIPT), "-d", str(root),
                         "-o", str(output)], check=True, capture_output=True)
                    entries = json.loads(output.read_text(encoding="utf-8"))
                    self.assertEqual(len(entries), 1)
                    self.assertEqual(entries[0]["command"],
                                     prefix.format(original) + "test.c")


if __name__ == "__main__":
    unittest.main()
