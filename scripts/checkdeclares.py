#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# checkdeclares: find struct declared more than once
#
# Copyright 2021 Wan Jiabing <wanjiabing@vivo.com>
# Inspired by checkincludes.pl
#
# This script checks for duplicate struct declares.
# Note that this will not take into consideration macros so
# you should run this only if you know you do have real dups
# and do not have them under #ifdef's.

import re
import sys

RE_STRUCT_DECL = re.compile(r'^\s*struct\s*(\w*);$')


def usage():
    print('Usage: checkdeclares.py file1.h ...', file=sys.stderr)
    print('Warns of struct declaration duplicates', file=sys.stderr)
    sys.exit(1)


def process_file(path):
    """Return (dup_count, error_msg)."""
    try:
        with open(path, 'r') as f:
            lines = f.readlines()
    except OSError as e:
        return 0, f"Cannot open {path}: {e}."

    declared = {}
    for line in lines:
        m = RE_STRUCT_DECL.match(line)
        if m:
            declared[m.group(1)] = declared.get(m.group(1), 0) + 1

    dups = 0
    for name, n in declared.items():
        if n > 1:
            print(f"{path}: struct {name} is declared more than once.")
            dups += 1
    return dups, None


def main(argv):
    if not argv:
        usage()
    dup_counter = 0
    for path in argv:
        n, err = process_file(path)
        if err:
            print(err, file=sys.stderr)
            sys.exit(1)
        dup_counter += n
    if dup_counter == 0:
        print('No duplicate struct declares found.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))