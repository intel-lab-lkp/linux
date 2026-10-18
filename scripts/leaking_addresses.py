#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# (c) 2017 Tobin C. Harding <me@tobin.cc>
#
# leaking_addresses.py: Scan the kernel for potential leaking addresses.
#
# Use --debug to output path before parsing.

import argparse
import os
import platform
import re
import signal
import subprocess
import sys

DIRS = ('/proc', '/sys')
TIMEOUT = 10

SKIP_ABS = {
    '/proc/kmsg',
    '/proc/device-tree',
    '/proc/1/syscall',
    '/sys/firmware/devicetree',
    '/sys/kernel/tracing/trace_pipe',
    '/sys/kernel/debug/tracing/trace_pipe',
    '/sys/kernel/security/apparmor/revision',
}

SKIP_ANY = {
    'pagemap', 'events', 'access', 'registers', 'snapshot_raw',
    'trace_pipe_raw', 'ptmx', 'trace_pipe', 'fd', 'usbmon',
}

RE_ARCH_X86 = re.compile(r'i[3456]86')

# Active per-arch state
_ARCH = platform.machine() or ''
_OPT_32BIT = False
_PAGE_OFFSET_32BIT = 0
_PGTABLE_LEVELS = None
_KALLSYMS = []
_KERNEL_CONFIG_FILE = ''
_PAGE_OFFSET_CACHE = None


def dprint(*args):
    if _DEBUG:
        print(*args, file=sys.stderr)


def is_x86_64():
    return _ARCH == 'x86_64'


def is_ppc64():
    return _ARCH == 'ppc64'


def is_ix86_32():
    return bool(RE_ARCH_X86.match(_ARCH))


def is_32bit():
    return _OPT_32BIT or bool(_PAGE_OFFSET_32BIT) or is_ix86_32()


def get_kernel_config_option(option):
    files = []
    if _KERNEL_CONFIG_FILE:
        files = [_KERNEL_CONFIG_FILE]
    elif os.access('/proc/config.gz', os.R_OK):
        try:
            import gzip
            with gzip.open('/proc/config.gz', 'rt') as f:
                for line in f:
                    if line.startswith(option):
                        return line.split('=', 1)[1].strip()
        except OSError as e:
            dprint(f'failed reading /proc/config.gz: {e}')
        return ''
    else:
        rel = subprocess.check_output(['uname', '-r'], text=True).strip()
        files = [f'/boot/config-{rel}', '/boot/config']

    for path in files:
        try:
            with open(path) as f:
                for line in f:
                    if line.startswith(option):
                        return line.split('=', 1)[1].strip()
        except OSError:
            continue
    return ''


def get_page_offset():
    global _PAGE_OFFSET_CACHE
    if _PAGE_OFFSET_CACHE is not None:
        return _PAGE_OFFSET_CACHE
    default = 0xc0000000
    if _PAGE_OFFSET_32BIT:
        _PAGE_OFFSET_CACHE = _PAGE_OFFSET_32BIT
        return _PAGE_OFFSET_CACHE
    val = get_kernel_config_option('CONFIG_PAGE_OFFSET')
    if val:
        _PAGE_OFFSET_CACHE = int(val, 16)
        return _PAGE_OFFSET_CACHE
    _PAGE_OFFSET_CACHE = default
    return default


def is_false_positive_64(match):
    if re.search(r'\b(0x)?(f|F){15}[0-9a-f]\b', match) or \
       re.search(r'\b(0x)?0{16}\b', match):
        return True
    if is_x86_64() and _in_vsyscall(match):
        return True
    return False


def is_false_positive_32(match):
    if re.search(r'\b(0x)?(f|F){7}[0-9a-f]\b', match):
        return True
    if int(match, 16) < get_page_offset():
        return True
    return False


def is_false_positive(match):
    if is_32bit():
        return is_false_positive_32(match)
    return is_false_positive_64(match)


def _in_vsyscall(match):
    v = int(match, 16)
    return 0xffffffffff600000 <= v <= 0xffffffffff601000


def get_address_re():
    if is_ppc64():
        return re.compile(r'\b(0x)?[89abcdef]00[[:xdigit:]]{13}\b'
                          .replace('[[:xdigit:]]', r'[0-9a-fA-F]'))
    if is_32bit():
        return re.compile(r'\b(0x)?[0-9a-fA-F]{8}\b')
    global _PGTABLE_LEVELS
    if _PGTABLE_LEVELS is None:
        val = get_kernel_config_option('CONFIG_PGTABLE_LEVELS')
        _PGTABLE_LEVELS = int(val) if val else 0
    if _PGTABLE_LEVELS == 5:
        return re.compile(r'\b(0x)?ff[0-9a-fA-F]{14}\b')
    return re.compile(r'\b(0x)?ffff[0-9a-fA-F]{12}\b')


def may_leak_address(path, line):
    if re.match(r'^SigBlk:', line) or re.match(r'^SigIgn:', line) \
            or re.match(r'^SigCgt:', line):
        return False
    if re.search(r'\bKEY=[0-9a-fA-F]{9,14} [0-9a-fA-F]{16} '
                 r'[0-9a-fA-F]{16}\b', line):
        return False
    if path.endswith('key') and re.search(
            r'\b[0-9a-fA-F]{9,14} [0-9a-fA-F]{16} [0-9a-fA-F]{16}\b',
            line):
        return False
    pattern = get_address_re()
    for m in pattern.finditer(line):
        if not is_false_positive(m.group(0)):
            return True
    return False


def parse_dmesg():
    try:
        out = subprocess.check_output(['dmesg'], text=True,
                                      stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, FileNotFoundError):
        return
    for line in out.splitlines(keepends=True):
        if may_leak_address('dmesg', line):
            sys.stdout.write('dmesg: ' + line)


def skip(path):
    if path in SKIP_ABS:
        return True
    return os.path.basename(path) in SKIP_ANY


def parse_binary(path):
    try:
        with open(path, 'rb') as f:
            data = f.read()
    except OSError:
        return
    for addr, symbol in _KALLSYMS:
        off = data.find(addr)
        if off != -1:
            print(f"{path}: {symbol} @ {off}")


def parse_file(path):
    if not os.access(path, os.R_OK):
        return
    # Skip pseudo-binaries
    if path.startswith(('/sys/kernel/btf/', '/sys/devices/pci',
                        '/sys/firmware/efi/efivars/', '/proc/bus/pci/')):
        return
    try:
        with open(path, 'r', errors='replace') as f:
            for line in f:
                line = line.rstrip('\n')
                if may_leak_address(path, line):
                    print(f"{path}: {line}")
    except (OSError, UnicodeDecodeError):
        if _KALLSYMS:
            parse_binary(path)


def check_path_for_leaks(path):
    if may_leak_address(path, path):
        print(f"Path name may contain address: {path}")


def timed_parse_file(path):
    def handler(signum, frame):
        raise TimeoutError()
    old = signal.signal(signal.SIGALRM, handler)
    signal.alarm(TIMEOUT)
    try:
        parse_file(path)
    except TimeoutError:
        print(f"timed out parsing: {path}", file=sys.stderr)
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, old)


def walk(dirs):
    stack = list(dirs)
    while stack:
        pwd = stack.pop()
        try:
            entries = os.listdir(pwd)
        except OSError:
            continue
        for name in entries:
            if name in ('.', '..'):
                continue
            path = os.path.join(pwd, name)
            if os.path.islink(path):
                continue
            # Skip /proc/PID except /proc/1
            if re.match(r'^/proc/\d+$', path) and path != '/proc/1':
                continue
            if skip(path):
                continue
            check_path_for_leaks(path)
            if os.path.isdir(path):
                stack.append(path)
                continue
            dprint(f'parsing: {path}')
            timed_parse_file(path)


def format_output(path, raw, squash_path, squash_filename, suppress_dmesg):
    if raw or not (squash_path or squash_filename):
        _dump_raw(path, suppress_dmesg)
        return
    total, dmesg, paths, files = _parse_raw(path)
    print(f"\nTotal number of results from scan (incl dmesg): {total}")
    if not suppress_dmesg:
        _print_dmesg(dmesg)
    if squash_filename:
        _squash_by(files, 'filename')
    if squash_path:
        _squash_by(paths, 'path')


def _dump_raw(path, suppress_dmesg):
    try:
        with open(path) as f:
            for line in f:
                if suppress_dmesg and line.startswith('dmesg:'):
                    continue
                sys.stdout.write(line)
    except OSError as e:
        sys.exit(f"{path}: {e}")


def _parse_raw(path):
    total = 0
    dmesg = []
    paths = {}
    files = {}
    try:
        with open(path) as f:
            for line in f:
                total += 1
                if line.startswith('dmesg:'):
                    dmesg.append(line)
                    continue
                idx = line.find(': ')
                if idx < 0:
                    continue
                p = line[:idx]
                v = line[idx + 2:]
                paths.setdefault(p, []).append(v)
                files.setdefault(os.path.basename(p), []).append(v)
    except OSError as e:
        sys.exit(f"{path}: {e}")
    return total, dmesg, paths, files


def _print_dmesg(dmesg):
    print("\ndmesg output:")
    if not dmesg:
        print('<no results>')
        return
    for line in dmesg:
        idx = line.find(': ')
        sys.stdout.write(line[idx + 2:] if idx >= 0 else line)


def _squash_by(ref, desc):
    print(f"\nResults squashed by {desc} (excl dmesg). "
          f"Displaying [<number of results> <{desc}>], <example result>")
    if not ref:
        print('<no results>')
        return
    for key in ref:
        vals = ref[key]
        print(f"[{len(vals)} {key}] {vals[0]}")


def _load_kallsyms(path):
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            addr_text = parts[0]
            if addr_text.startswith('0'):
                continue
            try:
                addr = int(addr_text, 16)
            except ValueError:
                continue
            _KALLSYMS.append((addr.to_bytes(8, 'little'), parts[2]))


def main(argv):
    global _OPT_32BIT, _PAGE_OFFSET_32BIT, _KERNEL_CONFIG_FILE, _DEBUG
    p = argparse.ArgumentParser(add_help=False)
    p.add_argument('-d', '--debug', action='store_true')
    p.add_argument('-h', '--help', action='store_true')
    p.add_argument('-o', '--output-raw')
    p.add_argument('-i', '--input-raw')
    p.add_argument('--suppress-dmesg', action='store_true')
    p.add_argument('--squash-by-path', action='store_true')
    p.add_argument('--squash-by-filename', action='store_true')
    p.add_argument('--raw', action='store_true')
    p.add_argument('--kallsyms')
    p.add_argument('--kernel-config-file')
    p.add_argument('--32-bit', action='store_true')
    p.add_argument('--page-offset-32-bit', type=lambda v: int(v, 0),
                   default=0)
    args = p.parse_args(argv)

    if args.help:
        print(f"Usage: {sys.argv[0]} [OPTIONS]")
        print()
        print("Options:")
        print("  -o, --output-raw=<file>     Save results for future processing.")
        print("  -i, --input-raw=<file>      Read results from file instead of scanning.")
        print("        --raw                 Show raw results (default).")
        print("        --suppress-dmesg      Do not show dmesg results.")
        print("        --squash-by-path      Show one result per unique path.")
        print("        --squash-by-filename  Show one result per unique filename.")
        print("        --kernel-config-file=<file>  Kernel configuration file (e.g /boot/config).")
        print("        --kallsyms=<file>     Read kernel symbol addresses from file.")
        print("        --32-bit              Scan 32-bit kernel.")
        print("        --page-offset-32-bit=o  Page offset (for 32-bit kernel).")
        print("  -d, --debug                 Display debugging output.")
        print("  -h, --help                  Display this help and exit.")
        print()
        print("Scans the running kernel for potential leaking addresses.")
        return 0
    _DEBUG = args.debug
    _OPT_32BIT = args.__dict__['32_bit']
    _PAGE_OFFSET_32BIT = args.page_offset_32_bit
    if args.kernel_config_file:
        _KERNEL_CONFIG_FILE = args.kernel_config_file

    if args.input_raw:
        format_output(args.input_raw, args.raw, args.squash_by_path,
                      args.squash_by_filename, args.suppress_dmesg)
        return 0

    if not (is_x86_64() or is_ppc64() or is_ix86_32()
            or _OPT_32BIT or _PAGE_OFFSET_32BIT):
        print("\nScript does not support your architecture, sorry.\n")
        print("Currently we support: x86_64, ppc64, x86\n")
        return 129

    out_fd = None
    if args.output_raw:
        try:
            out_fd = open(args.output_raw, 'w')
        except OSError as e:
            sys.exit(f"{args.output_raw}: {e}")
        # Redirect print() calls
        sys.stdout = out_fd

    if args.kallsyms:
        _load_kallsyms(args.kallsyms)

    parse_dmesg()
    walk(DIRS)

    if out_fd:
        out_fd.close()
        sys.stdout = sys.__stdout__
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))