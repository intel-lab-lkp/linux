#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright 2008, Intel Corporation
# Authors: Arjan van de Ven <arjan@linux.intel.com>
#
# Turn an oops output into an annotated disassembly of the faulting
# instruction and surrounding code, tracking register values backwards
# from the faulting point.
#
# usage:
#     dmesg | python3 scripts/markup_oops.py [OPTION] [VMLINUX]

import argparse
import re
import subprocess
import sys

REGS = {}

RE_32_REGS_A = re.compile(r'EAX: ([0-9a-f]+) EBX: ([0-9a-f]+) '
                          r'ECX: ([0-9a-f]+) EDX: ([0-9a-f]+)')
RE_32_REGS_B = re.compile(r'ESI: ([0-9a-f]+) EDI: ([0-9a-f]+) '
                          r'EBP: ([0-9a-f]+) ESP: ([0-9a-f]+)')
RE_64_REGS_A = re.compile(r'RAX: ([0-9a-f]+) RBX: ([0-9a-f]+) '
                          r'RCX: ([0-9a-f]+)')
RE_64_REGS_B = re.compile(r'RDX: ([0-9a-f]+) RSI: ([0-9a-f]+) '
                          r'RDI: ([0-9a-f]+)')
RE_64_REGS_C = re.compile(r'RBP: ([0-9a-f]+) R08: ([0-9a-f]+) '
                          r'R09: ([0-9a-f]+)')
RE_64_REGS_D = re.compile(r'R10: ([0-9a-f]+) R11: ([0-9a-f]+) '
                          r'R12: ([0-9a-f]+)')
RE_64_REGS_E = re.compile(r'R13: ([0-9a-f]+) R14: ([0-9a-f]+) '
                          r'R15: ([0-9a-f]+)')

RE_EIP_32 = re.compile(r'EIP: 0060:\[\<([a-z0-9]+)\>\]')
RE_RIP_64 = re.compile(r'RIP: 0010:\[\<([a-z0-9]+)\>\]')
RE_FUNC_32 = re.compile(r'EIP is at ([a-zA-Z0-9_]+)\+0x([0-9a-f]+)/0x[a-f0-9]')
RE_FUNC_64 = re.compile(
    r'RIP: 0010:\[\<[0-9a-f]+\>\]  \[\<[0-9a-f]+\>\] '
    r'([a-zA-Z0-9_]+)\+0x([0-9a-f]+)/0x[a-f0-9]')
RE_MOD_32 = re.compile(
    r'EIP is at ([a-zA-Z0-9_]+)\+(0x[0-9a-f]+)/0x[a-f0-9]+\W\[([a-zA-Z0-9_-]+)\]')
RE_MOD_64 = re.compile(
    r'RIP: 0010:\[\<[0-9a-f]+\>\]  \[\<[0-9a-f]+\>\] '
    r'([a-zA-Z0-9_]+)\+(0x[0-9a-f]+)/0x[a-f0-9]+\W\[([a-zA-Z0-9_-]+)\]')


def parse_x86_regs(line):
    """Extract register values from one oops line into REGS."""
    m = RE_32_REGS_A.search(line)
    if m:
        REGS['%eax'] = m.group(1)
        REGS['%ebx'] = m.group(2)
        REGS['%ecx'] = m.group(3)
        REGS['%edx'] = m.group(4)
    m = RE_32_REGS_B.search(line)
    if m:
        REGS['%esi'] = m.group(1)
        REGS['%edi'] = m.group(2)
        REGS['%esp'] = m.group(4)
    m = RE_64_REGS_A.search(line)
    if m:
        REGS['%eax'] = m.group(1)
        REGS['%ebx'] = m.group(2)
        REGS['%ecx'] = m.group(3)
    m = RE_64_REGS_B.search(line)
    if m:
        REGS['%edx'] = m.group(1)
        REGS['%esi'] = m.group(2)
        REGS['%edi'] = m.group(3)
    m = RE_64_REGS_C.search(line)
    if m:
        REGS['%r08'] = m.group(2)
        REGS['%r09'] = m.group(3)
    m = RE_64_REGS_D.search(line)
    if m:
        REGS['%r10'] = m.group(1)
        REGS['%r11'] = m.group(2)
        REGS['%r12'] = m.group(3)
    m = RE_64_REGS_E.search(line)
    if m:
        REGS['%r13'] = m.group(1)
        REGS['%r14'] = m.group(2)
        REGS['%r15'] = m.group(3)


def reg_name(reg):
    """Normalize a 64-bit register name to its 32-bit form."""
    reg = re.sub(r'r(.)x', r'e\1x', reg)
    reg = re.sub(r'r(.)i', r'e\1i', reg)
    reg = re.sub(r'r(.)p', r'e\1p', reg)
    return reg


def process_x86_regs(line, cntr):
    """Annotate a disassembly line with live register values."""
    if len(line) < 40:
        return ''
    m = re.search(r'([0-9a-zA-Z,%()\-\+]+)$', line)
    if not m:
        return ''

    lastword = m.group(1)
    clobber = lastword
    clobber = re.sub(r'\([a-z0-9%,]+\)', '', clobber)
    clobber = re.sub(r'.*,', '', clobber)
    if cntr == 0:
        clobber = ''

    clobberprime = reg_name(clobber)
    lastwordprime = reg_name(lastword)
    result = ''

    for reg in list(REGS.keys()):
        val = REGS[reg]
        if re.match(r'^0+$', val):
            val = '0'
        else:
            val = re.sub(r'^0*', '', val)

        if reg in clobber or reg in clobberprime:
            if val:
                result += f' {reg} => {val} '
            REGS[reg] = ''
            val = ''

        if reg in lastword or reg in lastwordprime:
            if val:
                result += f' {reg} = {val} '

    return result


def in_range(address, target):
    delta = int('0x' + address, 16) - int('0x' + target, 16)
    return -4096 < delta < 4096


def usage():
    print('Usage:')
    print('  dmesg | python3 markup_oops.py [OPTION] [VMLINUX]')
    print()
    print('OPTION:')
    print('  -c, --cross-compile CROSS_COMPILE\t'
          'Specify the prefix used for toolchain.')
    print('  -m, --module MODULE_DIRNAME\t\t'
          'Specify the module filename.')
    print('  -h, --help\t\t\t\tHelp.')
    sys.exit(0)


def run_cmd(cmd):
    """Run a command, return stdout text or None on error."""
    try:
        return subprocess.check_output(cmd, text=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def main(argv):
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument('-c', '--cross-compile', default='',
                        metavar='CROSS_COMPILE')
    parser.add_argument('-m', '--module', default='', metavar='MODULE_DIRNAME')
    parser.add_argument('-h', '--help', action='store_true')
    parser.add_argument('vmlinux', nargs='?', default=None)
    args = parser.parse_args(argv)

    if args.help:
        usage()

    cross = args.cross_compile
    vmlinux = args.vmlinux
    modulefile = args.module

    if vmlinux is None:
        kerver = subprocess.check_output(['uname', '-r'], text=True).rstrip()
        vmlinux = f'/lib/modules/{kerver}/build/vmlinux'
        print(f'No vmlinux specified, assuming {vmlinux}')

    filename = vmlinux
    target = '0'
    function = ''
    module = ''
    func_offset = '0'
    vmaoffset = 0

    for line in sys.stdin:
        m = RE_EIP_32.search(line) or RE_RIP_64.search(line)
        if m:
            target = m.group(1)
        m = RE_FUNC_32.search(line) or RE_FUNC_64.search(line)
        if m:
            function = m.group(1)
            func_offset = m.group(2)
        m = RE_MOD_32.search(line) or RE_MOD_64.search(line)
        if m:
            module = m.group(3)
        parse_x86_regs(line)

    if target == '0':
        print('No oops found!')
        usage()

    target_int = int('0x' + target, 16)
    offset_int = int('0x' + func_offset, 16)
    decodestart = target_int - offset_int
    decodestop = target_int + 8192

    # Module case: find .ko, compute VMA offset relative to text start.
    if module:
        if not modulefile:
            out = run_cmd(['modinfo', '-F', 'filename', module])
            modulefile = out.rstrip() if out else ''
        filename = modulefile
        if not filename:
            print(f'Module .ko file for {module} not found. Aborting')
            sys.exit(1)
        out = run_cmd([cross + 'objdump', '-dS', filename])
        if out is None:
            print('Cannot start objdump', file=sys.stderr)
            sys.exit(1)
        pat = re.compile(rf'^([0-9a-f]+) \<{re.escape(function)}\>:')
        for line in out.splitlines():
            m = pat.match(line)
            if m:
                vmaoffset = (target_int
                             - int('0x' + m.group(1), 16)
                             - offset_int)

    # Disassemble the region around the faulting instruction.
    out = run_cmd([
        cross + 'objdump', '-dS',
        f'--adjust-vma={vmaoffset}',
        f'--start-address={decodestart}',
        f'--stop-address={decodestop}',
        filename,
    ])
    if out is None:
        print('Cannot start objdump', file=sys.stderr)
        sys.exit(1)

    counter = 0
    state = 0
    center = -1
    lines = []

    for line in out.splitlines():
        if state == 0:
            m = re.match(r'^([a-f0-9]+):', line)
            if m and in_range(m.group(1), target):
                state = 1
        if state == 1:
            m = re.match(r'^([a-f0-9]{6,}):', line)
            if m:
                val = m.group(1)
                if not in_range(val, target):
                    break
                if val == target:
                    center = counter
            lines.append(line)
            counter += 1

    if counter == 0:
        print('No matching code found')
        sys.exit(1)
    if center == -1:
        print('No matching code found')
        sys.exit(1)

    # Find start: scan backwards up to 10 source lines or 20 asm lines.
    start = center
    codelines = binarylines = 0
    while start > 1:
        start -= 1
        if re.match(r'^([a-f0-9]+):', lines[start]):
            binarylines += 1
        else:
            codelines += 1
        if codelines > 10 or binarylines > 20:
            break

    # Find finish: scan forwards.
    finish = center
    codelines = binarylines = 0
    while finish < counter:
        finish += 1
        line = '' if finish >= counter else lines[finish]
        if re.match(r'^([a-f0-9]+):', line):
            binarylines += 1
        else:
            codelines += 1
        if codelines > 10 or binarylines > 20:
            break

    # Annotate registers backwards from the faulting instruction.
    reglines = [None] * len(lines)
    i = center
    while i >= start:
        reglines[i] = process_x86_regs(lines[i], center - i)
        i -= 1

    # Print annotated disassembly.
    i = start
    while i < finish:
        if i == center:
            prefix = f'*{lines[i]} '
        else:
            prefix = f' {lines[i]} '
        out_line = prefix
        if reglines[i] and len(reglines[i]) > 0:
            pad = 60 - len(prefix)
            out_line += ' ' * max(0, pad) + f'| {reglines[i]}'
        if i == center:
            out_line += '<--- faulting instruction'
        print(out_line)
        i += 1

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))