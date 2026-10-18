#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright 2008, Intel Corporation
# Authors: Arjan van de Ven <arjan@linux.intel.com>
#
# Turn dmesg output into an SVG graphic showing which initcalls take
# how much time. The kernel must be compiled with CONFIG_PRINTK_TIME
# and booted with "initcall_debug" on the command line.
#
# usage:
#     dmesg | python3 scripts/bootgraph.py > output.svg

import argparse
import re
import subprocess
import sys

RE_CALLING     = re.compile(r'([0-9.]+)\] calling  ([a-zA-Z0-9_.]+)\+')
RE_PID         = re.compile(r'@ ([0-9]+)')
RE_ASYNC_WAIT  = re.compile(r'([0-9.]+)\] async_waiting @ ([0-9]+)')
RE_RETURNED    = re.compile(r'([0-9.]+)\] initcall ([a-zA-Z0-9_.]+)\+.*returned')
RE_ASYNC_CONT  = re.compile(r'([0-9.]+)\] async_continuing @ ([0-9]+)')
RE_STOP        = re.compile(r'(Write protecting the|Freeing unused kernel memory)')

STYLES = [
    'fill:rgb(0,0,255);fill-opacity:0.5;stroke-width:1;stroke:rgb(0,0,0)',
    'fill:rgb(0,255,0);fill-opacity:0.5;stroke-width:1;stroke:rgb(0,0,0)',
    'fill:rgb(255,0,20);fill-opacity:0.5;stroke-width:1;stroke:rgb(0,0,0)',
    'fill:rgb(255,255,20);fill-opacity:0.5;stroke-width:1;stroke:rgb(0,0,0)',
    'fill:rgb(255,0,255);fill-opacity:0.5;stroke-width:1;stroke:rgb(0,0,0)',
    'fill:rgb(0,255,255);fill-opacity:0.5;stroke-width:1;stroke:rgb(0,0,0)',
    'fill:rgb(0,128,255);fill-opacity:0.5;stroke-width:1;stroke:rgb(0,0,0)',
    'fill:rgb(0,255,128);fill-opacity:0.5;stroke-width:1;stroke:rgb(0,0,0)',
    'fill:rgb(255,0,128);fill-opacity:0.5;stroke-width:1;stroke:rgb(0,0,0)',
    'fill:rgb(255,255,128);fill-opacity:0.5;stroke-width:1;stroke:rgb(0,0,0)',
    'fill:rgb(255,128,255);fill-opacity:0.5;stroke-width:1;stroke:rgb(0,0,0)',
    'fill:rgb(128,255,255);fill-opacity:0.5;stroke-width:1;stroke:rgb(0,0,0)',
]
STYLE_WAIT = 'fill:rgb(128,128,128);fill-opacity:0.5;stroke-width:0;stroke:rgb(0,0,0)'

HEADERSTEP = 20
XHEADER    = 15
YHEADER    = 25


def usage(exit_code=1):
    stream = sys.stdout if exit_code == 0 else sys.stderr
    print('Usage:', file=stream)
    print('1) dmesg | python3 scripts/bootgraph.py [OPTION] > output.svg',
          file=stream)
    print('2) python3 scripts/bootgraph.py -h', file=stream)
    print('', file=stream)
    print('Options:', file=stream)
    print('\t-header\tInsert kernel version and date', file=stream)
    sys.exit(exit_code)


def parse_dmesg(stream):
    """Return (start, end, types, pids, count, first, last)."""
    start, end, types, pids, pidctr = {}, {}, {}, {}, {}
    done = False
    first, last, count = 99999.0, 0.0, 0

    for line in stream:
        m = RE_CALLING.search(line)
        if m:
            func = m.group(2)
            if not done:
                start[func] = float(m.group(1))
                types[func] = 0
                first = min(first, start[func])
            mp = RE_PID.search(line)
            if mp:
                pids[func] = int(mp.group(1))
            count += 1

        m = RE_ASYNC_WAIT.search(line)
        if m:
            pid = int(m.group(2))
            pidctr[pid] = pidctr.get(pid, 0) + 1
            func = f'wait_{pid}_{pidctr[pid]}'
            if not done:
                start[func] = float(m.group(1))
                types[func] = 1
                first = min(first, start[func])
            pids[func] = pid
            count += 1

        m = RE_RETURNED.search(line)
        if m and not done:
            end[m.group(2)] = float(m.group(1))
            last = end[m.group(2)]

        m = RE_ASYNC_CONT.search(line)
        if m:
            pid = int(m.group(2))
            func = f'wait_{pid}_{pidctr.get(pid, "")}'
            end[func] = float(m.group(1))
            last = end[func]

        if RE_STOP.search(line):
            done = True

    return start, end, types, pids, count, first, last


def emit_svg(out, start, end, types, pids, first, last, header):
    out.write('<?xml version="1.0" standalone="no"?> \n')
    out.write('<svg width="2000" height="100%" version="1.1" '
              'xmlns="http://www.w3.org/2000/svg">\n')

    if header:
        version = subprocess.check_output(['uname', '-a'], text=True).rstrip()
        date = subprocess.check_output(['date'], text=True).rstrip()
        out.write(f'<text transform="translate({XHEADER},{YHEADER})">'
                  f'Kernel version: {version}</text>\n')
        cyheader = YHEADER + HEADERSTEP
        out.write(f'<text transform="translate({XHEADER},{cyheader})">'
                  f'Date: {date}</text>\n')

    mult = 1950.0 / (last - first)
    threshold2 = (last - first) / 120.0
    threshold = threshold2 / 10

    stylecounter = 0
    rows, rowscount = {}, 1

    for key in sorted(start.keys(), key=lambda k: start[k]):
        duration = end[key] - start[key]
        if duration < threshold:
            continue
        pid = pids.get(key)
        if pid not in rows:
            rows[pid] = rowscount
            rowscount += 1
        s = (start[key] - first) * mult
        s2, s3 = s + 6, s + 1
        e = (end[key] - first) * mult
        w = e - s
        y = rows[pid] * 150
        y2 = y + 4
        style = STYLES[stylecounter]
        stylecounter = (stylecounter + 1) % 12
        if types[key] == 1:
            y += 15
            out.write(f'<rect x="{s:.15g}" width="{w:.15g}" y="{y}" '
                      f'height="115" style="{STYLE_WAIT}"/>\n')
        else:
            out.write(f'<rect x="{s:.15g}" width="{w:.15g}" y="{y}" '
                      f'height="145" style="{style}"/>\n')
            if duration >= threshold2:
                out.write(f'<text transform="translate({s2:.15g},{y2}) '
                          f'rotate(90)">{key}</text>\n')
            else:
                out.write(f'<text transform="translate({s3:.15g},{y2}) '
                          f'rotate(90)" font-size="3pt">{key}</text>\n')

    # top timeline
    time = first
    step = (last - first) / 15.0
    while time < last:
        s3 = (time - first) * mult
        tm = int(time * 100) / 100.0
        out.write(f'<text transform="translate({s3:.15g},89) rotate(90)">{tm:.15g}</text>\n')
        time += step

    out.write('</svg>\n')


def main(argv):
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('-h', '--help', action='store_true')
    parser.add_argument('-header', '--header', action='store_true')
    args = parser.parse_args(argv)

    if args.help:
        usage(0)

    start, end, types, pids, count, first, last = parse_dmesg(sys.stdin)

    if count == 0:
        print("No data found in the dmesg. Make sure that 'printk.time=1' and",
              file=sys.stderr)
        print("'initcall_debug' are passed on the kernel command line.",
              file=sys.stderr)
        usage(1)

    emit_svg(sys.stdout, start, end, types, pids, first, last, args.header)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))