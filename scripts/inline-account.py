#!/usr/bin/env python3
# account code bytes per source code / functions from objdump -Sl output
# useful to find inline bloat
#
# SPDX-License-Identifier: GPL-2.0-only
# Author: Andi Kleen

import os
import sys
import re
import argparse
import bisect
import multiprocessing
from collections import Counter
from functools import reduce, partial

def get_args():
    p = argparse.ArgumentParser(
            description="""
Account code bytes per source code / functions from objdump.
Useful to find inline bloat.

The line numbers are the beginning of a block, so the actual code can be later.
Line numbers can be a also little off due to objdump bugs
also some misaccounting can happen due to inexact gcc debug information.
The number output for functions may account a single large function multiple
times.  program/object files need to be built with -g.

This is somewhat slow due to objdump -S being slow. It helps to have
plenty of cores.""")
    p.add_argument('--min-bytes', type=int, help='minimum bytes to report', default=100)
    p.add_argument('--threads', '-t', type=int, default=multiprocessing.cpu_count(),
                   help='Number of objdump processes to run')
    p.add_argument('--verbose', '-v', action='store_true', help="Print more")
    p.add_argument('--show', type=int, help='Number of results to show')
    p.add_argument('--objdump', default="objdump", help="Set objdump binary to run")
    p.add_argument('file', help='object file/program as input')
    return p.parse_args()

def get_syms(fn):
    syms = []
    pc = None
    with os.popen("nm -n --print-size " + fn) as f:
        for l in f:
            n = l.split()
            if len(n) > 2 and n[2].upper() == "T":
                pc = int(n[0], 16)
                syms.append(pc)
                ln = int(n[1], 16)
    if not pc:
        sys.exit(fn + " has no symbols")
    syms.append(pc + ln)
    return syms

class Account:
    def __init__(self):
        self.funcbytes = Counter()
        self.linebytes = Counter()
        self.funccount = Counter()
        self.nolinebytes = 0
        self.nofuncbytes = 0
        self.total = 0

    def add(self, b):
        self.funcbytes += b.funcbytes
        self.linebytes += b.linebytes
        self.funccount += b.funccount
        self.nolinebytes += b.nolinebytes
        self.nofuncbytes += b.nofuncbytes
        self.total += b.total
        return self

# dont add sys.exit here, causes deadlocks
def account_range(args, r):
    a = Account()
    line = None
    func = None
    codefunc = None

    cmd = ("%s -Sl %s --start-address=%#x --stop-address=%#x" %
                (args.objdump, args.file, r[0], r[1]))
    if args.verbose:
        print(cmd)
    with os.popen(cmd) as f:
        for l in f:
            #      250:       e8 00 00 00 00          callq  255 <proc_skip_spaces+0x5>
            m = re.match(r'\s*([0-9a-fA-F]+):\s+(.*)', l)
            if m:
                bytes = len(re.findall(r'[0-9a-f][0-9a-f] ', m.group(2)))
                if not func:
                    a.nofuncbytes += bytes
                    continue
                if not line:
                    a.nolinebytes += bytes
                    continue
                a.total += bytes
                a.funcbytes[func] += bytes
                a.linebytes[(file, line)] += bytes
                codefunc = func
                continue

            # sysctl_init():
            m = re.match(r'([a-zA-Z_][a-zA-Z0-9_]*)\(\):$', l)
            if m:
                if codefunc and m.group(1) != codefunc:
                    a.funccount[codefunc] += 1
                    codefunc = None
                func = m.group(1)
                continue

            # /sysctl.c:1666
            m = re.match(r'^([^:]+):(\d+)$', l)
            if m:
                file, line = m.group(1), int(m.group(2))
                continue

    if codefunc:
        a.funccount[codefunc] += 1
    return a

def get_boundaries(syms, sym_sizes, chunk):
    run = 0
    boundaries = [syms[0]]
    for i, x in enumerate(sym_sizes):
        run += x
        if run >= chunk:
            boundaries.append(syms[i])
            run = 0
    boundaries.append(syms[-1])
    return boundaries


def process(args):
    # objdump -S is slow, so we parallelize

    # split symbol table into chunks for parallelization
    # we split on functions boundaries to avoid mis-accounting
    syms = get_syms(args.file)
    if len(syms) < 2:
        print("not enough symbols")
        return
    sym_sizes = [syms[x + 1] - syms[x] for x, _ in enumerate(syms[:-1])]
    sym_total = sum(sym_sizes)
    chunk = max(int(sym_total / args.threads), 1)
    boundaries = get_boundaries(syms, sym_sizes, chunk)
    ranges = [(boundaries[x], boundaries[x+1]) for x in range(0, len(boundaries) - 1)]
    assert ranges[0][0] == syms[0]
    assert ranges[-1][1] == syms[-1]

    # map-reduce
    account_func = partial(account_range, args)
    if args.threads == 1:
        al = list(map(account_func, ranges))
    else:
        al = multiprocessing.Pool(args.threads).map(account_func, ranges)
    a = reduce(lambda a, b: a.add(b), al)

    print("Total code bytes seen", a.total)
    if args.verbose:
        print("Bytes with no function %d (%.2f%%)" % (a.nofuncbytes, 100.0*(float(a.nofuncbytes)/a.total)))
        print("Bytes with no lines %d (%.2f%%)" % (a.nolinebytes, 100.0*(float(a.nolinebytes)/a.total)))

    def sort_map(m):
        return sorted(list(m.keys()), key=lambda x: m[x], reverse=True)

    print("\nCode bytes by functions:")
    print("%-50s %-5s  %-5s   %-5s %-5s" % ("Function", "Total", "", "Avg", "Num"))
    for i, j in enumerate(sort_map(a.funcbytes)):
        if a.funcbytes[j] < args.min_bytes:
            break
        if args.show and i >= args.show:
            break
        print("%-50s %-5d (%.2f%%)  %-5d %-5d" % (
                j,
                a.funcbytes[j],
                a.funcbytes[j] / float(a.total),
                a.funcbytes[j] / a.funccount[j],
                a.funccount[j]))

    for j in list(a.linebytes.keys()):
        if a.linebytes[j] < args.min_bytes:
            del a.linebytes[j]

    prefix = os.path.commonprefix([x[0] for x in list(a.linebytes.keys())])

    print("\nCode bytes by nearby source line blocks:")
    print("prefix", prefix)

    print("%-50s %-5s" % ("Line", "Total"))
    for i, j in enumerate(sort_map(a.linebytes)):
        if args.show and i >= args.show:
            break
        print("%-50s %-5d (%.2f%%)" % (
                "%s:%d" % (j[0][len(prefix):], j[1]),
                a.linebytes[j],
                a.linebytes[j] / float(a.total)))
    if len(a.linebytes) == 0:
        print("Nothing found. enable CONFIG_DEBUG_INFO / -g?")

if __name__ == '__main__':
    process(get_args())
