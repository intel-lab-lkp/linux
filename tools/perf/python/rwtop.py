#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Periodically displays system-wide r/w call activity, broken down by pid."""

import argparse
from collections import defaultdict
import os
import sys
from typing import Optional, Dict, Any
import perf
from perf_live import LiveSession

class RwTop:
    """Periodically displays system-wide r/w call activity."""
    def __init__(self, interval: int = 3, nlines: int = 20) -> None:
        self.interval_ns = interval * 1000000000
        self.nlines = nlines
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
                "bytes_requested": 0,
                "bytes_written": 0,
                "total_writes": 0,
                "comm": "",
                "errors": defaultdict(int),
            }
        )
        self.unhandled: Dict[str, int] = defaultdict(int)
        self.session: Optional[perf.session] = None
        self.last_print_time: int = 0

    def process_event(self, sample: perf.sample_event) -> None:  # pylint: disable=too-many-branches
        """Process events."""
        event_name = str(sample.evsel)
        pid = sample.sample_pid
        sample_time = sample.sample_time

        if self.last_print_time == 0:
            self.last_print_time = sample_time

        # Check if interval has passed
        if sample_time - self.last_print_time >= self.interval_ns:
            self.print_totals()
            self.last_print_time = sample_time

        try:
            comm = f"PID({pid})" if not self.session else self.session.find_thread(pid).comm()
        except Exception:  # pylint: disable=broad-except
            comm = f"PID({pid})"

        if event_name in ("evsel(syscalls:sys_enter_read)", "evsel(raw_syscalls:sys_enter_read)"):
            try:
                count = sample.count
                self.reads[pid]["bytes_requested"] += count
                self.reads[pid]["total_reads"] += 1
                self.reads[pid]["comm"] = comm
            except AttributeError:
                self.unhandled[event_name] += 1
        elif event_name in ("evsel(syscalls:sys_exit_read)", "evsel(raw_syscalls:sys_exit_read)"):
            try:
                ret = sample.ret
                if ret > 0:
                    self.reads[pid]["bytes_read"] += ret
                else:
                    self.reads[pid]["errors"][ret] += 1
            except AttributeError:
                self.unhandled[event_name] += 1
        elif event_name in ("evsel(syscalls:sys_enter_write)",
                            "evsel(raw_syscalls:sys_enter_write)"):
            try:
                count = sample.count
                self.writes[pid]["bytes_requested"] += count
                self.writes[pid]["total_writes"] += 1
                self.writes[pid]["comm"] = comm
            except AttributeError:
                self.unhandled[event_name] += 1
        elif event_name in ("evsel(syscalls:sys_exit_write)", "evsel(raw_syscalls:sys_exit_write)"):
            try:
                ret = sample.ret
                if ret > 0:
                    self.writes[pid]["bytes_written"] += ret
                else:
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
            f"{'bytes_req':>10s}  {'bytes_read':>10s}"
        )
        print(f"{'-'*6}  {'-'*20}  {'-'*10}  {'-'*10}  {'-'*10}")

        count = 0
        for pid, data in sorted(self.reads.items(),
                                key=lambda kv: kv[1]["bytes_read"], reverse=True):
            print(
                f"{pid:6d}  {data['comm']:<20s}  {data['total_reads']:10d}  "
                f"{data['bytes_requested']:10d}  {data['bytes_read']:10d}"
            )
            count += 1
            if count >= self.nlines:
                break

        print("\nfailed reads by pid:\n")
        print(f"{'pid':>6s}  {'comm':<20s}  {'error #':>6s}  {'# errors':>10s}")
        print(f"{'-'*6}  {'-'*20}  {'-'*6}  {'-'*10}")

        errcounts = []
        for pid, data in self.reads.items():
            for error, cnt in data["errors"].items():
                errcounts.append((pid, data["comm"], error, cnt))

        sorted_errcounts = sorted(errcounts, key=lambda x: x[3], reverse=True)
        for pid, comm, error, cnt in sorted_errcounts[:self.nlines]:
            print(f"{pid:6d}  {comm:<20s}  {error:6d}  {cnt:10d}")

        print("\nwrite counts by pid:\n")
        print(
            f"{'pid':>6s}  {'comm':<20s}  {'# writes':>10s}  "
            f"{'bytes_req':>10s}  {'bytes_written':>13s}"
        )
        print(f"{'-'*6}  {'-'*20}  {'-'*10}  {'-'*10}  {'-'*13}")

        count = 0
        for pid, data in sorted(self.writes.items(),
                                key=lambda kv: kv[1]["bytes_written"], reverse=True):
            print(
                f"{pid:6d}  {data['comm']:<20s}  {data['total_writes']:10d}  "
                f"{data['bytes_requested']:10d}  {data['bytes_written']:13d}"
            )
            count += 1
            if count >= self.nlines:
                break

        print("\nfailed writes by pid:\n")
        print(f"{'pid':>6s}  {'comm':<20s}  {'error #':>6s}  {'# errors':>10s}")
        print(f"{'-'*6}  {'-'*20}  {'-'*6}  {'-'*10}")

        errcounts = []
        for pid, data in self.writes.items():
            for error, cnt in data["errors"].items():
                errcounts.append((pid, data["comm"], error, cnt))

        sorted_errcounts = sorted(errcounts, key=lambda x: x[3], reverse=True)
        for pid, comm, error, cnt in sorted_errcounts[:self.nlines]:
            print(f"{pid:6d}  {comm:<20s}  {error:6d}  {cnt:10d}")

        # Reset counts
        self.reads.clear()
        self.writes.clear()

    def run(self, input_file: str) -> None:
        """Run the session."""
        self.session = perf.session(perf.data(input_file), sample=self.process_event)
        self.session.process_events()

        # Print final totals if there are any left
        if self.reads or self.writes:
            self.print_totals()

        if self.unhandled:
            print("\nunhandled events:\n")
            print(f"{'event':<40s}  {'count':>10s}")
            print(f"{'-'*40}  {'-'*10}")
            for event_name, count in self.unhandled.items():
                print(f"{event_name:<40s}  {count:10d}")

def main() -> None:
    """Main function."""
    parser = argparse.ArgumentParser(description="Trace r/w activity by PID")
    parser.add_argument(
        "interval", type=int, nargs="?", default=3, help="Refresh interval in seconds"
    )
    parser.add_argument("-i", "--input", default="perf.data", help="Input file")
    args = parser.parse_args()

    analyzer = RwTop(args.interval)
    try:
        if not os.path.exists(args.input) and args.input == "perf.data":
            # Live mode
            events = (
                "syscalls:sys_enter_read,syscalls:sys_exit_read,"
                "syscalls:sys_enter_write,syscalls:sys_exit_write"
            )
            try:
                live_session = LiveSession(events, sample_callback=analyzer.process_event)
            except OSError:
                events = (
                    "raw_syscalls:sys_enter_read,raw_syscalls:sys_exit_read,"
                    "raw_syscalls:sys_enter_write,raw_syscalls:sys_exit_write"
                )
                live_session = LiveSession(events, sample_callback=analyzer.process_event)
            print("Live mode started. Press Ctrl+C to stop.", file=sys.stderr)
            live_session.run()
        else:
            analyzer.run(args.input)
    except IOError as e:
        print(e, file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nStopping live mode...", file=sys.stderr)
        if analyzer.reads or analyzer.writes:
            analyzer.print_totals()

if __name__ == "__main__":
    main()
