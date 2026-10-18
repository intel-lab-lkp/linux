#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
# Prefix all lines with "# ", unbuffered.

import sys


def main():
    needed = True
    while True:
        char = sys.stdin.buffer.read(1)
        if not char:
            return 0
        if needed:
            sys.stdout.buffer.write(b"# ")
            needed = False
        sys.stdout.buffer.write(char)
        sys.stdout.buffer.flush()
        if char == b"\n":
            needed = True


if __name__ == "__main__":
    sys.exit(main())
