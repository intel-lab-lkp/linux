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
HEADER_FMT = '<IIII'  # magic, version, nr_stacks, reserved
ENTRY_FMT = '<IIII'   # stack_id, nr, ref_count, reserved
HEADER_SIZE = struct.calcsize(HEADER_FMT)
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)


def addr2line(vmlinux, addr):
    """Resolve address to symbol using addr2line."""
    try:
        result = subprocess.run(
            ['addr2line', '-f', '-e', vmlinux, hex(addr)],
            capture_output=True, text=True, timeout=5
        )
        lines = result.stdout.strip().split('\n')
        if len(lines) >= 1 and lines[0] != '??':
            return lines[0]
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    return None


def parse_stackmap_bin(data):
    """Parse binary stackmap data, yield (stack_id, ref_count, [ips])."""
    if len(data) < HEADER_SIZE:
        raise ValueError("File too small for header")

    magic, version, nr_stacks, _ = struct.unpack_from(HEADER_FMT, data, 0)
    if magic != MAGIC:
        raise ValueError(f"Bad magic: 0x{magic:08x}, expected 0x{MAGIC:08x}")
    if version not in (1, 2):
        raise ValueError(f"Unsupported version: {version}")

    offset = HEADER_SIZE
    for _ in range(nr_stacks):
        if offset + ENTRY_SIZE > len(data):
            break
        stack_id, nr, ref_count, _ = struct.unpack_from(ENTRY_FMT, data, offset)
        offset += ENTRY_SIZE

        ips_size = nr * 8
        if offset + ips_size > len(data):
            break
        ips = struct.unpack_from(f'<{nr}Q', data, offset)
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

    if args.json:
        output = []
        for stack_id, ref_count, ips in stacks:
            entry = {
                'stack_id': stack_id,
                'ref_count': ref_count,
                'ips': [f'0x{ip:x}' for ip in ips]
            }
            if args.vmlinux:
                entry['symbols'] = [addr2line(args.vmlinux, ip) or f'0x{ip:x}'
                                    for ip in ips]
            output.append(entry)
        print(json.dumps(output, indent=2))
    else:
        for stack_id, ref_count, ips in stacks:
            print(f"stack_id {stack_id} [ref {ref_count}, depth {len(ips)}]")
            for i, ip in enumerate(ips):
                sym = ''
                if args.vmlinux:
                    resolved = addr2line(args.vmlinux, ip)
                    if resolved:
                        sym = f' {resolved}'
                print(f"  [{i}] 0x{ip:x}{sym}")
            print()

    print(f"Total: {len(stacks)} unique stacks", file=sys.stderr)


if __name__ == '__main__':
    main()
