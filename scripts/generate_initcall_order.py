#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Generates a linker script that specifies the correct initcall order.
# Copyright (C) 2019 Google LLC

import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

NM     = os.environ.get('NM') or sys.exit("ERROR: NM not set?")
OBJTREE = os.environ.get('objtree') or '.'

RE_INITCALL = re.compile(r'[a-z]\s+__initcall__(\S*)__(\d+)_(\d+)_(.*)$')
RE_FUNC_LEVEL = re.compile(r'^(.*)((early|rootfs|con|[0-9])s?)$')
RE_ARCHIVE_HDR = re.compile(r'^(.+):$')


def find_initcalls(index, file):
    """Return list of (level, secname) for one object/archive."""
    if not os.path.isfile(file):
        sys.exit(f"ERROR: file {file} doesn't exist?")
    try:
        output = subprocess.check_output(
            [NM, '--defined-only', file],
            stderr=subprocess.DEVNULL, text=True)
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        sys.exit(f"ERROR: failed to execute \"{NM}\": {e}")

    results = []
    current = {}

    def flush():
        for counter in sorted(current.keys(), key=int):
            e = current[counter]
            secname = f"{e['module']}__{counter}_{e['line']}_{e['function']}"
            results.append((e['level'], secname))

    for raw in output.splitlines():
        line = raw.strip()
        m = RE_ARCHIVE_HDR.match(line)
        if m:
            flush()
            current = {}
            continue

        m = RE_INITCALL.search(line)
        if not m:
            continue
        module = m.group(1) or ''
        counter = m.group(2)
        line_no = m.group(3)
        symbol = m.group(4)

        fm = RE_FUNC_LEVEL.match(symbol)
        if not fm:
            sys.exit(f"ERROR: invalid initcall name {symbol} in "
                     f"{file}({m.group(1)})")
        function, level = fm.group(1), fm.group(2)

        current[counter] = {
            'module': module, 'line': line_no,
            'function': function, 'level': level,
        }

    flush()
    return results


def process_files(paths):
    """Return {level: [secname, ...]} preserving link order."""
    njobs = int(os.environ.get('PARALLELISM')
                or os.cpu_count() or 1)

    indexed = [(i, os.path.join(OBJTREE, p))
               for i, p in enumerate(paths)]

    with ThreadPoolExecutor(max_workers=njobs) as pool:
        # Submit all, gather in submission order to preserve link order.
        futures = [pool.submit(find_initcalls, i, f)
                   for i, f in indexed]
        results_by_index = [f.result() for f in futures]

    sections = {}
    for idx_results in results_by_index:
        for level, secname in idx_results:
            sections.setdefault(level, []).append(secname)
    return sections


def generate_initcall_lds(paths):
    sections = process_files(paths)
    if not sections:
        sys.exit("ERROR: no initcalls?")

    print("SECTIONS {")
    for level in sorted(sections):
        if level == 'con':
            section = '.con_initcall.init'
        else:
            section = f".initcall{level}.init"
        print(f"\t{section} : {{")
        for secname in sections[level]:
            print(f"\t\t*({section}..{secname}) ;")
        print("\t}")
    print("}")


def main(argv):
    if not argv:
        sys.exit("usage: generate_initcall_order.py <object files...>")
    generate_initcall_lds(argv)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))