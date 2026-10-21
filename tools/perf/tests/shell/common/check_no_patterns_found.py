#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import os
import re
import sys


def main(argv):
    sys.stdin.reconfigure(encoding="utf-8", errors="surrogateescape")
    sys.stdout.reconfigure(encoding="utf-8", errors="surrogateescape")
    quiet = True
    try:
        if int(os.environ.get("TESTLOG_VERBOSITY", "0")) >= 2:
            quiet = False
    except ValueError:
        pass

    found = set()
    for line in sys.stdin:
        line = line.rstrip("\n")
        for r in argv:
            if re.search(r, line):
                found.add(r)

    passed = True
    for r in argv:
        if r in found:
            if not quiet:
                print(f'Regexp found: "{r}"')
            passed = False

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
