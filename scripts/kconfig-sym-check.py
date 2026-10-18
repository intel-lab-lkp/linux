#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Detect unknown symbols referenced in Kconfig files.
# Usage: kconfig-sym-check.py <srctree> [excludes file]

import os
import re
import subprocess
import sys

VALID = re.compile(r'^[a-zA-Z0-9_]+$')
RE_DEF = re.compile(r'^\s*(?:config|menuconfig)\s+([a-zA-Z0-9_]+)\s*(?:#.*)?$')
RE_HELP = re.compile(r'^\s*(?:help|---help---)$')
RE_REF = re.compile(
    r'^\s*(?:default|def_bool|def_tristate|select|depends\s+on|imply|'
    r'visible\s+if|range|if|bool|tristate|int|hex|string|prompt)\s+(.+)\s*$')
RE_STRING = re.compile(r'"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'')
RE_SKIP = re.compile(r'^\s*(?:#.*)?$')


def indent_depth(ws):
    col = 0
    for c in ws:
        if c == '\t':
            col = (col // 8) * 8 + 8
        else:
            col += 1
    return col


def find_kconfig_files(srctree):
    try:
        out = subprocess.check_output(
            ['git', '-C', srctree, 'ls-files', '*Kconfig*'],
            text=True, stderr=subprocess.DEVNULL)
        files = [ln.strip() for ln in out.splitlines() if ln.strip()]
        if files:
            return [os.path.join(srctree, f) for f in files]
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    try:
        out = subprocess.check_output(
            ['find', srctree, '-name', '*Kconfig*'], text=True)
        return [ln.strip() for ln in out.splitlines() if ln.strip()]
    except (subprocess.CalledProcessError, FileNotFoundError):
        return []


def parse_kconfig(path, configs, refs):
    with open(path) as f:
        raw = f.readlines()

    merged = []
    i = 0
    while i < len(raw):
        line = raw[i].rstrip('\n')
        while line.endswith('\\') and i + 1 < len(raw):
            line = line[:-1] + ' ' + raw[i + 1].rstrip('\n')
            i += 1
        merged.append(line)
        i += 1

    help_mode = False
    help_level = 0
    i = 0
    while i < len(merged):
        line = merged[i]

        if not line.strip():
            i += 1
            continue
        if line.lstrip().startswith('#'):
            i += 1
            continue

        level = indent_depth(re.match(r'^(\s*)', line).group(1))

        if help_mode and level < help_level:
            help_mode = False
        if help_mode:
            i += 1
            continue

        if RE_HELP.match(line):
            help_mode = True
            j = i + 1
            while j < len(merged) and RE_SKIP.match(merged[j]):
                j += 1
            if j >= len(merged):
                break
            next_level = indent_depth(
                re.match(r'^(\s*)', merged[j]).group(1))
            if next_level >= level:
                help_level = next_level
            else:
                help_mode = False
            i = j
            continue

        m = RE_DEF.match(line)
        if m:
            configs[m.group(1)] = configs.get(m.group(1), 0) + 1
            i += 1
            continue

        m = RE_REF.match(line)
        if m:
            s = m.group(1)
            s = RE_STRING.sub('', s)
            s = re.sub(r'#.*', '', s)
            s = re.sub(r'\$\((?:[^()]*|\((?:[^()]*|\([^()]*\))*\))*\)', '', s)
            s = re.sub(r'%%[^%]*%%', '', s)
            for sym in re.split(r'[^a-zA-Z0-9_]+', s):
                if not sym or not sym[0].isalpha():
                    continue
                if sym in ('if', 'y', 'n', 'm'):
                    continue
                if re.match(r'^0[xX][0-9a-fA-F]+$', sym):
                    continue
                refs[sym] = refs.get(sym, 0) + 1
        i += 1


def main(argv):
    if not argv:
        s = ''
        try:
            s = subprocess.check_output(
                ['git', 'rev-parse', '--show-toplevel'],
                text=True, stderr=subprocess.DEVNULL).strip()
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass
        msg = f'Usage: {sys.argv[0]} <srctree> [excludes file]\n'
        msg += 'Please provide <srctree>.'
        if s:
            msg += f" Is it '{s}'?"
        sys.exit(msg + '\n')

    srctree = argv[0]
    excludes = argv[1] if len(argv) > 1 else None

    files = [f for f in find_kconfig_files(srctree)
             if '/scripts/kconfig/tests/' not in f]

    configs = {}
    refs = {}
    for f in files:
        parse_kconfig(f, configs, refs)

    known = {}
    if excludes:
        with open(excludes) as f:
            for line in f:
                line = line.rstrip('\n')
                if not line.strip() or line.lstrip().startswith('#'):
                    continue
                m = re.match(r'^\s*([a-zA-Z0-9_]+)\s*(?:#.*)?$', line)
                if m:
                    known[m.group(1)] = 1

    ret = 0
    for k in sorted(refs):
        if k in configs or k in known:
            continue
        print(k, end='')
        if k in ('N', 'Y', 'M'):
            print(f" - warning: '{k}' is probably not what you want; "
                  f"Kconfig tristate literals are always lowercase "
                  f"('n', 'y', 'm')", end='')
        print()
        ret = 1
    return ret


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))