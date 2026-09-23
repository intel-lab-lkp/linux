#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Exercise compilation database paths with the real clang-tidy executable."""

import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("run-clang-tools.py")


@unittest.skipUnless(shutil.which("clang-tidy"), "clang-tidy is required")
class TestDatabasePath(unittest.TestCase):
    def test_database_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            build = root / "build"
            database = root / "database with spaces"
            build.mkdir()
            database.mkdir()
            entries = []
            for suffix in ("c", "cpp"):
                source = build / ("test." + suffix)
                source.write_text(
                    "#ifndef REQUIRED\n#error compilation flags lost\n"
                    "#endif\nint test;\n", encoding="utf-8")
                entries.append({
                    "directory": str(build), "file": str(source),
                    "arguments": ["clang", "-DREQUIRED", "-c", str(source)],
                })
            path = database / "compile_commands.json"
            path.write_text(json.dumps(entries), encoding="utf-8")
            paths = [str(path), str(path.relative_to(root)),
                     "./" + str(path.relative_to(root)),
                     "build/../" + str(path.relative_to(root))]
            for analysis in ("clang-tidy", "clang-analyzer"):
                for value in paths:
                    with self.subTest(analysis=analysis, path=value):
                        result = subprocess.run(
                            [sys.executable, str(SCRIPT.resolve()), analysis,
                             value], cwd=root, capture_output=True, text=True)
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertNotIn("compilation flags lost", result.stderr)
                        self.assertNotIn("Running without flags", result.stderr)
                        self.assertNotIn("Error while trying to load",
                                         result.stderr)


if __name__ == "__main__":
    unittest.main()
