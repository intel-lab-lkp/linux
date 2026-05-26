#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
stackmap_dump.py - Parse and display ftrace stack_map_bin binary export.

Usage:
    # Pull from device and parse
    adb pull /sys/kernel/debug/tracing/stack_map_bin /tmp/stack_map.bin
    python3 stackmap_dump.py /tmp/stack_map.bin

    # With vmlinux for offline symbol resolution
    python3 stackmap_dump.py /tmp/stack_map.bin --vmlinux vmlinux

    # JSON output for tooling
    python3 stackmap_dump.py /tmp/stack_map.bin --json
"""

import struct
import sys
import argparse
import json
import subprocess

MAGIC = 0x464D5342  # 'FSMB'
HEADER_SIZE = 16  # 4 x u32
ENTRY_SIZE = 16   # 4 x u32


def detect_endianness(data):
    """Detect byte order from magic number in header."""
    if len(data) < 4:
        raise ValueError("File too small")
    magic_le = struct.unpack_from('<I', data, 0)[0]
    if magic_le == MAGIC:
        return '<'
    magic_be = struct.unpack_from('>I', data, 0)[0]
    if magic_be == MAGIC:
        return '>'
    raise ValueError(f"Bad magic: 0x{magic_le:08x} (neither LE nor BE)")


def batch_addr2line(vmlinux, addrs):
    """Resolve multiple addresses in one addr2line invocation."""
    if not addrs:
        return {}
    try:
        # Feed addresses on stdin to avoid ARG_MAX limits with large
        # numbers of addresses (one stack can have 30+ frames; a
        # snapshot can have thousands of unique stacks).
        stdin = '\n'.join(hex(a) for a in addrs) + '\n'
        result = subprocess.run(
            ['addr2line', '-f', '-e', vmlinux],
            input=stdin, capture_output=True, text=True, timeout=60
        )
        lines = result.stdout.split('\n')
        # addr2line outputs 2 lines per address: function name + source location
        symbols = {}
        for i, addr in enumerate(addrs):
            idx = i * 2
            if idx < len(lines) and lines[idx] and lines[idx] != '??':
                symbols[addr] = lines[idx]
        return symbols
    except (subprocess.TimeoutExpired, FileNotFoundError) as e:
        print(f"warning: addr2line failed: {e}", file=sys.stderr)
        return {}


def parse_stackmap_bin(data):
    """Parse binary stackmap data, yield (stack_id, ref_count, [ips])."""
    if len(data) < HEADER_SIZE:
        raise ValueError("File too small for header")

    endian = detect_endianness(data)
    header_fmt = f'{endian}IIII'
    entry_fmt = f'{endian}IIII'

    magic, version, nr_stacks, _ = struct.unpack_from(header_fmt, data, 0)
    if version != 2:
        raise ValueError(f"Unsupported version: {version}")

    offset = HEADER_SIZE
    for _ in range(nr_stacks):
        if offset + ENTRY_SIZE > len(data):
            break
        stack_id, nr, ref_count, _ = struct.unpack_from(entry_fmt, data, offset)
        offset += ENTRY_SIZE

        ips_size = nr * 8
        if offset + ips_size > len(data):
            break
        ips = struct.unpack_from(f'{endian}{nr}Q', data, offset)
        offset += ips_size

        yield stack_id, ref_count, list(ips)


def main():
    parser = argparse.ArgumentParser(description='Parse ftrace stack_map_bin')
    parser.add_argument('file', help='Path to stack_map_bin file')
    parser.add_argument('--vmlinux', help='Path to vmlinux for symbol resolution')
    parser.add_argument('--json', action='store_true', help='JSON output')
    parser.add_argument('--top', type=int, default=0,
                        help='Show only top N stacks by ref_count')
    args = parser.parse_args()

    with open(args.file, 'rb') as f:
        data = f.read()

    stacks = list(parse_stackmap_bin(data))

    if args.top > 0:
        stacks.sort(key=lambda x: x[1], reverse=True)
        stacks = stacks[:args.top]

    # Batch symbol resolution
    symbols = {}
    if args.vmlinux:
        all_addrs = set()
        for _, _, ips in stacks:
            all_addrs.update(ips)
        symbols = batch_addr2line(args.vmlinux, list(all_addrs))

    if args.json:
        output = []
        for stack_id, ref_count, ips in stacks:
            entry = {
                'stack_id': stack_id,
                'ref_count': ref_count,
                'ips': [f'0x{ip:x}' for ip in ips]
            }
            if args.vmlinux:
                entry['symbols'] = [symbols.get(ip, f'0x{ip:x}')
                                    for ip in ips]
            output.append(entry)
        print(json.dumps(output, indent=2))
    else:
        for stack_id, ref_count, ips in stacks:
            print(f"stack_id {stack_id} [ref {ref_count}, depth {len(ips)}]")
            for i, ip in enumerate(ips):
                sym = symbols.get(ip, '')
                if sym:
                    sym = f' {sym}'
                print(f"  [{i}] 0x{ip:x}{sym}")
            print()

    print(f"Total: {len(stacks)} unique stacks", file=sys.stderr)


if __name__ == '__main__':
    main()
