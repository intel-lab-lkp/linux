#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Detect cycles in the header file dependency graph.
# Vegard Nossum <vegardno@ifi.uio.no>

import argparse
import os
import re
import sys

deps = {}
opt_include = []


def strip(filename):
    for inc in opt_include:
        prefix = inc + '/'
        if filename.startswith(prefix):
            return filename[len(prefix):]
    return filename


def search(filename):
    if os.path.isfile(filename):
        return filename
    for inc in opt_include:
        path = f'{inc}/{filename}'
        if os.path.isfile(path):
            return path
    return None


def parse_all(headers):
    queue = list(headers)
    while queue:
        h = queue.pop()
        if h in deps:
            continue
        deps[h] = []
        path = search(h)
        if path is None:
            continue
        with open(path) as f:
            lines = f.readlines()
        for i, line in enumerate(lines):
            m = re.match(r'^#\s*include\s*<(.*?)>', line)
            if m:
                dep = m.group(1)
                queue.append(dep)
                deps[h].append((i + 1, dep))


def print_cycle(cycle):
    # Shift line numbers to align with the "In file included" message
    cycle = list(cycle)
    for i in range(len(cycle) - 1):
        cycle[i] = (cycle[i + 1][0], cycle[i][1])
    cycle[-1] = (0, cycle[-1][1])

    first = cycle.pop(0)
    last = cycle.pop() if cycle else None

    msg = 'In file included'
    if last is not None:
        print(f'{msg} from {last[1]},')
    for hdr in reversed(cycle):
        marker = ' <-- here' if last is not None and hdr[1] == last[1] else ''
        print(f'{" " * len(msg)} from {hdr[1]}:{hdr[0]}{marker}')
    print(f'{first[1]}:{first[0]}: warning: recursive header inclusion')


def detect_cycles(headers, opt_all):
    queue = [[(0, h)] for h in headers]
    while queue:
        top = queue.pop()
        name = top[-1][1]
        for dep in deps.get(name, []):
            chain = top + [(dep[0], dep[1])]
            if any(c[1] == dep[1] for c in top):
                print_cycle(chain)
                if not opt_all:
                    return
                continue
            queue.append(chain)


def mangle(name):
    return name.replace('/', '__').replace('.', '_').replace('-', '_')


def graph():
    print('digraph {')
    print()
    print('\t/* vertices */')
    for h in deps:
        print(f'\t{mangle(h)} [label="{h}"];')
    print()
    print('\t/* edges */')
    for h in deps:
        for dep in deps[h]:
            print(f'\t{mangle(h)} -> {mangle(dep[1])};')
    print('}')


def main(argv):
    global opt_include

    p = argparse.ArgumentParser(add_help=False)
    p.add_argument('--all', action='store_true')
    p.add_argument('--graph', action='store_true')
    p.add_argument('-I', dest='includes', action='append', default=[])
    p.add_argument('files', nargs='*')
    p.add_argument('-h', '--help', '--version', action='store_true')
    args = p.parse_args(argv)

    if args.help:
        print(f'Usage: {sys.argv[0]} [options] file...')
        return 0

    opt_include = list(args.includes) + ['include']
    # Match Perl: grep { strip($_) } keeps the ORIGINAL path, using
    # strip() only as a truthiness filter. This makes deps keys
    # consistent with the Perl version (e.g. 'include/x.h', not 'x.h').
    headers = [h for h in args.files if strip(h)]

    parse_all(headers)

    if args.graph:
        graph()
    else:
        detect_cycles(headers, args.all)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))