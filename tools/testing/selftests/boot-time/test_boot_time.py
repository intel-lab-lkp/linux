#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
#
# Copyright (c) 2024 Collabora Ltd
#
# This script reads the
# /sys/kernel/debug/tracing/events/synthetic/initcall_latency/hist file,
# extracts function names and timings, and compares them against reference
# timings provided in an input JSON file to identify significant boot
# slowdowns.
# The script operates in two modes:
# - Generate Mode: parses initcall timings from the current kernel's ftrace
#   event histogram and generates a JSON reference file with function
#   names, start times, end times, and latencies.
# - Test Mode: compares current initcall timings against the reference
#   file, allowing users to define a maximum allowed difference between the
#   values (delta). Users can also apply custom delta thresholds for
#   specific initcalls using regex-based overrides. The comparison can be
#   done on latency, start, or end times.
#

import os
import sys
import argparse
import gzip
import json
import re
import subprocess

this_dir = os.path.dirname(os.path.realpath(__file__))
sys.path.append(os.path.join(this_dir, "../kselftest/"))

import ksft

def load_reference_from_json(file_path):
    """
    Load reference data from a JSON file and returns the parsed data.
    @file_path: path to the JSON file.
    """

    try:
        with open(file_path, 'r', encoding="utf-8") as file:
            return json.load(file)
    except FileNotFoundError:
        ksft.print_msg(f"Error: File {file_path} not found.")
        ksft.exit_fail()
    except json.JSONDecodeError:
        ksft.print_msg(f"Error: Failed to decode JSON from {file_path}.")
        ksft.exit_fail()


def mount_debugfs(path):
    """
    Mount debugfs at the specified path if it is not already mounted.
    @path: path where debugfs should be mounted
    """
    # Check if debugfs is already mounted
    with open('/proc/mounts', 'r', encoding="utf-8") as mounts:
        for line in mounts:
            if 'debugfs' in line and path in line:
                print(f"debugfs is already mounted at {path}")
                return True

    # Mount debugfs
    try:
        subprocess.run(['mount', '-t', 'debugfs', 'none', path], check=True)
        return True
    except subprocess.CalledProcessError as e:
        print(f"Failed to mount debugfs: {e.stderr}")
        return False


def ensure_unique_function_name(func, initcall_entries):
    """
    Ensure the function name is unique by appending a suffix if necessary.
    @func: the original function name.
    @initcall_entries: a dictionary containing parsed initcall entries.
    """
    i = 2
    base_func = func
    while func in initcall_entries:
        func = f'{base_func}[{i}]'
        i += 1
    return func


def parse_initcall_latency_hist():
    """
    Parse the ftrace histogram for the initcall_latency event, extracting
    function names, start times, end times, and latencies. Return a
    dictionary where each entry is structured as follows:
    {
        <function symbolic name>: {
            "start": <start time>,
            "end": <end time>,
            "latency": <latency>
        }
    }
    """

    pattern = re.compile(r'\{ func: \[\w+\] ([\w_]+)\s*, start: *(\d+), end: *(\d+) \} hitcount: *\d+  lat: *(\d+)')
    initcall_entries = {}

    try:
        with open('/sys/kernel/debug/tracing/events/synthetic/initcall_latency/hist', 'r', encoding="utf-8") as hist_file:
            for line in hist_file:
                match = pattern.search(line)
                if match:
                    func = match.group(1).strip()
                    start = int(match.group(2))
                    end = int(match.group(3))
                    latency = int(match.group(4))

                    # filter out unresolved names
                    if not func.startswith("0x"):
                        func = ensure_unique_function_name(func, initcall_entries)

                        initcall_entries[func] = {
                            "start": start,
                            "end": end,
                            "latency": latency
                        }
    except FileNotFoundError:
        print("Error: Histogram file not found.")

    return initcall_entries


def compare_initcall_list(ref_initcall_entries, cur_initcall_entries):
    """
    Compare the current list of initcall functions against the reference
    file. Print warnings if there are unique entries in either.
    @ref_initcall_entries: reference initcall entries.
    @cur_initcall_entries: current initcall entries.
    """
    ref_entries = set(ref_initcall_entries.keys())
    cur_entries = set(cur_initcall_entries.keys())

    unique_to_ref = ref_entries - cur_entries
    unique_to_cur = cur_entries - ref_entries

    if (unique_to_ref):
        ksft.print_msg(
            f"Warning: {list(unique_to_ref)} not found in current data. Consider updating reference file.")
    if unique_to_cur:
        ksft.print_msg(
            f"Warning: {list(unique_to_cur)} not found in reference data. Consider updating reference file.")


def run_test(ref_file_path, delta, overrides, mode):
    """
    Run the test comparing the current timings with the reference values.
    @ref_file_path: path to the JSON file containing reference values.
    @delta: default allowed difference between reference and current
    values.
    @overrides: override rules in the form of regex:threshold.
    @mode: the comparison mode (either 'start', 'end', or 'latency').
    """

    ref_data = load_reference_from_json(ref_file_path)

    ref_initcall_entries = ref_data['data']
    cur_initcall_entries = parse_initcall_latency_hist()

    compare_initcall_list(ref_initcall_entries, cur_initcall_entries)

    ksft.set_plan(len(ref_initcall_entries))

    for func_name in ref_initcall_entries:
        effective_delta = delta
        for regex, override_delta in overrides.items():
            if re.match(regex, func_name):
                effective_delta = override_delta
                break
        if (func_name in cur_initcall_entries):
            ref_metric = ref_initcall_entries[func_name].get(mode)
            cur_metric = cur_initcall_entries[func_name].get(mode)
            if (cur_metric > ref_metric and (cur_metric - ref_metric) >= effective_delta):
                ksft.test_result_fail(func_name)
                ksft.print_msg(f"'{func_name}' {mode} differs by "
                               f"{(cur_metric - ref_metric)} usecs.")
            else:
                ksft.test_result_pass(func_name)
        else:
            ksft.test_result_skip(func_name)


def generate_reference_file(file_path):
    """
    Generate a reference file in JSON format, containing kernel metadata
    and initcall timing data.
    @file_path: output file path.
    """
    metadata = {}

    config_file = "/proc/config.gz"
    if os.path.isfile(config_file):
        with gzip.open(config_file, "rt", encoding="utf-8") as f:
            config = f.read()
            metadata["config"] = config

    metadata["version"] = os.uname().release

    cmdline_file = "/proc/cmdline"
    if os.path.isfile(cmdline_file):
        with open(cmdline_file, "r", encoding="utf-8") as f:
            cmdline = f.read().strip()
            metadata["cmdline"] = cmdline

    ref_data = {
        "metadata": metadata,
        "data": parse_initcall_latency_hist(),
    }

    with open(file_path, "w", encoding='utf-8') as f:
        json.dump(ref_data, f, indent=4)
        print(f"Generated {file_path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="")

    subparsers = parser.add_subparsers(dest='mode', required=True, help='Choose between generate or test modes')

    generate_parser = subparsers.add_parser('generate', help="Generate a reference file")
    generate_parser.add_argument('out_ref_file', nargs='?', default='reference_initcall_timings.json',
                                 help='Path to output JSON reference file (default: reference_initcall_timings.json)')

    compare_parser = subparsers.add_parser('test', help='Test against a reference file')
    compare_parser.add_argument('in_ref_file', help='Path to JSON reference file')
    compare_parser.add_argument(
        'delta', type=int, help='Maximum allowed delta between the current and the reference timings (usecs)')
    compare_parser.add_argument('--override', '-o', action='append', type=str,
                                help="Specify regex-based rules as regex:delta (e.g., '^acpi_.*:50')")
    compare_parser.add_argument('--mode', '-m', default='latency', choices=[
                                'start', 'end', 'latency'],
                                help="Comparison mode: 'latency' (default) for latency, 'start' for start times, or 'end' for end times.")

    args = parser.parse_args()

    if args.mode == 'generate':
        generate_reference_file(args.out_ref_file)
        sys.exit(0)

    # Process overrides
    overrides = {}
    if args.override:
        for override in args.override:
            try:
                pattern, delta = override.split(":")
                overrides[pattern] = int(delta)
            except ValueError:
                print(f"Invalid override format: {override}. Expected format is 'regex:delta'.")
                sys.exit(1)

    # Ensure debugfs is mounted
    if not mount_debugfs("/sys/kernel/debug"):
        ksft.exit_fail()

    ksft.print_header()

    run_test(args.in_ref_file, args.delta, overrides, args.mode)

    ksft.finished()
