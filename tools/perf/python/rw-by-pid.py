#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Display r/w activity for all processes."""

import argparse
from collections import defaultdict
import sys
from typing import Optional, Dict, List, Tuple, Any
import perf

class RwByPid:
    """Tracks and displays read/write activity by PID."""
    def __init__(self) -> None:
        self.reads: Dict[int, Dict[str, Any]] = defaultdict(
            lambda: {
                "bytes_requested": 0,
                "bytes_read": 0,
                "total_reads": 0,
                "comm": "",
                "errors": defaultdict(int),
            }
        )
        self.writes: Dict[int, Dict[str, Any]] = defaultdict(
            lambda: {
                "bytes_written": 0,
                "total_writes": 0,
                "comm": "",
                "errors": defaultdict(int),
            }
        )
        self.unhandled: Dict[str, int] = defaultdict(int)
        self.session: Optional[perf.session] = None

    def process_event(self, sample: perf.sample_event) -> None:  # pylint: disable=too-many-branches
        """Process events."""
        event_name = str(sample.evsel)[6:-1]
        pid = sample.sample_pid

        assert self.session is not None
        try:
            comm = self.session.find_thread(pid).comm()
        except Exception:  # pylint: disable=broad-except
            comm = "unknown"

        if event_name in ("syscalls:sys_enter_read", "raw_syscalls:sys_enter_read"):
            try:
                count = sample.count
                self.reads[pid]["bytes_requested"] += count
                self.reads[pid]["total_reads"] += 1
                self.reads[pid]["comm"] = comm
            except AttributeError:
                self.unhandled[event_name] += 1
        elif event_name in ("syscalls:sys_exit_read", "raw_syscalls:sys_exit_read"):
            try:
                ret = sample.ret
                if ret > 0:
                    self.reads[pid]["bytes_read"] += ret
                else:
                    self.reads[pid]["errors"][ret] += 1
            except AttributeError:
                self.unhandled[event_name] += 1
        elif event_name in ("syscalls:sys_enter_write", "raw_syscalls:sys_enter_write"):
            try:
                count = sample.count
                self.writes[pid]["bytes_written"] += count
                self.writes[pid]["total_writes"] += 1
                self.writes[pid]["comm"] = comm
            except AttributeError:
                self.unhandled[event_name] += 1
        elif event_name in ("syscalls:sys_exit_write", "raw_syscalls:sys_exit_write"):
            try:
                ret = sample.ret
                if ret <= 0:
                    self.writes[pid]["errors"][ret] += 1
            except AttributeError:
                self.unhandled[event_name] += 1
        else:
            self.unhandled[event_name] += 1

    def print_totals(self) -> None:
        """Print summary tables."""
        print("read counts by pid:\n")
        print(
            f"{'pid':>6s}  {'comm':<20s}  {'# reads':>10s}  "
            f"{'bytes_requested':>15s}  {'bytes_read':>10s}"
        )
        print(f"{'-'*6}  {'-'*20}  {'-'*10}  {'-'*15}  {'-'*10}")

        for pid, data in sorted(self.reads.items(),
                                key=lambda kv: kv[1]["bytes_read"], reverse=True):
            print(
                f"{pid:6d}  {data['comm']:<20s}  {data['total_reads']:10d}  "
                f"{data['bytes_requested']:15d}  {data['bytes_read']:10d}"
            )

        print("\nfailed reads by pid:\n")
        print(f"{'pid':>6s}  {'comm':<20s}  {'error #':>6s}  {'# errors':>10s}")
        print(f"{'-'*6}  {'-'*20}  {'-'*6}  {'-'*10}")

        errcounts: List[Tuple[int, str, int, int]] = []
        for pid, data in self.reads.items():
            for error, count in data["errors"].items():
                errcounts.append((pid, data["comm"], error, count))

        for pid, comm, error, count in sorted(errcounts, key=lambda x: x[3], reverse=True):
            print(f"{pid:6d}  {comm:<20s}  {error:6d}  {count:10d}")

        print("\nwrite counts by pid:\n")
        print(f"{'pid':>6s}  {'comm':<20s}  {'# writes':>10s}  {'bytes_written':>15s}")
        print(f"{'-'*6}  {'-'*20}  {'-'*10}  {'-'*15}")

        for pid, data in sorted(self.writes.items(),
                                key=lambda kv: kv[1]["bytes_written"], reverse=True):
            print(
                f"{pid:6d}  {data['comm']:<20s}  "
                f"{data['total_writes']:10d}  {data['bytes_written']:15d}"
            )

        print("\nfailed writes by pid:\n")
        print(f"{'pid':>6s}  {'comm':<20s}  {'error #':>6s}  {'# errors':>10s}")
        print(f"{'-'*6}  {'-'*20}  {'-'*6}  {'-'*10}")

        errcounts = []
        for pid, data in self.writes.items():
            for error, count in data["errors"].items():
                errcounts.append((pid, data["comm"], error, count))

        for pid, comm, error, count in sorted(errcounts, key=lambda x: x[3], reverse=True):
            print(f"{pid:6d}  {comm:<20s}  {error:6d}  {count:10d}")

        if self.unhandled:
            print("\nunhandled events:\n")
            print(f"{'event':<40s}  {'count':>10s}")
            print(f"{'-'*40}  {'-'*10}")
            for event_name, count in self.unhandled.items():
                print(f"{event_name:<40s}  {count:10d}")

    def run(self, input_file: str) -> None:
        """Run the session."""
        self.session = perf.session(perf.data(input_file), sample=self.process_event)
        self.session.process_events()
        self.print_totals()

def main() -> None:
    """Main function."""
    parser = argparse.ArgumentParser(description="Trace r/w activity by PID")
    parser.add_argument("-i", "--input", default="perf.data", help="Input file")
    args = parser.parse_args()

    analyzer = RwByPid()
    try:
        analyzer.run(args.input)
    except IOError as e:
        print(e, file=sys.stderr)
        sys.exit(1)

if __name__ == "__main__":
    main()
