#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# (c) 2008, Steven Rostedt <srostedt@redhat.com>
#
# recordmcount.py - makes a section __mcount_loc holding all offsets
# to calls to mcount. See recordmcount.pl header for detailed design.
#
# usage: recordmcount.py arch endian bits objdump objcopy cc ld nm rm mv \
#                       is_module inputfile

import os
import re
import subprocess
import sys

TEXT_SECTIONS = {
    '.text', '.init.text', '.ref.text', '.sched.text',
    '.spinlock.text', '.irqentry.text', '.softirqentry.text',
    '.kprobes.text', '.cpuidle.text', '.text.unlikely',
}
TEXT_SECTION_PREFIXES = ('.text.',)

MCOUNT_SECTION = '__mcount_loc'


class Config:
    """Per-architecture parsing configuration."""

    def __init__(self):
        self.local_re   = re.compile(r'^[0-9a-fA-F]+\s+t\s+(\S+)')
        self.weak_re    = re.compile(r'^[0-9a-fA-F]+\s+([wW])\s+(\S+)')
        self.section_re = re.compile(r'Disassembly of section\s+(\S+):')
        self.func_re    = re.compile(r'^([0-9a-fA-F]+)\s+<([^^]*?)>:')
        self.mcount_re  = re.compile(
            r'^\s*([0-9a-fA-F]+):.*\s(mcount|__fentry__)$')
        self.section_type   = '@progbits'
        self.mcount_adjust  = 0
        self.alignment      = None
        self.type           = '.long'
        self.ld_flags       = ''
        self.objdump_flags  = ''
        self.objcopy_flags  = ''
        self.cc_flags       = ''


def config_for(arch, endian, bits, cc):
    cfg = Config()
    if arch in ('x86_64', 'i386', 'x86'):
        arch = 'x86_64' if bits == 64 else 'i386'
    if arch == 'x86_64':
        cfg.mcount_re = re.compile(
            r'^\s*([0-9a-fA-F]+):.*\s(mcount|__fentry__)'
            r'([+-]0x[0-9a-zA-Z]+)?$')
        cfg.type = '.quad'
        cfg.alignment = 8
        cfg.mcount_adjust = -1
        cfg.ld_flags      = '-m elf_x86_64'
        cfg.objdump_flags = '-M x86-64'
        cfg.objcopy_flags = '-O elf64-x86-64'
        cfg.cc_flags      = '-m64'
    elif arch == 'i386':
        cfg.alignment = 4
        cfg.mcount_adjust = -1
        cfg.ld_flags      = '-m elf_i386'
        cfg.objdump_flags = '-M i386'
        cfg.objcopy_flags = '-O elf32-i386'
        cfg.cc_flags      = '-m32'
    elif arch == 's390' and bits == 64:
        if '-DCC_USING_HOTPATCH' in cc:
            cfg.mcount_re = re.compile(
                r'^\s*([0-9a-fA-F]+):\s*c0 04 00 00 00 00\s*'
                r'(brcl\s*0,|jgnop\s*)[0-9a-f]+ <([^\+]*)>$')
            cfg.mcount_adjust = 0
        cfg.alignment = 8
        cfg.type = '.quad'
        cfg.ld_flags = '-m elf64_s390'
        cfg.cc_flags = '-m64'
    elif arch == 'sh':
        cfg.alignment = 2
        cfg.ld_flags = '-m shlelf_linux'
        cfg.objcopy_flags = '-O elf32-shbig-linux' if endian == 'big' \
            else '-O elf32-sh-linux'
    elif arch == 'powerpc':
        cfg.local_re = re.compile(r'^[0-9a-fA-F]+\s+t\s+(\.?\S+)')
        cfg.func_re  = re.compile(r'^([0-9a-fA-F]+)\s+<(\.?\w*?)>:')
        cfg.mcount_re = re.compile(
            r'^\s*([0-9a-fA-F]+):.*\s\.?_mcount$')
        emu = 'ppc'
        if endian == 'big':
            cfg.cc_flags      = '-mbig-endian'
            cfg.ld_flags      = '-EB'
        else:
            cfg.cc_flags      = '-mlittle-endian'
            cfg.ld_flags      = '-EL'
            emu = 'lppc'
        if bits == 64:
            cfg.type = '.quad'
            cfg.cc_flags += ' -m64'
            cfg.ld_flags += f' -m elf64{emu}'
        else:
            cfg.cc_flags += ' -m32'
            cfg.ld_flags += f' -m elf32{emu}'
    elif arch == 'arm':
        cfg.alignment = 2
        cfg.section_type = '%progbits'
        cfg.mcount_re = re.compile(
            r'^\s*([0-9a-fA-F]+):\s*R_ARM_(CALL|PC24|THM_CALL)'
            r'\s+(__gnu_mcount_nc|mcount)$')
    elif arch == 'arm64':
        cfg.alignment = 3
        cfg.section_type = '%progbits'
        cfg.mcount_re = re.compile(
            r'^\s*([0-9a-fA-F]+):\s*R_AARCH64_CALL26\s+_mcount$')
        cfg.type = '.quad'
    elif arch == 'sparc64':
        cfg.func_re = re.compile(r'^([0-9a-fA-F]+)\s+<(\w*?)>:')
        cfg.mcount_re = re.compile(r'^\s*([0-9a-fA-F]+):.*\s_mcount$')
        cfg.alignment = 8
        cfg.type = '.xword'
        cfg.ld_flags      = '-m elf64_sparc'
        cfg.cc_flags      = '-m64'
        cfg.objcopy_flags = '-O elf64-sparc'
    elif arch == 'mips':
        if_bits = bits
        cfg.mcount_re = re.compile(
            r'^\s*([0-9a-fA-F]+): R_MIPS_26\s+_mcount$')
        cfg.objdump_flags = f'-Melf-trad{endian}mips'
        if endian == 'big':
            cfg.ld_flags = f'-melf{bits}btsmip -EB'
        else:
            cfg.ld_flags = f'-melf{bits}ltsmip -EL'
        cfg.cc_flags = f'-mno-abicalls -fno-pic -mabi={bits}'
        if bits == 64:
            cfg.func_re = re.compile(
                r'^([0-9a-fA-F]+)\s+<(.|[^\$]L.*?|\$[^L].*?'
                r'|[^\$][^L].*?)>:')
            cfg.type = '.dword'
    elif arch == 'microblaze':
        cfg.mcount_re = re.compile(r'^\s*([0-9a-fA-F]+):.*\s_mcount$')
    elif arch == 'riscv':
        cfg.func_re = re.compile(
            r'^([0-9a-fA-F]+)\s+<([^.0-9][0-9a-zA-Z_\.]+)>:')
        cfg.mcount_re = re.compile(
            r'^\s*([0-9a-fA-F]+):\sR_RISCV_CALL(_PLT)?\s_mcount$')
        cfg.type = '.quad'
        cfg.alignment = 2
    elif arch == 'csky':
        cfg.mcount_re = re.compile(
            r'^\s*([0-9a-fA-F]+):\s*R_CKCORE_PCREL_JSR_IMM26BY2\s+'
            r'_mcount$')
        cfg.alignment = 2
    else:
        sys.exit(f"Arch {arch} is not supported with "
                 f"CONFIG_DYNAMIC_FTRACE")
    return cfg, arch


def run(cmd):
    try:
        return subprocess.check_output(cmd, text=True,
                                        stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return ''


def main(argv):
    if len(argv) != 12:
        print("usage: recordmcount.py arch endian bits objdump objcopy cc "
              "ld nm rm mv is_module inputfile", file=sys.stderr)
        sys.exit(1)

    (arch, endian, bits_s, objdump, objcopy, cc,
     ld, nm, rm, mv, is_module, inputfile) = argv
    bits = int(bits_s)

    if re.search(r'kernel/trace/ftrace\.o$', inputfile):
        sys.exit(0)

    if not objdump:  objdump = 'objdump'
    if not objcopy:  objcopy = 'objcopy'
    if not cc:       cc = 'gcc'
    if not ld:       ld = 'ld'
    if not nm:       nm = 'nm'
    if not rm:       rm = 'rm'
    if not mv:       mv = 'mv'

    cfg, arch = config_for(arch, endian, bits, cc)

    dirname, filename = os.path.split(inputfile)
    dirname = dirname or '.'
    base, ext = os.path.splitext(filename)
    mcount_s = os.path.join(dirname, f'.tmp_mc_{base}.s')
    mcount_o = os.path.join(dirname, f'.tmp_mc_{base}.o')

    # Step 1: collect locals and weaks
    locals_set = set()
    weak = {}
    nm_out = run([nm, inputfile])
    for line in nm_out.splitlines():
        m = cfg.local_re.match(line)
        if m:
            locals_set.add(m.group(1))
            continue
        m = cfg.weak_re.match(line)
        if m:
            weak[m.group(2)] = m.group(1)

    offsets = []
    ref_func = None
    offset = 0
    state = {'opened': False, 'fh': None}

    def open_out():
        if state['opened']:
            return
        state['fh'] = open(mcount_s, 'w')
        state['fh'].write(f'\t.section {MCOUNT_SECTION},"a",'
                          f'{cfg.section_type}\n')
        if cfg.alignment is not None:
            state['fh'].write(f'\t.align {cfg.alignment}\n')
        state['opened'] = True

    def update_funcs():
        if not ref_func or not offsets:
            return
        if ref_func in weak:
            sys.exit(f"{inputfile}: ERROR: referencing weak function "
                     f"{ref_func} for mcount")
        if ref_func in locals_set:
            convert_set.add(ref_func)
        open_out()
        for cur in offsets:
            state['fh'].write(f"\t{cfg.type} {ref_func} + "
                              f"{cur - offset}\n")

    convert_set = set()

    # Step 2: parse objdump output
    objdump_cmd = ['env', 'LC_ALL=C', objdump, '-hdr', inputfile]
    if cfg.objdump_flags:
        objdump_cmd[3:3] = cfg.objdump_flags.split()
    try:
        proc = subprocess.Popen(objdump_cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.DEVNULL, text=True)
    except FileNotFoundError:
        sys.exit(f"error running {objdump}")

    read_headers = True
    read_function = False
    text_found = False
    text = ''

    for raw in proc.stdout:
        line = raw.rstrip('\n')

        if read_headers and MCOUNT_SECTION in line:
            print(f"ERROR: {MCOUNT_SECTION} already in {inputfile}\n"
                  f"\tDelete {inputfile} and try again.",
                  file=sys.stderr)
            sys.exit(-1)

        m = cfg.section_re.search(line)
        if m:
            read_headers = False
            sec = m.group(1)
            read_function = sec in TEXT_SECTIONS or any(
                sec.startswith(p) for p in TEXT_SECTION_PREFIXES)
            update_funcs()
            text_found = False
            ref_func = None
            offsets.clear()
            continue

        if read_function:
            m = cfg.func_re.match(line)
            if m:
                text_found = True
                text = m.group(2)
                if text not in locals_set and text not in weak:
                    ref_func = text
                    read_function = False
                    offset = int(m.group(1), 16)
                elif ref_func is None and text not in weak \
                        and not text.startswith('.L'):
                    ref_func = text
                    offset = int(m.group(1), 16)

        if text_found:
            m = cfg.mcount_re.search(line)
            if m:
                offsets.append(int(m.group(1), 16) + cfg.mcount_adjust)

    proc.wait()
    update_funcs()

    if not state['opened']:
        sys.exit(0)
    state['fh'].close()

    # Step 3: assemble
    subprocess.call([cc] + cfg.cc_flags.split() +
                    ['-o', mcount_o, '-c', mcount_s])

    if convert_set:
        glob = ' '.join(f'--globalize-symbol {s}' for s in convert_set)
        loc = ' '.join(f'--localize-symbol {s}' for s in convert_set)
        globalobj = os.path.join(dirname, f'.tmp_gl_{filename}')
        globalmix = os.path.join(dirname, f'.tmp_mx_{filename}')
        subprocess.call([objcopy] + cfg.objcopy_flags.split() +
                        glob.split() + [inputfile, globalobj])
        subprocess.call([ld] + cfg.ld_flags.split() +
                        ['-r', globalobj, mcount_o, '-o', globalmix])
        subprocess.call([objcopy] + cfg.objcopy_flags.split() +
                        loc.split() + [globalmix, inputfile])
        for p in (globalobj, globalmix):
            try:
                os.unlink(p)
            except OSError:
                pass
    else:
        mix = os.path.join(dirname, f'.tmp_mx_{filename}')
        subprocess.call([ld] + cfg.ld_flags.split() +
                        ['-r', inputfile, mcount_o, '-o', mix])
        os.replace(mix, inputfile)

    for p in (mcount_o, mcount_s):
        try:
            os.unlink(p)
        except OSError:
            pass
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))