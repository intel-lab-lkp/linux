#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Takes a (sorted) output of readprofile and turns it into a list suitable
# for linker scripts.
#
# usage:
#     readprofile | sort -rn | python3 profile2linkerlist.py > functionlist

import re
import sys

RE_PROFILE = re.compile(r'\W*[0-9]+\W*([a-zA-Z_0-9]+)\W*[0-9]+')


def main():
    for line in sys.stdin:
        if 'unknown' in line or 'total' in line:
            continue
        m = RE_PROFILE.search(line)
        if m:
            print(f'*(.text.{m.group(1)})')
    return 0


if __name__ == '__main__':
    sys.exit(main())