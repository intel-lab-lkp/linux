#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Check the stack usage of functions.
# Usage: objdump -d vmlinux | checkstack.py [arch] [min_stack]

import platform
import re
import sys

X = '[0-9a-f]'
RE_FUNC = re.compile(rf'^({X}*) <(.*)>:$')
RE_FILE = re.compile(r'(.*):\s*file format')


def arm_push_handler(line):
    if re.search(r'.*push.*fp, ip, lr, pc}', line):
        return (line.count(',') + 1) * 4
    return 0


def config_for(arch):
    """Return (main_re, dynamic_re, sub_fn)."""
    if re.match(r'^(?:aarch|arm)64$', arch):
        return (
            re.compile(rf'^.*stp.*sp, ?#-([0-9]{{1,8}})\]!'),
            re.compile(rf'^.*sub.*sp, sp, #(0x{X}{{1,8}})'),
            None)
    if arch == 'arm':
        return (re.compile(r'.*sub.*sp, sp, #([0-9]{1,4})'), None,
                arm_push_handler)
    if re.match(r'^x86(?:_64)?$', arch) or re.match(r'^i[3456]86$', arch):
        return (
            re.compile(rf'^.*[as][du][db]    \$(0x{X}{{1,8}}),%(?:e|r)sp$'),
            re.compile(r'^.*[as][du][db]    (%.*),%(?:e|r)sp$'),
            None)
    if arch == 'm68k':
        return (re.compile(r'.*(?:linkw %fp,|addaw )#-([0-9]{1,4})'
                           r'(?:,%sp)?$'), None, None)
    if arch == 'mips64':
        return (re.compile(r'.*daddiu.*sp,sp,-([0-9]{1,8})'), None, None)
    if arch == 'mips':
        return (re.compile(r'.*addiu.*sp,sp,-([0-9]{1,8})'), None, None)
    if arch == 'nios2':
        return (re.compile(r'.*addi.*sp,sp,-([0-9]{1,8})'), None, None)
    if arch == 'openrisc':
        return (re.compile(r'.*l\.addi.*r1,r1,-([0-9]{1,8})'), None, None)
    if arch in ('parisc', 'parisc64'):
        return (re.compile(rf'.*ldo ({X}{{1,8}})\(sp\),sp'), None, None)
    if arch == 'powerpc' or re.match(r'^ppc(?:64)?(?:le)?$', arch):
        return (re.compile(rf'.*st[dw]u.*r1,-({X}{{1,8}})\(r1\)'),
                None, None)
    if re.match(r'^s390x?$', arch):
        return (re.compile(r'.*(?:lay|ag?hi).*\%r15,-([0-9]+)'
                           r'(?:\(\%r15\))?$'), None, None)
    if arch in ('sparc', 'sparc64'):
        return (re.compile(r'.*save.*%sp, -([0-9]{1,8}), %sp'), None, None)
    if re.match(r'^riscv(?:64)?$', arch):
        return (re.compile(r'.*addi.*sp,sp,-([0-9]{1,8})'), None, None)
    if re.match(r'^loongarch(?:32|64)?$', arch):
        return (re.compile(r'.*addi\..*sp, .*sp, -([0-9]{1,8}).*'),
                None, None)
    print(f'wrong or unknown architecture "{arch}"')
    sys.exit(1)


def parse_size(s):
    return int(s, 16) if s.startswith('0x') else int(s)


def main(argv):
    arch = argv[0] if argv else platform.machine()
    min_stack = int(argv[1]) if len(argv) > 1 and argv[1].isdigit() else 512

    main_re, dyn_re, sub_fn = config_for(arch)

    stack = []
    func = None
    file = ''
    intro = ''
    total = 0

    for raw in sys.stdin:
        line = raw.rstrip('\n')

        m = RE_FUNC.match(line)
        if m:
            func = m.group(2)
            if total > min_stack:
                stack.append(f'{intro}{total}\n')
            addr = m.group(1)
            intro = f'0x{addr} {func} [{file}]:'
            pad = 56 - len(intro)
            while pad > 0:
                intro += '\t'
                pad -= 8
            total = 0
            continue

        m = RE_FILE.match(line)
        if m:
            file = m.group(1).replace('.ko', '')
            idx = file.rfind('/')
            if idx != -1:
                file = file[idx + 1:]
            continue

        m = main_re.search(line)
        if m:
            size = parse_size(m.group(1))
            if size > 0xf0000000:
                size = -size + 0x80000000 + 0x80000000
            if size > 0x10000000:
                continue
            total += size
            continue

        if dyn_re is not None:
            m = dyn_re.search(line)
            if m:
                total += parse_size(m.group(1))
                continue

        if sub_fn is not None:
            total += sub_fn(line)

    if total > min_stack:
        stack.append(f'{intro}{total}\n')

    def key(entry):
        m = re.search(r':\t*(\d+)$', entry)
        size = int(m.group(1)) if m else 0
        m = re.search(r' (.*):', entry)
        fname = m.group(1) if m else ''
        return (-size, fname)

    for entry in sorted(stack, key=key):
        sys.stdout.write(entry)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))