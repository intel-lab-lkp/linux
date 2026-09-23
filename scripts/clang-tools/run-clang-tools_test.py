#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

"""Tests for run-clang-tools.py."""

import importlib.util
import os
from pathlib import Path
import unittest
from unittest import mock


spec = importlib.util.spec_from_file_location(
    "run_clang_tools", Path(__file__).with_name("run-clang-tools.py"))
run_clang_tools = importlib.util.module_from_spec(spec)
spec.loader.exec_module(run_clang_tools)


class TestRunClangTools(unittest.TestCase):
    def test_broken_pipe_redirects_stdout_and_exits(self):
        args = mock.Mock(path="compile_commands.json")
        with mock.patch.object(run_clang_tools, "parse_arguments", return_value=args), \
             mock.patch("builtins.open", mock.mock_open(read_data="[]")), \
             mock.patch.object(run_clang_tools.multiprocessing, "Lock"), \
             mock.patch.object(run_clang_tools.multiprocessing, "Pool") as pool, \
             mock.patch("os.open", return_value=42) as open_devnull, \
             mock.patch("os.dup2") as dup2, \
             mock.patch.object(run_clang_tools.sys, "stdout") as stdout:
            pool.return_value.__enter__.return_value.map.side_effect = BrokenPipeError
            stdout.fileno.return_value = 1

            with self.assertRaises(SystemExit) as raised:
                run_clang_tools.main()

            self.assertEqual(raised.exception.code, 1)
            open_devnull.assert_called_once_with(os.devnull, os.O_WRONLY)
            dup2.assert_called_once_with(42, 1)


if __name__ == "__main__":
    unittest.main()
