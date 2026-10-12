# SPDX-License-Identifier: GPL-2.0-only OR MIT
# Copyright (C) 2025 TNG Technology Consulting GmbH

import os
import re
from dataclasses import dataclass, field
from sbom.cmd_graph.deps_parser import parse_cmd_file_deps
from sbom.cmd_graph.dependency_filter import should_include_dependency
import sbom.sbom_logging as sbom_logging
from sbom.path_utils import PathStr

SAVEDCMD_PATTERN = re.compile(r"^(saved)?cmd_.*?:=\s*(?P<full_command>.+)$")
MAKE_PREREQS_PATTERN = re.compile(r"^make_prereqs_.*?:=\s*(?P<prereqs>.+)$")
DEPS_ASSIGNMENT_PATTERN = re.compile(r"^deps_.*?:=")


@dataclass
class CmdFile:
    cmd_file_path: PathStr
    savedcmd: str
    make_prereqs: list[str] = field(default_factory=list)
    deps: list[str] = field(default_factory=list)

    @classmethod
    def create(cls, cmd_file_path: PathStr) -> "CmdFile | None":
        r"""
        Parse the following fields from a `.cmd` file:

        * `(saved)?cmd_<target> := <shell command>` (required) —
          the shell command Kbuild ran to produce the build target.
        * `make_prereqs_<target> := <prerequisites>` (required but can be empty) —
          the non-phony make prerequisites.
        * `deps_<target> := \` (optional) —
          additional dependencies not covered by make prerequisites, e.g., headers.

        Args:
            cmd_file_path (Path): absolute Path to a `.cmd` file.

        Returns:
            cmd_file (CmdFile): Parsed `.cmd` file or `None` if no `savedcmd` line was found.
        """
        with open(cmd_file_path, "rt", encoding="utf-8") as f:
            lines = [line.strip() for line in f.readlines() if line.strip() != "" and not line.startswith("#")]

        savedcmd: str | None = None
        make_prereqs: list[str] = []
        deps: list[str] = []

        for i, line in enumerate(lines):
            match = SAVEDCMD_PATTERN.match(line)
            if match is not None:
                savedcmd = match.group("full_command")
                continue

            match = MAKE_PREREQS_PATTERN.match(line)
            if match is not None:
                make_prereqs = match.group("prereqs").split()
                continue

            if DEPS_ASSIGNMENT_PATTERN.match(line):
                j = i + 1
                while j < len(lines) and lines[j].endswith("\\"):
                    deps.append(lines[j].removesuffix("\\").strip())
                    j += 1

        if savedcmd is None:
            sbom_logging.error(
                "Skip parsing '{cmd_file_path}' because no 'savedcmd_' command was found.", cmd_file_path=cmd_file_path
            )
            return None

        return CmdFile(cmd_file_path, savedcmd, make_prereqs, deps)

    def get_dependencies(self: "CmdFile", target_path: PathStr, obj_tree: PathStr) -> list[PathStr]:
        """
        Parses all dependencies required to build a target file from its cmd file.

        Args:
            target_path: path to the target file relative to `obj_tree`.
            obj_tree: absolute path to the object tree.

        Returns:
            list[PathStr]: dependency file paths relative to `obj_tree`.
        """
        input_files: list[PathStr] = list(self.make_prereqs)

        if self.deps:
            input_files += [str(p) for p in parse_cmd_file_deps(self.deps)]

        cmd_file_dependencies: list[PathStr] = []
        for input_file in input_files:
            # input files are either absolute or already relative to the object tree
            if os.path.isabs(input_file):
                input_file = os.path.relpath(input_file, obj_tree)
            if should_include_dependency(input_file):
                cmd_file_dependencies.append(input_file)
        unique_cmd_file_dependencies = list(dict.fromkeys(cmd_file_dependencies))
        return unique_cmd_file_dependencies
