#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# extract-sys-certs.py [-s <systemmap-file>] <vmlinux-file> <keyring-file>

import re
import subprocess
import sys

RE_SECTION = re.compile(
    r'\s*([0-9]+)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+'
    r'([0-9a-f]+)\s+([0-9a-f]+)\s+2\*\*([0-9]+)')
RE_SYMBOL = re.compile(r'([0-9a-f]+)\s([a-zA-Z])\s(\S+)')


def parse_symbols(lines):
    syms = {}
    n = 0
    for line in lines:
        m = RE_SYMBOL.search(line)
        if m:
            syms[m.group(3)] = int(m.group(1), 16)
            n += 1
    return syms, n


def main(argv):
    args = list(argv)
    sysmap = ''
    if len(args) == 4 and args[0] == '-s':
        sysmap = args[1]
        args = args[2:]
    elif len(args) != 2:
        sys.exit(f'Format: {sys.argv[0]} [-s <systemmap-file>] '
                 f'<vmlinux-file> <keyring-file>')

    vmlinux, keyring = args

    try:
        objdump_out = subprocess.check_output(
            ['objdump', '-h', vmlinux], text=True)
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        sys.exit(f'{vmlinux}: {e}')

    sections = []
    for line in objdump_out.splitlines():
        m = RE_SECTION.search(line)
        if not m:
            continue
        sections.append({
            'name': m.group(2),
            'len': int(m.group(3), 16),
            'vma': int(m.group(4), 16),
            'foff': int(m.group(6), 16),
        })

    print(f'Have {len(sections)} sections')

    try:
        nm_out = subprocess.check_output(
            ['nm', vmlinux], text=True, stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, FileNotFoundError):
        nm_out = ''

    symbols, nr = parse_symbols(nm_out.splitlines())

    if nr == 0 and sysmap:
        print(f'No symbols in vmlinux, trying {sysmap}')
        with open(sysmap) as f:
            symbols, nr = parse_symbols(f.readlines())

    if nr == 0:
        sys.exit('No symbols available')
    print(f'Have {nr} symbols')

    if '__cert_list_start' not in symbols or \
            'system_certificate_list_size' not in symbols:
        sys.exit("Can't find system certificate list")

    start = symbols['__cert_list_start']
    size_sym = symbols['system_certificate_list_size']

    found = None
    size = 0
    buf = b''
    with open(vmlinux, 'rb') as f:
        for sec in sections:
            s_vma = sec['vma']
            s_len = sec['len']
            s_vend = s_vma + s_len

            if start < s_vma or start >= s_vend:
                continue

            if size_sym < s_vma or size_sym > s_vend:
                sys.exit('Certificate list size was not found on the same section')

            if found is not None:
                sys.exit(f"Cert object in multiple sections: "
                         f"{sec['name']} and {found['name']}")

            size_off = size_sym - s_vma + sec['foff']
            f.seek(size_off)
            packed = f.read(8)
            if len(packed) != 8:
                sys.exit(vmlinux)
            size = int.from_bytes(packed, sys.byteorder)
            end = start + size

            print(f'Have {size} bytes of certs at VMA 0x{start:x}')
            if end > s_vend:
                sys.exit(f"Cert object partially overflows section {sec['name']}")
            found = sec

        if found is None:
            sys.exit('Cert object not inside a section')

        print(f"Certificate list in section {found['name']}")
        foff = start - found['vma'] + found['foff']
        print(f'Certificate list at file offset 0x{foff:x}')
        f.seek(foff)
        buf = f.read(size)
        if len(buf) != size:
            sys.exit(f'Short read on {vmlinux}')

    with open(keyring, 'wb') as f:
        f.write(buf)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))