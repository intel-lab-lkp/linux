#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Periodically displays system-wide r/w call activity, broken down by pid."""
from __future__ import annotations

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
        self.offline = False
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
        self.comm_cache: Dict[int, str] = {}
        self.session: Optional[perf.session] = None
        self.last_print_time: int = 0
        machine = os.uname().machine
        self.host_64_bit = ("64" in machine or machine in ("s390x", "alpha")
                            or sys.maxsize > 0xffffffff)

    def get_comm(self, pid: int) -> str:
        """Resolve and cache the comm(and) of a pid."""
        comm = self.comm_cache.get(pid)
        if comm:
            return comm
        comm = None
        try:
            if self.session:
                thread = self.session.find_thread(pid)
                comm = thread.comm() if thread else None
            else:
                with open(f"/proc/{pid}/comm", "r", encoding="utf-8", errors="replace") as f:
                    comm = f.read().strip()
        except (TypeError, AttributeError):
            # find_thread returns None when the thread isn't known.
            pass
        except OSError:
            # The thread may have exited before /proc could be read.
            pass
        if not comm:
            comm = f"PID({pid})"
        comm = ''.join(c if c.isprintable() else '?' for c in comm)
        self.comm_cache[pid] = comm
        return comm

    def process_event(self, sample: perf.sample_event) -> None:
        """Process events."""
        event_name = str(sample.evsel)
        pid = sample.sample_pid
        sample_time = sample.sample_time

        if self.last_print_time == 0:
            self.last_print_time = sample_time

        # Check if interval has passed
        if sample_time > self.last_print_time and sample_time - self.last_print_time >= self.interval_ns:
            self.print_totals()
            self.last_print_time = sample_time

        # Map each event onto the totals it updates. "enter" events count the
        # requested bytes, "exit" events the transferred bytes or the error.
        handlers = {
            "evsel(syscalls:sys_enter_read)": (self.reads, "total_reads", None),
            "evsel(syscalls:sys_exit_read)": (self.reads, None, "bytes_read"),
            "evsel(syscalls:sys_enter_write)": (self.writes, "total_writes", None),
            "evsel(syscalls:sys_exit_write)": (self.writes, None, "bytes_written"),
        }
        handler = handlers.get(event_name)
        if not handler:
            self.unhandled[event_name] += 1
            return

        totals, count_key, bytes_key = handler
        try:
            value = sample.count if count_key else sample.ret
        except AttributeError:
            self.unhandled[event_name] += 1
            return

        data = totals[pid]
        data["comm"] = self.get_comm(pid)
        if count_key:
            data["bytes_requested"] += value
            data[count_key] += 1
        elif bytes_key:
            is_64_bit = (getattr(self.session, "is_64_bit", self.host_64_bit)
                         if self.session else self.host_64_bit)
            if value > 0:
                if not is_64_bit and 0xfffff000 <= value <= 0xffffffff:
                    value -= 0x100000000
                elif value >= 0xfffffffffffff000:
                    value -= 0x10000000000000000
            if value >= 0:
                data[bytes_key] += value
            else:
                data["errors"][value] += 1

    def print_totals(self) -> None:
        """Print summary tables."""
        if not self.offline:
            print('\x1b[H\x1b[2J', end='')
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
        self.comm_cache.clear()

    def run(self, input_file: str) -> None:
        """Run the session."""
        self.session = perf.session(perf.data(input_file), sample=self.process_event)
        try:
            self.session.process_events()
        finally:
            self.session = None

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
    parser.add_argument("-l", "--live", action="store_true", help="Run in live mode")
    args = parser.parse_args()

    analyzer = RwTop(args.interval)
    try:
        if args.live or (not os.path.exists(args.input) and args.input == "perf.data"):
            # Live mode
            events = (
                "syscalls:sys_enter_read,syscalls:sys_exit_read,"
                "syscalls:sys_enter_write,syscalls:sys_exit_write"
            )
            live_session = LiveSession(events, sample_callback=analyzer.process_event)
            print("Live mode started. Press Ctrl+C to stop.", file=sys.stderr)
            live_session.run()
        else:
            analyzer.offline = True
            analyzer.run(args.input)
    except IOError as e:
        print(e, file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        if not analyzer.offline:
            print("\nStopping live mode...", file=sys.stderr)
        if analyzer.reads or analyzer.writes:
            analyzer.print_totals()

if __name__ == "__main__":
    main()
