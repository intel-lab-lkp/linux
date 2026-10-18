#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# checkversion finds uses of macros in <linux/version.h> without the
# corresponding #include; or includes of <linux/version.h> without any
# use.
# Copyright (C) 2003, Randy Dunlap <rdunlap@infradead.org>

import re
import sys

SKIP_FILES = (
    'include/generated/uapi/linux/version.h',
    'usr/include/linux/version.h',
)

RE_VERSION_MACROS = re.compile(
    r'LINUX_VERSION_CODE|(?<!\w)KERNEL_VERSION|LINUX_VERSION_MAJOR'
    r'|LINUX_VERSION_PATCHLEVEL|LINUX_VERSION_SUBLEVEL')
RE_INCLUDE_Q = re.compile(r'^\s*#\s*include\s*"linux/version\.h"')
RE_INCLUDE_A = re.compile(r'^\s*#\s*include\s*<linux/version\.h>')


def strip_comments(line, in_comment):
    """Return (line_without_comments, in_comment)."""
    out = []
    i = 0
    while i < len(line):
        if in_comment:
            end = line.find('*/', i)
            if end == -1:
                return ''.join(out), True
            i = end + 2
            in_comment = False
        else:
            start = line.find('/*', i)
            if start == -1:
                out.append(line[i:])
                break
            out.append(line[i:start])
            i = start + 2
            in_comment = True
    return ''.join(out), in_comment


def strip_strings(line, in_string):
    """Return (line_without_strings, in_string)."""
    out = []
    i = 0
    while i < len(line):
        if in_string:
            end = line.find('"', i)
            if end == -1:
                return ''.join(out), True
            i = end + 1
            in_string = False
        else:
            start = line.find('"', i)
            if start == -1:
                out.append(line[i:])
                break
            out.append(line[i:start])
            i = start + 1
            in_string = True
    return ''.join(out), in_string


def check_file(path, debugging=False):
    try:
        with open(path, 'r') as f:
            lines = f.readlines()
    except OSError as e:
        print(f"Can't open {path}: {e}", file=sys.stderr)
        return

    in_comment = False
    in_string = False
    version_linenr = 0
    uses_version = False

    for idx, raw in enumerate(lines, 1):
        line, in_comment = strip_comments(raw, in_comment)
        if in_comment and not line.strip():
            continue

        if RE_INCLUDE_Q.search(line):
            version_linenr = idx

        line, in_string = strip_strings(line, in_string)
        if in_string and not line.strip():
            continue

        if RE_INCLUDE_A.search(line):
            version_linenr = idx

        if RE_VERSION_MACROS.search(line):
            uses_version = True
            if version_linenr:
                break

    if uses_version and not version_linenr:
        print(f"{path}: {len(lines)}: need linux/version.h")
    if version_linenr and not uses_version:
        print(f"{path}: {version_linenr} linux/version.h not needed.")
    if debugging:
        if version_linenr and uses_version:
            print(f"{path}: version use is OK ({version_linenr})")
        if not version_linenr and not uses_version:
            print(f"{path}: version use is OK (none)")


def main(argv):
    for path in argv:
        if any(skip in path for skip in SKIP_FILES):
            continue
        check_file(path)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))