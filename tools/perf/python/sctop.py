#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
System call top

Periodically displays system-wide system call totals, broken down by
syscall.  If a [comm] arg is specified, only syscalls called by
[comm] are displayed. If an [interval] arg is specified, the display
will be refreshed every [interval] seconds.  The default interval is
3 seconds.

Ported from tools/perf/scripts/python/sctop.py
"""

import argparse
from collections import defaultdict
import sys
import threading
import perf
from perf_live import LiveSession




class SCTopAnalyzer:
    """Periodically displays system-wide system call totals."""

    def __init__(self, for_comm: str | None, interval: int, offline: bool = False):
        self.for_comm = for_comm
        self.interval = interval
        self.syscalls: dict[int, int] = defaultdict(int)
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self.print_syscall_totals)
        self.offline = offline
        self.last_print_time: int | None = None

    def syscall_name(self, syscall_id: int) -> str:
        """Lookup syscall name by ID."""
        try:
            return perf.syscall_name(syscall_id)
        except Exception:  # pylint: disable=broad-exception-caught
            pass
        return str(syscall_id)

    def process_event(self, sample: perf.sample_event) -> None:
        """Collect syscall events."""
        name = str(sample.evsel)
        syscall_id = getattr(sample, "id", -1)

        if syscall_id == -1:
            return

        if hasattr(self, 'session') and self.session:
            comm = self.session.process(sample.sample_pid).comm()
        else:
            comm = getattr(sample, "comm", "Unknown")

        if name in ("evsel(raw_syscalls:sys_enter)", "evsel(syscalls:sys_enter)"):
            if self.for_comm is not None and comm != self.for_comm:
                return
            with self.lock:
                self.syscalls[syscall_id] += 1

        if self.offline and hasattr(sample, "time"):
            interval_ns = self.interval * (10 ** 9)
            if self.last_print_time is None:
                self.last_print_time = sample.time
            elif sample.time - self.last_print_time >= interval_ns:
                self.print_current_totals()
                self.last_print_time = sample.time

    def print_current_totals(self):
        """Print current syscall totals."""
        # Clear terminal
        print("\x1b[2J\x1b[H", end="")

        if self.for_comm is not None:
            print(f"\nsyscall events for {self.for_comm}:\n")
        else:
            print("\nsyscall events:\n")

        print(f"{'event':40s}  {'count':10s}")
        print(f"{'-' * 40:40s}  {'-' * 10:10s}")

        with self.lock:
            current_syscalls = list(self.syscalls.items())
            self.syscalls.clear()

        current_syscalls.sort(key=lambda kv: (kv[1], kv[0]), reverse=True)

        for syscall_id, val in current_syscalls:
            print(f"{self.syscall_name(syscall_id):<40s}  {val:10d}")

    def print_syscall_totals(self):
        """Periodically print syscall totals."""
        while not self.stop_event.is_set():
            self.print_current_totals()
            self.stop_event.wait(self.interval)
        # Print final batch
        self.print_current_totals()

    def start(self):
        """Start the background thread."""
        self.thread.start()

    def stop(self):
        """Stop the background thread."""
        self.stop_event.set()
        self.thread.join()


def main():
    """Main function."""
    ap = argparse.ArgumentParser(description="System call top")
    ap.add_argument("args", nargs="*", help="[comm] [interval] or [interval]")
    ap.add_argument("-i", "--input", help="Input file name")
    args = ap.parse_args()

    for_comm = None
    default_interval = 3
    interval = default_interval

    if len(args.args) > 2:
        print("Usage: perf script -s sctop.py [comm] [interval]")
        sys.exit(1)

    if len(args.args) > 1:
        for_comm = args.args[0]
        try:
            interval = int(args.args[1])
        except ValueError:
            print(f"Invalid interval: {args.args[1]}")
            sys.exit(1)
    elif len(args.args) > 0:
        try:
            interval = int(args.args[0])
        except ValueError:
            for_comm = args.args[0]
            interval = default_interval

    analyzer = SCTopAnalyzer(for_comm, interval, offline=bool(args.input))

    if not args.input:
        analyzer.start()

    try:
        if args.input:
            session = perf.session(perf.data(args.input), sample=analyzer.process_event)
            analyzer.session = session
            session.process_events()
        else:
            try:
                live_session = LiveSession(
                    "raw_syscalls:sys_enter", sample_callback=analyzer.process_event
                )
            except OSError:
                live_session = LiveSession(
                    "syscalls:sys_enter", sample_callback=analyzer.process_event
                )
            live_session.run()
    except KeyboardInterrupt:
        pass
    except IOError as e:
        print(f"Error: {e}")
    finally:
        if args.input:
            analyzer.print_current_totals()
        else:
            analyzer.stop()


if __name__ == "__main__":
    main()
