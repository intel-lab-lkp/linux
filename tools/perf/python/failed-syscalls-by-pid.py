#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Displays system-wide failed system call totals, broken down by pid.
If a [comm] or [pid] arg is specified, only syscalls called by it are displayed.

Ported from tools/perf/scripts/python/failed-syscalls-by-pid.py
"""

import argparse
from collections import defaultdict
import errno
from typing import Optional
import perf


def strerror(nr: int) -> str:
    """Return error string for a given errno."""
    try:
        return errno.errorcode[abs(nr)]
    except KeyError:
        return f"Unknown {nr} errno"


class SyscallAnalyzer:
    """Analyzes failed syscalls and aggregates counts."""

    def __init__(self, for_comm: Optional[str] = None, for_pid: Optional[int] = None):
        self.for_comm = for_comm
        self.for_pid = for_pid
        self.session: Optional[perf.session] = None
        self.syscalls: dict[tuple[str, int, int, int], int] = defaultdict(int)
        self.unhandled: dict[str, int] = defaultdict(int)

    def process_event(self, sample: perf.sample_event) -> None:
        """Process a single sample event."""
        event_name = str(sample.evsel)
        if "sys_exit" not in event_name:
            return

        pid = sample.sample_pid
        if hasattr(self, 'session') and self.session:
            comm = self.session.process(pid).comm()
        else:
            comm = "Unknown"

        if self.for_comm and comm != self.for_comm:
            return
        if self.for_pid and pid != self.for_pid:
            return

        ret = getattr(sample, "ret", 0)
        if ret < 0:
            syscall_id = getattr(sample, "id", -1)
            if syscall_id == -1:
                syscall_id = getattr(sample, "sys_id", -1)

            if syscall_id != -1:
                self.syscalls[(comm, pid, syscall_id, ret)] += 1
            else:
                self.unhandled[event_name] += 1

    def print_summary(self) -> None:
        """Print aggregated statistics."""
        if self.for_comm is not None:
            print(f"\nsyscall errors for {self.for_comm}:\n")
        elif self.for_pid is not None:
            print(f"\nsyscall errors for PID {self.for_pid}:\n")
        else:
            print("\nsyscall errors:\n")

        print(f"{'comm [pid]':<30}  {'count':>10}")
        print(f"{'-' * 30:<30}  {'-' * 10:>10}")

        sorted_keys = sorted(self.syscalls.keys(), key=lambda k: (k[0], k[1], k[2]))
        current_comm_pid = None
        for comm, pid, syscall_id, ret in sorted_keys:
            if current_comm_pid != (comm, pid):
                print(f"\n{comm} [{pid}]")
                current_comm_pid = (comm, pid)
            try:
                name = perf.syscall_name(syscall_id) or str(syscall_id)
            except AttributeError:
                name = str(syscall_id)
            print(f"  syscall: {name:<16}")
            err_str = strerror(ret)
            count = self.syscalls[(comm, pid, syscall_id, ret)]
            print(f"    err = {err_str:<20}  {count:10d}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(
        description="Displays system-wide failed system call totals, broken down by pid.")
    ap.add_argument("-i", "--input", default="perf.data",
                    help="Input file name")
    ap.add_argument("filter", nargs="?", help="COMM or PID to filter by")
    args = ap.parse_args()

    F_COMM = None
    F_PID = None

    if args.filter:
        try:
            F_PID = int(args.filter)
        except ValueError:
            F_COMM = args.filter

    analyzer = SyscallAnalyzer(F_COMM, F_PID)

    try:
        print("Press control+C to stop and show the summary")
        session = perf.session(perf.data(args.input), sample=analyzer.process_event)
        analyzer.session = session
        session.process_events()
        analyzer.print_summary()
    except KeyboardInterrupt:
        analyzer.print_summary()
    except Exception as e:
        print(f"Error processing events: {e}")
