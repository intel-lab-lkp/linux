#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# checkincludes: find/remove files included more than once
#
# Copyright abandoned, 2000, Niels Kristian Bech Jensen <nkbj@image.dk>.
# Copyright 2009 Luis R. Rodriguez <mcgrof@gmail.com>
#
# Checks for duplicate #include directives. With -r, removes duplicates
# in place. Does not account for macros or #ifdef branches; run only when
# you know the duplicates are real.

import re
import sys

RE_INCLUDE = re.compile(r'^\s*#\s*include\s*[<"](\S*)[>"]')


def usage():
    print("Usage: checkincludes.py [-r]", file=sys.stderr)
    print("By default we just warn of duplicates", file=sys.stderr)
    print("To remove duplicated includes in place use -r", file=sys.stderr)
    sys.exit(1)


def process_file(path, remove):
    """Return (dup_count, error_msg). error_msg is None on success."""
    try:
        with open(path, 'r') as f:
            lines = f.readlines()
    except OSError as e:
        return 0, f"Cannot open {path}: {e}"

    included = {}
    for line in lines:
        m = RE_INCLUDE.match(line)
        if m:
            included[m.group(1)] = included.get(m.group(1), 0) + 1

    if not remove:
        dups = 0
        for filename, n in included.items():
            if n > 1:
                print(f"{path}: {filename} is included more than once.")
                dups += 1
        return dups, None

    # Removal mode: keep the last occurrence, remove earlier duplicates.
    # Same semantics as the Perl original (first N-1 occurrences removed).
    dups = 0
    out_lines = []
    for line in lines:
        m = RE_INCLUDE.match(line)
        if m and included.get(m.group(1), 0) > 1:
            included[m.group(1)] -= 1
            dups += 1
        else:
            out_lines.append(line)

    # Match original: always rewrite, even when nothing was removed.
    try:
        with open(path, 'w') as f:
            f.writelines(out_lines)
    except OSError as e:
        return 0, f"Cannot write to {path}: {e}"

    if dups > 0:
        print(f"{path}: removed {dups} duplicate includes")
    return dups, None


def main(argv):
    remove = False
    args = list(argv)

    if not args:
        usage()

    # Original only parses "-r" when there are >= 2 args.
    if len(args) >= 2 and args[0].startswith('-'):
        if args[0] == '-r':
            remove = True
            args.pop(0)
        else:
            usage()

    dup_counter = 0
    for path in args:
        n, err = process_file(path, remove)
        if err:
            print(err, file=sys.stderr)
            sys.exit(1)
        dup_counter += n

    if dup_counter == 0:
        print("No duplicate includes found.")
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))