#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Displays system-wide system call totals, broken down by syscall.

If a [comm] arg is specified, only syscalls called by [comm] are displayed.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
from typing import DefaultDict
import perf

syscalls: DefaultDict[int, int] = defaultdict(int)
for_comm = None
session = None


def print_syscall_totals():
    """Print aggregated statistics."""
    if for_comm is not None:
        print(f"\nsyscall events for {for_comm}:\n")
    else:
        print("\nsyscall events:\n")

    print(f"{'event':<40} {'count':>10}")
    print("---------------------------------------- -----------")

    for sc_id, val in sorted(syscalls.items(),
                             key=lambda kv: (kv[1], kv[0]), reverse=True):
        e_machine = getattr(session, "e_machine", 0) or 0
        if e_machine:
            name = perf.syscall_name(sc_id, e_machine) or str(sc_id)
        else:
            name = perf.syscall_name(sc_id) or str(sc_id)
        print(f"{name:<40} {val:>10}")


def process_event(sample):
    """Process a single sample event."""
    event_name = str(sample.evsel)
    if event_name.startswith("evsel(raw_syscalls:sys_enter"):
        sc_id = getattr(sample, "id", -1)
    elif event_name.startswith("evsel(syscalls:sys_enter"):
        sc_id = getattr(sample, "__syscall_nr", getattr(sample, "id", None))
        if sc_id is not None and (sc_id < 0 or sc_id > 0xffff):
            sc_id = None
        if sc_id is None:
            sc_id = getattr(sample, "nr", -1)
    else:
        return

    if sc_id < 0 or sc_id > 0xffff:
        return

    comm = "unknown"
    try:
        if session:
            proc = session.find_thread(sample.sample_tid)
            if proc:
                comm = proc.comm() or "unknown"
    except (TypeError, AttributeError):
        pass

    if for_comm and comm != for_comm:
        return
    syscalls[sc_id] += 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("comm", nargs="?", help="Only report syscalls for comm")
    ap.add_argument("-i", "--input", default="perf.data", help="Input file name")
    args = ap.parse_args()
    for_comm = args.comm
    session = perf.session(perf.data(args.input), sample=process_event)
    session.process_events()
    print_syscall_totals()
