#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Calculate CPI from perf stat data or live."""

import argparse
import sys
import time
from typing import Any, Optional
import perf

class StatCpiAnalyzer:
    """Accumulates cycles and instructions and calculates CPI."""

    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.data: dict[str, tuple[int, int, int]] = {}
        self.prev_data: dict[str, tuple[int, int, int]] = {}
        self.recorded_pairs: set[tuple[int, int]] = set()

    def get_key(self, event: str, cpu: int, thread: int) -> str:
        """Get key for data dictionary."""
        return f"{event}-{cpu}-{thread}"

    def store_key(self, cpu: int, thread: int) -> None:
        """Store CPU and thread IDs."""
        self.recorded_pairs.add((cpu, thread))

    def store(self, event: str, cpu: int, thread: int, counts: tuple[int, int, int]) -> None:
        """Store counter values, computing difference from previous absolute values."""
        self.store_key(cpu, thread)
        key = self.get_key(event, cpu, thread)

        val, ena, run = counts
        if key in self.prev_data:
            prev_val, prev_ena, prev_run = self.prev_data[key]
            cur_val = val - prev_val
            cur_ena = ena - prev_ena
            cur_run = run - prev_run
        else:
            cur_val = val
            cur_ena = ena
            cur_run = run

        self.data[key] = (cur_val, cur_ena, cur_run)
        self.prev_data[key] = counts # Store absolute value for next time

    def get(self, event: str, cpu: int, thread: int) -> float:
        """Get scaled counter value."""
        key = self.get_key(event, cpu, thread)
        if key not in self.data:
            return 0.0
        val, ena, run = self.data[key]
        if run > 0:
            return val * (ena / float(run))
        return float(val)

    def process_stat_event(self, event: Any, name: Optional[str] = None) -> None:
        """Process PERF_RECORD_STAT and PERF_RECORD_STAT_ROUND events."""
        if event.type == perf.RECORD_STAT:
            if name:
                if "cycles" in name:
                    event_name = "cycles"
                elif "instructions" in name:
                    event_name = "instructions"
                else:
                    return
                self.store(event_name, event.cpu, event.thread, (event.val, event.ena, event.run))
        elif event.type == perf.RECORD_STAT_ROUND:
            timestamp = getattr(event, "time", 0)
            self.print_interval(timestamp)
            self.data.clear()
            self.recorded_pairs.clear()

    def print_interval(self, timestamp: int) -> None:
        """Print CPI for the current interval."""
        for cpu, thread in sorted(self.recorded_pairs):
            cyc = self.get("cycles", cpu, thread)
            ins = self.get("instructions", cpu, thread)
            cpi = 0.0
            if ins != 0:
                cpi = cyc / float(ins)
            t_sec = timestamp / 1000000000.0
            print(f"{t_sec:15f}: cpu {cpu}, thread {thread} -> cpi {cpi:f} ({cyc:.0f}/{ins:.0f})")

    def read_counters(self, evlist: Any) -> None:
        """Read counters live."""
        for evsel in evlist:
            name = str(evsel)
            if "cycles" in name:
                event_name = "cycles"
            elif "instructions" in name:
                event_name = "instructions"
            else:
                continue

            for cpu in evsel.cpus():
                for thread in evsel.threads():
                    try:
                        counts = evsel.read(cpu, thread)
                        self.store(event_name, cpu, thread,
                                   (counts.val, counts.ena, counts.run))
                    except OSError:
                        pass

    def run_file(self) -> None:
        """Process events from file."""
        session = perf.session(perf.data(self.args.input), stat=self.process_stat_event)
        session.process_events()

    def run_live(self) -> None:
        """Read counters live."""
        evlist = perf.parse_events("cycles,instructions")
        if not evlist:
            print("Failed to parse events", file=sys.stderr)
            return
        try:
            evlist.open()
        except OSError as e:
            print(f"Failed to open events: {e}", file=sys.stderr)
            return

        print("Live mode started. Press Ctrl+C to stop.")
        try:
            while True:
                time.sleep(self.args.interval)
                timestamp = time.time_ns()
                self.read_counters(evlist)
                self.print_interval(timestamp)
                self.data.clear()
                self.recorded_pairs.clear()
        except KeyboardInterrupt:
            print("\nStopped.")
        finally:
            evlist.close()

def main() -> None:
    """Main function."""
    ap = argparse.ArgumentParser(description="Calculate CPI from perf stat data or live")
    ap.add_argument("-i", "--input", help="Input file name (enables file mode)")
    ap.add_argument("-I", "--interval", type=float, default=1.0,
                    help="Interval in seconds for live mode")
    args = ap.parse_args()

    analyzer = StatCpiAnalyzer(args)
    if args.input:
        analyzer.run_file()
    else:
        analyzer.run_live()

if __name__ == "__main__":
    main()
