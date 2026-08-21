# flamegraph.py - create flame graphs from perf samples
# SPDX-License-Identifier: GPL-2.0
#
# Usage:
#
#     perf record -a -g -F 99 sleep 60
#     perf script report flamegraph
#
# Combined:
#
#     perf script flamegraph -a -F 99 sleep 60
#
# Written by Andreas Gerstmayr <agerstmayr@redhat.com>
# Flame Graphs invented by Brendan Gregg <bgregg@netflix.com>
# Works in tandem with d3-flame-graph by Martin Spier <mspier@netflix.com>
#
# pylint: disable=missing-module-docstring
# pylint: disable=missing-class-docstring
# pylint: disable=missing-function-docstring

import argparse
from dataclasses import dataclass
import hashlib
import io
import json
import os
import subprocess
import sys
import re
from typing import Dict, Optional, Union
import urllib.request
from perf_trace_context import perf_config_get


def default_objdump():
    config = perf_config_get("annotate.objdump")
    return config if config else "objdump"


MINIMAL_HTML = """<head>
  <link rel="stylesheet" type="text/css" href="https://cdn.jsdelivr.net/npm/d3-flame-graph@4.1.3/dist/d3-flamegraph.css">
</head>
<body>
  <div id="chart"></div>
  <script type="text/javascript" src="https://d3js.org/d3.v7.js"></script>
  <script type="text/javascript" src="https://cdn.jsdelivr.net/npm/d3-flame-graph@4.1.3/dist/d3-flamegraph.min.js"></script>
  <script type="text/javascript">
  const stacks = [/** @flamegraph_json **/];
  // Note, options is unused.
  const options = [/** @options_json **/];

  var chart = flamegraph();
  d3.select("#chart")
        .datum(stacks[0])
        .call(chart);
  </script>
</body>
"""

# pylint: disable=too-few-public-methods
class Node:
    def __init__(self, name: str, libtype: str):
        self.name = name
        # "root" | "kernel" | ""
        # "" indicates user space
        self.libtype = libtype
        self.value: int = 0
        self.children: list[Node] = []

    def to_json(self) -> Dict[str, Union[str, int, list[Dict]]]:
        return {
            "n": self.name,
            "l": self.libtype,
            "v": self.value,
            "c": [x.to_json() for x in self.children]
        }


@dataclass
class Symbol:
    start: int
    size: int


class Instructions:
    def __init__(self):
        self.symbols:      dict[str, dict[str, Symbol]] = {}
        self.instructions: dict[tuple[str, str], dict[int, str]] = {}
        self.objdump_line: re.Pattern = re.compile(r"^\s+([0-9a-f]+):\s+(.*)")
        self.symbol_line: re.Pattern = re.compile(
            r"^([0-9a-f]+) (.{7})\s+\S+\s+([0-9a-f]+)\s+(.*)$")
        self.objdump:      str = default_objdump()

    def load_symbols(self, dso: str) -> dict[str, Symbol]:
        result = subprocess.run(
            [self.objdump, "--demangle", "-t", dso],
            capture_output=True, text=True, check=False
        )

        if result.returncode != 0:
            return {}

        symbols: dict[str, Symbol] = {}

        for line in result.stdout.splitlines():
            match = self.symbol_line.match(line)
            if not match:
                continue

            value, flags, size_str, name = match.groups()
            # the 7th flag column holds the symbol type, "F" for a function
            if flags[6] != "F":
                continue

            size = int(size_str, 16)
            if size == 0:
                continue

            # a name may be preceded by its visibility
            parts = name.split(maxsplit=1)
            if len(parts) == 2 and parts[0] in (".hidden", ".protected",
                                                ".internal"):
                name = parts[1]

            symbols[name.strip()] = Symbol(start=int(value, 16), size=size)

        return symbols

    def load_instructions(self, dso: str, sym: Symbol) -> dict[int, str]:
        result = subprocess.run(
            [
                self.objdump, "-d",
                "--no-show-raw-insn",
                f"--start-address=0x{sym.start:x}",
                f"--stop-address=0x{sym.start + sym.size:x}",
                dso,
            ],
            capture_output=True, text=True, check=False
        )

        instructions: dict[int, str] = {}

        for line in result.stdout.splitlines():
            match = self.objdump_line.match(line)
            if not match:
                continue
            addr = int(match.group(1), 16)
            insn = match.group(2).strip()
            instructions[addr] = insn

        return instructions

    def lookup_instruction(self, dso: str, func: str,
                           off: int) -> Optional[tuple[int, Optional[str]]]:
        if dso not in self.symbols:
            self.symbols[dso] = self.load_symbols(dso)

        sym = self.symbols[dso].get(func)
        if sym is None:
            return None

        if (dso, func) not in self.instructions:
            self.instructions[(dso, func)] = self.load_instructions(dso, sym)

        addr = sym.start + off
        return (addr, self.instructions[(dso, func)].get(addr))


class FlameGraphCLI:
    def __init__(self, args):
        self.args = args
        self.stack = Node("all", "root")
        self.instructions = Instructions() if args.asm else None

    @staticmethod
    def get_libtype_from_dso(dso: Optional[str]) -> str:
        """
        when kernel-debuginfo is installed,
        dso points to /usr/lib/debug/lib/modules/*/vmlinux
        """
        if dso and (dso == "[kernel.kallsyms]" or dso.endswith("/vmlinux")):
            return "kernel"

        return ""

    @staticmethod
    def find_or_create_node(node: Node, name: str, libtype: str) -> Node:
        for child in node.children:
            if child.name == name:
                return child

        child = Node(name, libtype)
        node.children.append(child)
        return child

    def process_event(self, event) -> None:
        # ignore events where the event name does not match
        # the one specified by the user
        if self.args.event_name and event.get("ev_name") != self.args.event_name:
            return

        pid = event.get("sample", {}).get("pid", 0)
        # event["dso"] sometimes contains /usr/lib/debug/lib/modules/*/vmlinux
        # for user-space processes; let's use pid for kernel or user-space distinction
        if pid == 0:
            comm = event["comm"]
            libtype = "kernel"
        else:
            comm = f"{event['comm']} ({pid})"
            libtype = ""
        node = self.find_or_create_node(self.stack, comm, libtype)

        if "callchain" in event:
            for entry in reversed(event["callchain"]):
                name = entry.get("sym", {}).get("name", "[unknown]")
                libtype = self.get_libtype_from_dso(entry.get("dso"))
                node = self.find_or_create_node(node, name, libtype)
        else:
            name = event.get("symbol", "[unknown]")
            libtype = self.get_libtype_from_dso(event.get("dso"))
            node = self.find_or_create_node(node, name, libtype)

        if self.args.asm:
            # use the sample IP directly rather than callchain[0], since with
            # precise event recording (i.e. :pp) the top callchain entry may
            # point to the next instruction rather than the sampled IP
            sym_name = event.get("symbol")
            sym_off  = event.get("symoff")
            dso      = event.get("dso")

            if sym_name and sym_off is not None and dso:
                found = self.instructions.lookup_instruction(dso, sym_name, sym_off)
                if found is not None:
                    addr, insn = found
                    name = f"{insn} [0x{addr:x}]" if insn else f"[0x{addr:x}]"
                    libtype = self.get_libtype_from_dso(dso)
                    node = self.find_or_create_node(node, name, libtype)
        node.value += 1

    def get_report_header(self) -> str:
        if self.args.input == "-":
            # when this script is invoked with "perf script flamegraph",
            # no perf.data is created and we cannot read the header of it
            return ""

        try:
            # if the file name other than perf.data is given,
            # we read the header of that file
            if self.args.input:
                output = subprocess.check_output(["perf", "report", "--header-only",
                                                  "-i", self.args.input])
            else:
                output = subprocess.check_output(["perf", "report", "--header-only"])

            result = output.decode("utf-8")
            if self.args.event_name:
                result += "\nFocused event: " + self.args.event_name
            return result
        except Exception as err:  # pylint: disable=broad-except
            print(f"Error reading report header: {err}", file=sys.stderr)
            return ""

    def trace_end(self) -> None:
        stacks_json = json.dumps(self.stack, default=lambda x: x.to_json())

        if self.args.format == "html":
            report_header = self.get_report_header()
            options = {
                "colorscheme": self.args.colorscheme,
                "context": report_header
            }
            options_json = json.dumps(options)

            template_md5sum = None
            if self.args.format == "html":
                if os.path.isfile(self.args.template):
                    template = f"file://{self.args.template}"
                else:
                    if not self.args.allow_download:
                        print(f"""Warning: Flame Graph template '{self.args.template}'
does not exist. To avoid this please install a package such as the
js-d3-flame-graph or libjs-d3-flame-graph, specify an existing flame
graph template (--template PATH) or use another output format (--format
FORMAT).""",
                              file=sys.stderr)
                        if self.args.input == "-":
                            print(
"""Not attempting to download Flame Graph template as script command line
input is disabled due to using live mode. If you want to download the
template retry without live mode. For example, use 'perf record -a -g
-F 99 sleep 60' and 'perf script report flamegraph'. Alternatively,
download the template from:
https://cdn.jsdelivr.net/npm/d3-flame-graph@4.1.3/dist/templates/d3-flamegraph-base.html
and place it at:
/usr/share/d3-flame-graph/d3-flamegraph-base.html""",
                                  file=sys.stderr)
                            sys.exit(1)
                        s = None
                        while s not in ["y", "n"]:
                            s = input("Do you wish to download a template from cdn.jsdelivr.net?" +
                                      "(this warning can be suppressed with --allow-download) [yn] "
                                      ).lower()
                        if s == "n":
                            sys.exit(1)
                    template = ("https://cdn.jsdelivr.net/npm/d3-flame-graph@4.1.3/dist/templates/"
                                "d3-flamegraph-base.html")
                    template_md5sum = "143e0d06ba69b8370b9848dcd6ae3f36"

            try:
                with urllib.request.urlopen(template) as url_template:
                    output_str = "".join([
                        l.decode("utf-8") for l in url_template.readlines()
                    ])
            except Exception as err:
                print(f"Error reading template {template}: {err}\n"
                      "a minimal flame graph will be generated", file=sys.stderr)
                output_str = MINIMAL_HTML
                template_md5sum = None

            if template_md5sum:
                download_md5sum = hashlib.md5(output_str.encode("utf-8")).hexdigest()
                if download_md5sum != template_md5sum:
                    s = None
                    while s not in ["y", "n"]:
                        s = input(f"""Unexpected template md5sum.
{download_md5sum} != {template_md5sum}, for:
{output_str}
continue?[yn] """).lower()
                    if s == "n":
                        sys.exit(1)

            output_str = output_str.replace("/** @options_json **/", options_json)
            output_str = output_str.replace("/** @flamegraph_json **/", stacks_json)

            output_fn = self.args.output or "flamegraph.html"
        else:
            output_str = stacks_json
            output_fn = self.args.output or "stacks.json"

        if output_fn == "-":
            with io.open(sys.stdout.fileno(), "w", encoding="utf-8", closefd=False) as out:
                out.write(output_str)
        else:
            print(f"dumping data to {output_fn}")
            try:
                with io.open(output_fn, "w", encoding="utf-8") as out:
                    out.write(output_str)
            except IOError as err:
                print(f"Error writing output file: {err}", file=sys.stderr)
                sys.exit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Create flame graphs.")
    parser.add_argument("-f", "--format",
                        default="html", choices=["json", "html"],
                        help="output file format")
    parser.add_argument("-o", "--output",
                        help="output file name")
    parser.add_argument("--template",
                        default="/usr/share/d3-flame-graph/d3-flamegraph-base.html",
                        help="path to flame graph HTML template")
    parser.add_argument("--colorscheme",
                        default="blue-green",
                        help="flame graph color scheme",
                        choices=["blue-green", "orange"])
    parser.add_argument("-i", "--input",
                        help=argparse.SUPPRESS)
    parser.add_argument("--allow-download",
                        default=False,
                        action="store_true",
                        help="allow unprompted downloading of HTML template")
    parser.add_argument("-e", "--event",
                        default="",
                        dest="event_name",
                        type=str,
                        help="specify the event to generate flamegraph for")
    parser.add_argument("--asm",
                        default=False,
                        action="store_true",
                        help="annotate leaf frames with instruction-level nodes")

    cli_args = parser.parse_args()
    cli = FlameGraphCLI(cli_args)

    process_event = cli.process_event
    trace_end = cli.trace_end
