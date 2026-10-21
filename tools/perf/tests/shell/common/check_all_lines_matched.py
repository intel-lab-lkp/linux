#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0

import os
import re
import sys


def main(argv):
    sys.stdin.reconfigure(encoding="utf-8", errors="surrogateescape")
    sys.stdout.reconfigure(encoding="utf-8", errors="surrogateescape")
    max_printed = 20
    if "TESTLOG_ERR_MSG_MAX_LINES" in os.environ:
        try:
            max_printed = int(os.environ["TESTLOG_ERR_MSG_MAX_LINES"])
        except ValueError:
            pass

    quiet = True
    try:
        if int(os.environ.get("TESTLOG_VERBOSITY", "0")) >= 2:
            quiet = False
    except ValueError:
        pass

    passed = True
    lines_printed = 0

    for line in sys.stdin:
        line = line.rstrip("\n")
        matched = False
        for r in argv:
            if re.search(r, line):
                matched = True
                break
        if not matched:
            show = lines_printed < max_printed
            lines_printed += 1
            if show and not quiet:
                print(f'Line did not match any pattern: "{line}"')
            passed = False

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
