#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
flamegraph.py - create flame graphs from perf samples using perf python module
"""

import argparse
import hashlib
import json
import os
import subprocess
import sys
import urllib.request
from typing import Dict, Optional, Union
import perf

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

class Node:
    """A node in the flame graph tree."""
    def __init__(self, name: str, libtype: str):
        self.name = name
        self.libtype = libtype
        self.value: int = 0
        self.children: dict[str, Node] = {}

    def to_json(self) -> Dict[str, Union[str, int, list[Dict]]]:
        """Convert the node to a JSON-serializable dictionary."""
        return {
            "n": self.name,
            "l": self.libtype,
            "v": self.value,
            "c": [x.to_json() for x in self.children.values()]
        }


class FlameGraphCLI:
    """Command-line interface for generating flame graphs."""
    def __init__(self, args):
        self.args = args
        self.stack = Node("all", "root")
        self.session = None

    @staticmethod
    def get_libtype_from_dso(dso: Optional[str]) -> str:
        """Determine the library type from the DSO name."""
        if dso and (dso == "[kernel.kallsyms]" or dso.endswith("/vmlinux") or dso == "[kernel]"):
            return "kernel"
        return ""

    @staticmethod
    def find_or_create_node(node: Node, name: str, libtype: str) -> Node:
        """Find a child node with the given name or create a new one."""
        if name in node.children:
            return node.children[name]
        child = Node(name, libtype)
        node.children[name] = child
        return child

    def process_event(self, sample) -> None:
        """Process a single perf sample event."""
        if self.args.event_name and str(sample.evsel) != self.args.event_name:
            return

        pid = sample.sample_pid
        dso_type = ""
        try:
            thread = self.session.process(sample.sample_tid)
            comm = thread.comm()
        except Exception:
            comm = "[unknown]"

        if pid == 0:
            comm = "swapper"
            dso_type = "kernel"
        else:
            comm = f"{comm} ({pid})"

        node = self.find_or_create_node(self.stack, comm, dso_type)

        callchain = sample.callchain
        if callchain:
            # We want to traverse from root to leaf.
            # perf callchain iterator gives leaf to root.
            # We collect them and reverse.
            frames = list(callchain)
            for entry in reversed(frames):
                name = entry.symbol or "[unknown]"
                libtype = self.get_libtype_from_dso(entry.dso)
                node = self.find_or_create_node(node, name, libtype)
        else:
            # Fallback if no callchain
            name = getattr(sample, "symbol", "[unknown]")
            libtype = self.get_libtype_from_dso(getattr(sample, "dso", "[unknown]"))
            node = self.find_or_create_node(node, name, libtype)

        node.value += 1

    def get_report_header(self) -> str:
        """Get the header from the perf report."""
        try:
            input_file = self.args.input or "perf.data"
            output = subprocess.check_output(["perf", "report", "--header-only", "-i", input_file])
            result = output.decode("utf-8")
            if self.args.event_name:
                result += "\nFocused event: " + self.args.event_name
            return result
        except Exception:
            return ""

    def run(self) -> None:
        """Run the flame graph generation."""
        input_file = self.args.input or "perf.data"
        if not os.path.exists(input_file):
            print(f"Error: {input_file} not found. (try 'perf record' first)", file=sys.stderr)
            sys.exit(1)

        try:
            self.session = perf.session(perf.data(input_file),
                                        sample=self.process_event)
        except Exception as e:
            print(f"Error opening session: {e}", file=sys.stderr)
            sys.exit(1)

        self.session.process_events()

        stacks_json = json.dumps(self.stack, default=lambda x: x.to_json())
        # Escape HTML special characters to prevent XSS
        stacks_json = stacks_json.replace("<", "\\u003c") \
            .replace(">", "\\u003e").replace("&", "\\u0026")

        if self.args.format == "html":
            report_header = self.get_report_header()
            options = {
                "colorscheme": self.args.colorscheme,
                "context": report_header
            }
            options_json = json.dumps(options)
            options_json = options_json.replace("<", "\\u003c") \
                .replace(">", "\\u003e").replace("&", "\\u0026")

            template = self.args.template
            template_md5sum = None
            output_str = None

            if not os.path.isfile(template):
                if template.startswith("http://") or template.startswith("https://"):
                    if not self.args.allow_download:
                        print("Warning: Downloading templates is disabled. "
                              "Use --allow-download.", file=sys.stderr)
                        template = None
                else:
                    print(f"Warning: Template file '{template}' not found.", file=sys.stderr)
                    if self.args.allow_download:
                        print("Using default CDN template.", file=sys.stderr)
                        template = (
                            "https://cdn.jsdelivr.net/npm/d3-flame-graph@4.1.3/dist/templates/"
                            "d3-flamegraph-base.html"
                        )
                        template_md5sum = "143e0d06ba69b8370b9848dcd6ae3f36"
                    else:
                        template = None

            use_minimal = False
            try:
                if not template:
                    use_minimal = True
                elif template.startswith("http"):
                    with urllib.request.urlopen(template) as url_template:
                        output_str = "".join([l.decode("utf-8") for l in url_template.readlines()])
                else:
                    with open(template, "r", encoding="utf-8") as f:
                        output_str = f.read()
            except Exception as err:
                print(f"Error reading template {template}: {err}\n", file=sys.stderr)
                use_minimal = True

            if use_minimal:
                print("Using internal minimal HTML that refers to d3's web site. JavaScript " +
                      "loaded this way from a local file may be blocked unless your " +
                      "browser has relaxed permissions. Run with '--allow-download' to fetch" +
                      "the full D3 HTML template.", file=sys.stderr)
                output_str = MINIMAL_HTML

            elif template_md5sum:
                assert output_str is not None
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

            assert output_str is not None
            output_str = output_str.replace("/** @options_json **/", options_json)
            output_str = output_str.replace("/** @flamegraph_json **/", stacks_json)
            output_fn = self.args.output or "flamegraph.html"
        else:
            output_str = stacks_json
            output_fn = self.args.output or "stacks.json"

        if output_fn == "-":
            sys.stdout.write(output_str)
        else:
            print(f"dumping data to {output_fn}")
            with open(output_fn, "w", encoding="utf-8") as out:
                out.write(output_str)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Create flame graphs using perf python module.")
    parser.add_argument("-f", "--format", default="html", choices=["json", "html"],
                        help="output file format")
    parser.add_argument("-o", "--output", help="output file name")
    parser.add_argument("--template",
                        default="/usr/share/d3-flame-graph/d3-flamegraph-base.html",
                        help="path to flame graph HTML template")
    parser.add_argument("--colorscheme", default="blue-green",
                        help="flame graph color scheme", choices=["blue-green", "orange"])
    parser.add_argument("-i", "--input", help="input perf.data file")
    parser.add_argument("--allow-download", default=False, action="store_true",
                        help="allow unprompted downloading of HTML template")
    parser.add_argument("-e", "--event", default="", dest="event_name", type=str,
                        help="specify the event to generate flamegraph for")

    cli_args = parser.parse_args()
    cli = FlameGraphCLI(cli_args)
    cli.run()
