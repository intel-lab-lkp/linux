#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Copyright 2005-2009 - Steven Rostedt
#
# Simplify the kernel config to only the modules loaded on this system.

import os
import re
import subprocess
import sys

DEBUG = 'LOCALMODCONFIG_DEBUG' in os.environ

# Module state populated by read_kconfig()
depends = {}
selects = {}
prompts = {}
objects = {}
config2kfile = {}
defaults = {}
read_kconfigs = {}
ksource = '.'


def dprint(*args):
    if DEBUG:
        print(*args, file=sys.stderr)


def read_config():
    uname = subprocess.check_output(['uname', '-r'], text=True).strip()

    searchconfigs = [
        ('.config', 'cat', None),
        ('/proc/config.gz', 'zcat', None),
        (f'/boot/config-{uname}', 'cat', None),
        (f'/boot/vmlinuz-{uname}', 'scripts/extract-ikconfig',
         'scripts/extract-ikconfig'),
        ('vmlinux', 'scripts/extract-ikconfig', 'scripts/extract-ikconfig'),
        (f'/lib/modules/{uname}/kernel/kernel/configs.ko',
         'scripts/extract-ikconfig', 'scripts/extract-ikconfig'),
        ('kernel/configs.ko', 'scripts/extract-ikconfig',
         'scripts/extract-ikconfig'),
        ('kernel/configs.o', 'scripts/extract-ikconfig',
         'scripts/extract-ikconfig'),
    ]

    for file, exec_cmd, test_cmd in searchconfigs:
        if not os.path.isfile(file):
            continue
        if test_cmd is not None:
            r = subprocess.run([test_cmd, file], capture_output=True)
            if r.returncode:
                continue
        print(f"using config: '{file}'", file=sys.stderr)
        try:
            out = subprocess.check_output(
                f'{exec_cmd} {file}', shell=True, text=True,
                stderr=subprocess.DEVNULL)
        except subprocess.CalledProcessError:
            continue
        return out.splitlines(keepends=True)

    sys.exit('No config file found')


def read_kconfig(kconfig, if_state):
    """Recursively read a Kconfig file, populate globals.

    if_state is a [level, [deps...]] list for mutation.
    """
    state = 'NONE'
    config = ''
    cont = False
    line = ''

    source = f'{ksource}/{kconfig}'
    last_source = ''
    while re.search(r'\$\((\w+)\)', source) and source != last_source:
        last_source = source
        m = re.search(r'\$\((\w+)\)', source)
        source = source.replace(m.group(0), os.environ.get(m.group(1), ''))

    try:
        with open(source) as f:
            raw_lines = f.readlines()
    except OSError:
        sys.exit(f"Can't open {source}")

    iflevel = if_state[0]
    ifdeps = if_state[1]

    for raw in raw_lines:
        line_text = raw.rstrip('\n')

        if cont:
            line_text = line + ' ' + line_text

        if line_text.endswith('\\'):
            cont = True
            line = line_text[:-1]
            continue
        cont = False

        m = re.match(r'^source\s+"?([^"]+)', line_text)
        if m:
            sub = m.group(1)
            if sub not in read_kconfigs:
                read_kconfigs[sub] = 1
                read_kconfig(sub, if_state)
            continue

        m = re.match(r'^\s*(?:menu)?config\s+(\S+)\s*$', line_text)
        if m:
            state = 'NEW'
            config = m.group(1)
            config2kfile[f'CONFIG_{config}'] = kconfig
            for i in range(if_state[0]):
                if i:
                    depends[config] = depends.get(config, '') + ' ' + ifdeps[i]
                else:
                    depends[config] = ifdeps[i]
                state = 'DEP'
            continue

        m = re.match(r'^\s*depends\s+on\s+(.*)$', line_text)
        if m and state in ('NEW', 'DEP'):
            if state == 'NEW':
                depends[config] = m.group(1)
                state = 'DEP'
            else:
                depends[config] = depends.get(config, '') + ' ' + m.group(1)
            continue

        m = re.match(r'^\s*def(?:_(?:bool|tristate)|ault)\s+(\S.*)$',
                     line_text)
        if m and state != 'NONE':
            dep = m.group(1)
            defaults[config] = 1
            if not re.match(r'^\s*(?:y|m|n)\s*$', dep):
                dep = re.sub(r'.*\sif\s+', '', dep)
                depends[config] = depends.get(config, '') + ' ' + dep
                dprint(f'Added default depends {dep} to {config}')
            continue

        m = re.match(r'^\s*select\s+(\S+)', line_text)
        if m and state != 'NONE':
            conf = m.group(1)
            if conf in selects:
                selects[conf] += ' ' + config
            else:
                selects[conf] = config
            continue

        if state != 'NONE' and \
                re.match(r'^\s*(?:tristate\s+\S|prompt\b)', line_text):
            prompts[config] = 1
            continue

        m = re.match(r'^if\s+(.*\S)\s*$', line_text)
        if m:
            deps_str = re.sub(r'^[^a-zA-Z0-9_]*', '', m.group(1))
            deps_str = re.sub(r'[^a-zA-Z0-9_]*$', '', deps_str)
            parts = re.split(r'[^a-zA-Z0-9_]+', deps_str)
            if if_state[0] < len(ifdeps):
                ifdeps[if_state[0]] = ':'.join(parts)
            else:
                ifdeps.append(':'.join(parts))
            if_state[0] += 1
            continue

        if re.match(r'^endif', line_text):
            if if_state[0]:
                if_state[0] -= 1
            continue

        if re.match(r'^\s*(?:---)?help(?:---)?\s*$', line_text) or \
                re.match(r'^(?:comment|choice|menu)\b', line_text):
            state = 'NONE'


def convert_vars(line, make_vars):
    """Expand $(VAR) references in a Makefile line."""
    out = ''
    while True:
        m = re.match(r'^(.*?)(\$\(([^()]+)\))(.*)$', line)
        if not m:
            out += line
            break
        out += m.group(1)
        var = m.group(3)
        if var in make_vars:
            out += make_vars[var]
        else:
            out += m.group(2)
        line = m.group(4)
    return out


def main(argv):
    global ksource

    config_file = read_config()

    localmodconfig = False
    localyesconfig = False
    args = []
    for a in argv:
        if a == '--localmodconfig':
            localmodconfig = True
        elif a == '--localyesconfig':
            localyesconfig = True
        else:
            args.append(a)

    ksource = args[0] if args else '.'
    kconfig = args[1] if len(args) > 1 else None
    lsmod_file = os.environ.get('LSMOD')

    try:
        makefiles = subprocess.check_output(
            ['find', ksource, '-name', 'Makefile', '-or', '-name', 'Kbuild'],
            text=True, stderr=subprocess.DEVNULL).splitlines()
    except subprocess.CalledProcessError:
        makefiles = []

    if kconfig:
        read_kconfig(kconfig, [0, []])

    # Parse Makefiles for obj-$(CONFIG_FOO) += obj.o
    for makefile in makefiles:
        line = ''
        make_vars = {}
        try:
            with open(makefile) as f:
                for raw in f:
                    line_text = raw.rstrip('\n')
                    if line_text.endswith('\\'):
                        line += line_text[:-1]
                        continue
                    line += line_text
                    expanded = convert_vars(line, make_vars)
                    line = ''

                    m = re.match(
                        r'obj-\$[({](CONFIG_[^})]*)[)}]\s*[+:]?=\s*(.*)',
                        expanded)
                    objs = None
                    var = None
                    if m:
                        var = m.group(1)
                        objs = m.group(2)
                    else:
                        m = re.match(r'^\s*(\S+)\s*[:]?=\s*(.*\S)',
                                     expanded)
                        if m:
                            make_vars[m.group(1)] = m.group(2)
                    if objs is not None:
                        for o in re.split(r'\s+', objs):
                            o = o.replace('-', '_')
                            m = re.match(r'(.*)\.o$', o)
                            if m:
                                objects.setdefault(m.group(1), []).append(var)
        except OSError:
            continue

    # Get loaded modules
    modules = {}
    if lsmod_file:
        if not os.path.isfile(lsmod_file):
            alt = os.path.join(os.environ.get('objtree', ''), lsmod_file)
            if os.path.isfile(alt):
                lsmod_file = alt
            else:
                sys.exit(f'{lsmod_file} not found')
        with open(lsmod_file) as f:
            lines = f.readlines()
    else:
        lsmod = 'lsmod'
        for d in ('/sbin', '/bin', '/usr/sbin', '/usr/bin'):
            if os.path.isfile(f'{d}/lsmod') and os.access(
                    f'{d}/lsmod', os.X_OK):
                lsmod = f'{d}/lsmod'
                break
        try:
            out = subprocess.check_output([lsmod], text=True,
                                          stderr=subprocess.DEVNULL)
            lines = out.splitlines(keepends=True)
        except (subprocess.CalledProcessError, FileNotFoundError):
            sys.exit(f'Can not call lsmod with {lsmod}')

    for line in lines:
        if line.startswith('Module'):
            continue
        m = re.match(r'^(\S+)', line)
        if m:
            modules[m.group(1)] = 1

    # Build config set from objects
    configs = {}
    for mod in modules:
        if mod in objects:
            for conf in objects[mod]:
                configs[conf] = mod
                dprint(f'{conf} added by direct ({mod})')
        else:
            print(f'{mod} config not found!', file=sys.stderr)

    # Read current configs
    orig_configs = {}
    for line in config_file:
        m = re.search(r'(CONFIG_[A-Za-z_0-9]*)=(m|y)', line)
        if m:
            orig_configs[m.group(1)] = m.group(2)

    valid = 'A-Za-z_0-9'

    def parse_config_depends(p, depconfig):
        nonlocal repeat
        while re.search(rf'[{valid}]', p):
            m = re.match(rf'^[^{valid}]*([{valid}]+)', p)
            if not m:
                sys.exit('parse_config_depends: unexpected')
            conf = 'CONFIG_' + m.group(1)
            p = re.sub(rf'^[^{valid}]*[{valid}]+', '', p, count=1)

            if conf not in orig_configs or orig_configs[conf] == 'y':
                continue
            if conf not in configs:
                repeat = 1
                dprint(f'{conf} selected by depend {depconfig}')
                configs[conf] = 1

    def parse_config_selects(config, p):
        nonlocal repeat
        next_config = None
        while re.search(rf'[{valid}]', p):
            m = re.match(rf'^[^{valid}]*([{valid}]+)', p)
            if not m:
                sys.exit('parse_config_selects: unexpected')
            conf = 'CONFIG_' + m.group(1)
            p = re.sub(rf'^[^{valid}]*[{valid}]+', '', p, count=1)

            if conf not in orig_configs:
                dprint(f'{conf} not set for {config} select')
                continue
            if orig_configs[conf] != 'm':
                dprint(f'{conf} (non module) selects {config}, we are good')
                return
            if conf in configs:
                dprint(f'{conf} selects {config} so we are good')
                return
            if next_config is None:
                next_config = conf

        if next_config is None:
            if config not in defaults:
                print(f'WARNING: {config} is required, but nothing in the',
                      file=sys.stderr)
                print(f'  current config selects it.', file=sys.stderr)
            return

        repeat = 1
        configs[next_config] = 1
        dprint(f'{next_config} selected by select {config}')

    repeat = 1
    while repeat:
        repeat = 0
        process_selects = {}
        for config in list(configs.keys()):
            if config in orig_configs and orig_configs[config] != 'm':
                continue
            short = config.replace('CONFIG_', '', 1)
            if short in depends:
                parse_config_depends(depends[short], short)
            if short not in prompts and short in selects:
                process_selects[short] = 1

        for config in process_selects:
            dprint(f'Process select {config}')
            parse_config_selects(config, selects[config])

    # Preserve list
    preserved = []
    if 'LMC_KEEP' in os.environ:
        preserved = os.environ['LMC_KEEP'].split(':')

    def in_preserved(conf):
        kconf = config2kfile.get(conf)
        if kconf is None:
            return False
        for excl in preserved:
            if re.match(f'^{excl}', kconf):
                return True
        return False

    setconfigs = {}

    for line in config_file:
        # IKCONFIG special case
        m = re.search(r'CONFIG_IKCONFIG', line)
        if m:
            if '# CONFIG_IKCONFIG is not set' in line:
                print('CONFIG_IKCONFIG=m')
                print('# CONFIG_IKCONFIG_PROC is not set')
            else:
                sys.stdout.write(line)
            continue

        m = re.match(r'CONFIG_MODULE_SIG_KEY="(.+)"', line)
        if m:
            orig_cert = m.group(1)
            default_cert = 'certs/signing_key.pem'
            if 'MODULE_SIG_KEY' not in depends or \
                    f'"{default_cert}"' not in depends['MODULE_SIG_KEY']:
                print(f'WARNING: MODULE_SIG_KEY assertion failure, '
                      f'update needed to {sys.argv[0]}', file=sys.stderr)
                sys.stdout.write(line)
            elif orig_cert != default_cert and not os.path.isfile(orig_cert):
                print(f'Module signature verification enabled but module '
                      f'signing key "{orig_cert}" not found. Resetting '
                      f'signing key to default value.', file=sys.stderr)
                print(f'CONFIG_MODULE_SIG_KEY="{default_cert}"')
            else:
                sys.stdout.write(line)
            continue

        m = re.match(r'CONFIG_SYSTEM_TRUSTED_KEYS="(.+)"', line)
        if m:
            orig_keys = m.group(1)
            if not os.path.isfile(orig_keys):
                print(f'System keyring enabled but keys "{orig_keys}" not '
                      f'found. Resetting keys to default value.',
                      file=sys.stderr)
                print('CONFIG_SYSTEM_TRUSTED_KEYS=""')
            else:
                sys.stdout.write(line)
            continue

        m = re.match(r'^(CONFIG.*)=(m|y)', line)
        if m:
            conf, val = m.group(1), m.group(2)
            if in_preserved(conf):
                dprint(f'Preserve config {conf}')
                sys.stdout.write(line)
                continue
            if conf in configs:
                if localyesconfig:
                    setconfigs[conf] = 'y'
                    print(f'{conf}=y')
                    continue
                setconfigs[conf] = val
            elif val == 'm':
                print(f'# {conf} is not set')
                continue
        sys.stdout.write(line)

    # Integrity check
    for mod in modules:
        if mod not in objects:
            continue
        if not any(c in setconfigs for c in objects[mod]):
            print(f'module {mod} did not have configs', end='',
                  file=sys.stderr)
            for c in objects[mod]:
                print(f' {c}', end='', file=sys.stderr)
            print(file=sys.stderr)

    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))