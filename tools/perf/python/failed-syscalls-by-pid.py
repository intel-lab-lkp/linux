#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Displays system-wide failed system call totals, broken down by pid.
If a [comm] or [pid] arg is specified, only syscalls called by it are displayed.

Ported from tools/perf/scripts/python/failed-syscalls-by-pid.py
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import errno
import os
import sys
from typing import Optional
import perf


_GENERIC_ERRNO_OVERRIDES: dict[int, str] = {
    11: "EAGAIN", 35: "EDEADLK", 36: "ENAMETOOLONG", 37: "ENOLCK",
    38: "ENOSYS", 39: "ENOTEMPTY", 40: "ELOOP", 42: "ENOMSG",
    45: "EOPNOTSUPP", 60: "ENOSTR", 61: "ENODATA", 78: "EREMCHG",
    87: "EUSERS", 89: "EDESTADDRREQ", 90: "EMSGSIZE", 110: "ETIMEDOUT",
    111: "ECONNREFUSED", 122: "EDQUOT",
}

_ARCH_ERRNO_TABLES: dict[int, dict[int, str]] = {
    2: {35: "EAGAIN", 36: "EINPROGRESS", 37: "EALREADY", 78: "EDEADLK", 87: "ENOMSG"},
    8: {35: "ENOMSG", 45: "EDEADLK", 89: "ENOSYS", 90: "ELOOP", 122: "EDQUOT"},
    15: {35: "ENOMSG", 45: "EDEADLK", 246: "EAGAIN", 251: "ENOSYS"},
    43: {35: "EAGAIN", 36: "EINPROGRESS", 37: "EALREADY", 78: "EDEADLK", 87: "ENOMSG"},
    0x9026: {35: "EAGAIN", 11: "EDEADLK", 60: "ETIMEDOUT", 61: "ECONNREFUSED"},
}


def strerror(nr: int, e_machine: int = 0) -> str:
    """Return error string for a given errno, accounting for target e_machine."""
    err_num = abs(nr)
    if e_machine in _ARCH_ERRNO_TABLES and err_num in _ARCH_ERRNO_TABLES[e_machine]:
        return _ARCH_ERRNO_TABLES[e_machine][err_num]
    if e_machine != 0 and err_num in _GENERIC_ERRNO_OVERRIDES:
        return _GENERIC_ERRNO_OVERRIDES[err_num]
    try:
        return errno.errorcode[err_num]
    except KeyError:
        return f"Unknown {nr} errno"


class SyscallAnalyzer:
    """Analyzes failed syscalls and aggregates counts."""

    def __init__(self, for_comm: Optional[str] = None, for_pid: Optional[int] = None):
        self.for_comm = for_comm
        self.for_pid = for_pid
        self.session: Optional[perf.session] = None
        self.syscalls: dict[tuple[str, int, int, int], int] = defaultdict(int)
        machine = os.uname().machine
        self.host_64_bit = ("64" in machine or machine in ("s390x", "alpha")
                            or sys.maxsize > 0xffffffff)

    def process_event(self, sample: perf.sample_event) -> None:
        """Process raw_syscalls:sys_exit and syscalls:sys_exit events."""
        event_name = str(sample.evsel)
        if "raw_syscalls:sys_exit" not in event_name and "syscalls:sys_exit" not in event_name:
            return

        pid = sample.sample_tid
        comm = "Unknown"
        if hasattr(self, 'session') and self.session:
            try:
                thread = self.session.find_thread(sample.sample_pid, pid)
                if thread:
                    comm = thread.comm() or "Unknown"
            except (OSError, ValueError, KeyError, RuntimeError, TypeError, AttributeError):
                pass

        if self.for_comm is not None and comm != self.for_comm:
            return
        if self.for_pid is not None and pid != self.for_pid:
            return

        ret = getattr(sample, "ret", 0)
        is_64_bit = (getattr(self.session, "is_64_bit", self.host_64_bit)
                     if self.session else self.host_64_bit)
        if ret > 0:
            if not is_64_bit and 0xfffff000 <= ret <= 0xffffffff:
                ret -= 0x100000000
            elif ret >= 0xfffffffffffff000:
                ret -= 0x10000000000000000

        if -4095 <= ret < 0:
            syscall_id = getattr(sample, "__syscall_nr", -1)
            if syscall_id < 0 or syscall_id > 0xffff:
                syscall_id = getattr(sample, "nr", -1)
            if syscall_id < 0 or syscall_id > 0xffff:
                syscall_id = getattr(sample, "sys_id", -1)
            if syscall_id < 0 or syscall_id > 0xffff:
                syscall_id = getattr(sample, "id", -1)

            if 0 <= syscall_id <= 0xffff:
                self.syscalls[(comm, pid, syscall_id, ret)] += 1

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

        sorted_keys = sorted(
            self.syscalls.keys(),
            key=lambda k: (k[0], k[1], k[2], -self.syscalls[k])
        )
        current_comm_pid = None
        current_syscall = None
        emach = getattr(self.session, "e_machine", 0) or 0
        for comm, pid, syscall_id, ret in sorted_keys:
            if current_comm_pid != (comm, pid):
                print(f"\n{comm} [{pid}]")
                current_comm_pid = (comm, pid)
                current_syscall = None
            if current_syscall != syscall_id:
                try:
                    if emach:
                        name = perf.syscall_name(syscall_id, emach) or str(syscall_id)
                    else:
                        name = perf.syscall_name(syscall_id) or str(syscall_id)
                except AttributeError:
                    name = str(syscall_id)
                print(f"  syscall: {name:<16}")
                current_syscall = syscall_id
            err_str = strerror(ret, emach)
            count = self.syscalls[(comm, pid, syscall_id, ret)]
            print(f"    err = {err_str:<20}  {count:10d}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(
        description="Displays system-wide failed system call totals, "
                    "broken down by pid.")
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
    except (OSError, ValueError, KeyError, RuntimeError, TypeError, AttributeError) as e:
        print(f"Error processing events: {e}")
        sys.exit(1)
    finally:
        analyzer.session = None
