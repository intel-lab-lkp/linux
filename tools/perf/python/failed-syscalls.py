#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Failed system call counts."""

import argparse
from collections import defaultdict
import sys
from typing import Optional
import perf

class FailedSyscalls:
    """Tracks and displays failed system call totals."""
    def __init__(self, comm: Optional[str] = None) -> None:
        self.failed_syscalls: dict[str, int] = defaultdict(int)
        self.for_comm = comm
        self.session: Optional[perf.session] = None
        self.unhandled: dict[str, int] = defaultdict(int)

    def process_event(self, sample: perf.sample_event) -> None:
        """Process sys_exit events."""
        event_name = str(sample.evsel)
        if not event_name.startswith("evsel(syscalls:sys_exit_") and \
           not event_name.startswith("evsel(raw_syscalls:sys_exit_"):
            return

        try:
            ret = sample.ret
        except AttributeError:
            self.unhandled[event_name] += 1
            return

        if ret >= 0:
            return

        pid = sample.sample_pid
        assert self.session is not None
        try:
            comm = self.session.find_thread(pid).comm()
        except Exception: # pylint: disable=broad-except
            comm = "unknown"

        if self.for_comm and comm != self.for_comm:
            return

        self.failed_syscalls[comm] += 1

    def print_totals(self) -> None:
        """Print summary table."""
        print("\nfailed syscalls by comm:\n")
        print(f"{'comm':<20s}  {'# errors':>10s}")
        print(f"{'-'*20}  {'-'*10}")

        for comm, val in sorted(self.failed_syscalls.items(),
                                key=lambda kv: (kv[1], kv[0]), reverse=True):
            print(f"{comm:<20s}  {val:10d}")

    def run(self, input_file: str) -> None:
        """Run the session."""
        self.session = perf.session(perf.data(input_file), sample=self.process_event)
        self.session.process_events()
        self.print_totals()

def main() -> None:
    """Main function."""
    parser = argparse.ArgumentParser(description="Trace failed syscalls")
    parser.add_argument("comm", nargs="?", help="Filter by command name")
    parser.add_argument("-i", "--input", default="perf.data", help="Input file")
    args = parser.parse_args()

    analyzer = FailedSyscalls(args.comm)
    try:
        analyzer.run(args.input)
    except IOError as e:
        print(e, file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
