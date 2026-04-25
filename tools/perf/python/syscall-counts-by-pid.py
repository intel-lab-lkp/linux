#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Displays system-wide system call totals, broken down by syscall.
If a [comm] arg is specified, only syscalls called by [comm] are displayed.
"""

import argparse
from collections import defaultdict
import perf

syscalls: dict[tuple[str, int, int], int] = defaultdict(int)
for_comm = None
for_pid = None
session = None


def print_syscall_totals():
    """Print aggregated statistics."""
    if for_comm is not None:
        print(f"\nsyscall events for {for_comm}:\n")
    elif for_pid is not None:
        print(f"\nsyscall events for PID {for_pid}:\n")
    else:
        print("\nsyscall events:\n")

    print(f"{'comm [pid]/syscalls':<40} {'count':>10}")
    print("---------------------------------------- -----------")

    sorted_keys = sorted(syscalls.keys(), key=lambda k: (k[0], k[1], k[2]))
    current_comm_pid = None
    for comm, pid, sc_id in sorted_keys:
        if current_comm_pid != (comm, pid):
            print(f"\n{comm} [{pid}]")
            current_comm_pid = (comm, pid)
        name = perf.syscall_name(sc_id) or str(sc_id)
        print(f"  {name:<38} {syscalls[(comm, pid, sc_id)]:>10}")


def process_event(sample):
    """Process a single sample event."""
    event_name = str(sample.evsel)
    if event_name == "evsel(raw_syscalls:sys_enter)":
        sc_id = getattr(sample, "id", -1)
    elif event_name.startswith("evsel(syscalls:sys_enter_"):
        sc_id = getattr(sample, "__syscall_nr", None)
        if sc_id is None:
            sc_id = getattr(sample, "nr", -1)
    else:
        return

    if sc_id == -1:
        return

    pid = sample.sample_pid

    if for_pid and pid != for_pid:
        return

    comm = "unknown"
    try:
        if session:
            proc = session.find_thread(pid)
            if proc:
                comm = proc.comm()
    except (TypeError, AttributeError):
        pass

    if for_comm and comm != for_comm:
        return
    syscalls[(comm, pid, sc_id)] += 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("filter", nargs="?", help="COMM or PID to filter by")
    ap.add_argument("-i", "--input", default="perf.data", help="Input file name")
    args = ap.parse_args()

    if args.filter:
        try:
            for_pid = int(args.filter)
        except ValueError:
            for_comm = args.filter

    session = perf.session(perf.data(args.input), sample=process_event)
    session.process_events()
    print_syscall_totals()
