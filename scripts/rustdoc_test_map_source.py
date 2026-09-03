# SPDX-License-Identifier: GPL-2.0

import sys
import re
import os
import subprocess

LOCATION_REGEX = re.compile(r"\.location: (.*):(\d+)")
ANCHOR_REGEX = re.compile(r"static __DOCTEST_ANCHOR: .+ = .* \+ (\d+) \+ (\d+);")
SOURCE_INFO_REGEX = re.compile(r"(rust/[\w/.-]+\.rs):(\d+):(\d+)")


def transform(file, line):
    with open(file, "r") as f:
        lines = f.readlines()

    line_idx = line - 1

    # Find the last location and anchor before the erroring line
    real_path = None
    orig_line = None
    anchor_line = None

    for i in range(line_idx, -1, -1):
        if real_path is None:
            match = LOCATION_REGEX.search(lines[i])
            if match:
                real_path = match.group(1)
                orig_line = int(match.group(2))

        if anchor_line is None:
            match = ANCHOR_REGEX.search(lines[i])
            if match:
                anchor_line = i + int(match.group(1)) + int(match.group(2))

        if real_path is not None and anchor_line is not None:
            break

    new_line = orig_line + (line_idx - anchor_line)
    return real_path, new_line


def main():
    actual_rustc = os.environ.get("ACTUAL_RUSTC")
    args = sys.argv[1:]

    # We redirected Rust output so it is not TTY anymore.
    # Add `--color=always` back to preserve the color behaviour.
    if sys.stderr.isatty() and not any(arg.startswith("--color") for arg in args):
        args.append("--color=always")

    result = subprocess.run([actual_rustc] + args, stderr=subprocess.PIPE, text=True)

    def replacer(match):
        orig = match.group(0)
        file = match.group(1)
        line = int(match.group(2))
        col = int(match.group(3))

        if file != "rust/doctests_kernel_generated.rs":
            return orig

        new_file, new_line = transform(file, line)
        return f"{orig} (generated from {new_file}:{new_line}:{col})"

    if result.stderr:
        sys.stderr.write(SOURCE_INFO_REGEX.sub(replacer, result.stderr))

    sys.exit(result.returncode)


if __name__ == "__main__":
    main()
