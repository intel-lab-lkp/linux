#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
#
# Copyright (C) 2023 Red Hat, Inc. Alessandro Carminati <alessandro.carminati@gmail.com>
#
# kas_alias: Adds alias to duplicate symbols in the kallsyms output.

import subprocess
import sys
import os
import argparse
import re
from collections import namedtuple

regex_filter = [
        "^__compound_literal\\.[0-9]+$",
        "^__[wm]*key\\.[0-9]+$",
        "^_*TRACE_SYSTEM.*$",
        "^__already_done\\.[0-9]+$",
        "^__msg\\.[0-9]+$",
        "^__func__\\.[0-9]+$",
        "^CSWTCH\\.[0-9]+$",
        "^_rs\\.[0-9]+$",
        "^___tp_str\\.[0-9]+$",
        "^__flags\\.[0-9]+$",
        "^___done\\.[0-9]+$",
        "^__print_once\\.[0-9]+$",
        "^___once_key\\.[0-9]+$",
        "^__pfx_.*$",
        "^__cfi_.*$"
        ]

class SeparatorType:
    def __call__(self, separator):
        if len(separator) != 1:
            raise argparse.ArgumentTypeError("Separator must be a single character")
        return separator

Line = namedtuple('Line', ['address', 'type', 'name'])

def parse_file(filename):
    symbol_list = []
    name_occurrences = {}

    with open(filename, 'r') as file:
        for line in file:
            fields = line.strip().split()

            if len(fields) >= 3:
                address, type, name = fields[0], fields[1], ' '.join(fields[2:])
                symbol_list.append(Line(address, type, name))
                name_occurrences[name] = name_occurrences.get(name, 0) + 1

    return symbol_list, name_occurrences

def find_duplicate(symbol_list, name_occurrences):
    name_to_lines = {}
    duplicate_lines = []

    for line in symbol_list:
        if line.name in name_to_lines:
            first_occurrence = name_to_lines[line.name]
            duplicate_lines.extend([first_occurrence, line])
        else:
            name_to_lines[line.name] = line

    return duplicate_lines

def start_addr2line_process(binary_file, addr2line_file):
    try:
        addr2line_process = subprocess.Popen([addr2line_file, '-fe', binary_file],
                                             stdin=subprocess.PIPE,
                                             stdout=subprocess.PIPE,
                                             stderr=subprocess.PIPE,
                                             text=True)
        return addr2line_process
    except Exception as e:
        print(f"Error starting addr2line process: {str(e)}")
        sys.exit(1)

def addr2line_fetch_address(addr2line_process, address):
    try:
        addr2line_process.stdin.write(address + '\n')
        addr2line_process.stdin.flush()
        addr2line_process.stdout.readline().strip()
        output = addr2line_process.stdout.readline().strip()

        return os.path.normpath(output)
    except Exception as e:
        print(f"Error communicating with addr2line: {str(e)}")
        sys.exit(1)

def process_line(obj, config):
    if config:
        return not (any(re.match(regex, obj.name) for regex in regex_filter))
    else:
        return obj.type in {"T", "t"}

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Add alias to multiple occurring symbols name in kallsyms')
    parser.add_argument('-a', "--addr2line", dest="addr2line_file", required=True)
    parser.add_argument('-v', "--vmlinux", dest="vmlinux_file", required=True)
    parser.add_argument('-o', "--outfile", dest="output_file", required=True)
    parser.add_argument('-n', "--nmdata", dest="nm_data_file", required=True)
    parser.add_argument('-b', "--basedir", dest="linux_base_dir", required=True)
    parser.add_argument('-s', "--separator", dest="separator", required=False, default="@", type=SeparatorType())
    parser.add_argument('-d', "--data", dest="include_data", required=False, action='store_true')
    config = parser.parse_args()

    try:
        config.linux_base_dir = os.path.normpath(os.getcwd() + "/" + config.linux_base_dir) + "/"
        symbol_list, name_occurrences = parse_file(config.nm_data_file)
        addr2line_process = start_addr2line_process(config.vmlinux_file, config.addr2line_file)

        with open(config.output_file, 'w') as file:
            for obj in symbol_list:
                file.write(f"{obj.address} {obj.type} {obj.name}\n")
                if (name_occurrences[obj.name] > 1) and process_line(obj, config.include_data) :
                    output = addr2line_fetch_address(addr2line_process, obj.address)
                    decoration = config.separator + "".join(
                        "_" if not c.isalnum() else c for c in output.replace(config.linux_base_dir, "")
                    )
                    # The addr2line can emit the special string "?:??" when addr2line can not find the
                    # specified address in the DWARF section that after normalization it becomes "____".
                    # In such cases, emitting an alias wouldn't make sense, so it is skipped.
                    if decoration != config.separator + "____":
                        file.write(f"{obj.address} {obj.type} {obj.name + decoration}\n")

        addr2line_process.stdin.close()
        addr2line_process.stdout.close()
        addr2line_process.stderr.close()
        addr2line_process.wait()

    except Exception as e:
        print(f"An error occurred: {str(e)}")
        raise SystemExit("Script terminated due to an error")
