#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Sort MAINTAINERS file sections alphabetically.

import argparse
import re
import sys
from functools import cmp_to_key

PREFERRED_ORDER = 'MRLSWQBCPTFXNK'


def by_category_cmp(a, b):
    aa = a.upper().replace('THE REST', 'ZZZZZZ')
    bb = b.upper().replace('THE REST', 'ZZZZZZ')
    return (aa > bb) - (aa < bb)


def by_pattern_cmp(a, b):
    a1 = a[0].upper() if a else ''
    b1 = b[0].upper() if b else ''
    ai = PREFERRED_ORDER.find(a1)
    bi = PREFERRED_ORDER.find(b1)
    if ai < 0:
        ai = 1000
    if bi < 0:
        bi = 1000
    if (a1 == 'F' and b1 == 'F') or (a1 == 'X' and b1 == 'X'):
        return (a > b) - (a < b)
    if ai < bi:
        return -1
    if ai == bi:
        return 0
    return 1


def trim(s):
    return s.strip()


def alpha_output(ref, filename, order):
    if not ref:
        return
    with open(filename, 'w') as f:
        sep = None
        for key in sorted(ref.keys(), key=cmp_to_key(by_category_cmp)):
            if key == ' ':
                f.write(ref[key])
            else:
                if sep is None:
                    sep = '\n'
                else:
                    f.write(sep)
                f.write(key + '\n')
                patterns = ref[key].split('\n')
                if order:
                    patterns = sorted(patterns, key=cmp_to_key(by_pattern_cmp))
                for p in patterns:
                    if p:
                        f.write(p + '\n')


def file_input(ref, filename):
    lastline = ''
    case = ' '
    ref[case] = ''

    with open(filename) as f:
        lines = f.readlines()

    for line in lines:
        m = re.match(r'^([A-Z]):\s*(.*)', line)
        if m:
            line = m.group(1) + ':\t' + trim(m.group(2)) + '\n'
            if lastline == '':
                ref[case] += line
                continue
            case = trim(lastline)
            if case in ref:
                sys.exit(f"Header '{case}' already exists")
            ref[case] = line
            lastline = ''
            continue

        if case == ' ':
            ref[case] += lastline
            lastline = line
            continue
        if trim(lastline) != '':
            sys.exit(f"Odd non-pattern line '{lastline}' for '{case}'")
        lastline = line

    ref[case] += lastline


def main(argv):
    p = argparse.ArgumentParser(add_help=False)
    p.add_argument('--input', default='MAINTAINERS')
    p.add_argument('--output', default='MAINTAINERS.new')
    p.add_argument('--section', default='SECTION.new')
    p.add_argument('--order', action='store_true', default=False)
    p.add_argument('-h', '--help', '--usage', action='store_true')
    p.add_argument('patterns', nargs='*')
    args = p.parse_args(argv)

    if args.help:
        print_usage()
        return 0

    hash_ = {}
    file_input(hash_, args.input)

    new_hash = {}
    for pattern in args.patterns:
        for key in list(hash_.keys()):
            if re.search(pattern, key) or re.search(pattern, hash_[key]):
                new_hash[key] = hash_[key]
                del hash_[key]

    alpha_output(hash_, args.output, args.order)
    alpha_output(new_hash, args.section, args.order)
    return 0


def print_usage():
    print(f'''usage: {sys.argv[0]} [options] <pattern matching regexes>

  --input => MAINTAINERS file to read (default: MAINTAINERS)
  --output => sorted MAINTAINERS file to write (default: MAINTAINERS.new)
  --section => new sorted MAINTAINERS file to write (default: SECTION.new)
  --order => Use the preferred section content output ordering (default: 0)''')


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))