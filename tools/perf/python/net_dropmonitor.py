#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Monitor the system for dropped packets and produce a report of drop locations and counts.
Ported from tools/perf/scripts/python/net_dropmonitor.py
"""

import argparse
from collections import defaultdict
import sys
import perf


class DropMonitor:
    """Monitors dropped packets and aggregates counts by location."""

    def __init__(self):
        self.drop_log: dict[tuple[str, int], int] = defaultdict(int)
        self.unhandled: dict[str, int] = defaultdict(int)

    def print_drop_table(self) -> None:
        """Print aggregated results."""
        print(f"{'LOCATION':>25} {'OFFSET':>25} {'COUNT':>25}")
        for (sym, off) in sorted(self.drop_log.keys()):
            print(f"{sym:>25} {off:>25d} {self.drop_log[(sym, off)]:>25d}")

    def process_event(self, sample: perf.sample_event) -> None:
        """Process a single sample event."""
        if str(sample.evsel) != "evsel(skb:kfree_skb)":
            return

        try:
            symbol = getattr(sample, "symbol", "[unknown]")
            symoff = getattr(sample, "symoff", 0)
            self.drop_log[(symbol, symoff)] += 1
        except AttributeError:
            self.unhandled[str(sample.evsel)] += 1


if __name__ == "__main__":
    ap = argparse.ArgumentParser(
        description="Monitor the system for dropped packets and produce a "
                    "report of drop locations and counts.")
    ap.add_argument("-i", "--input", default="perf.data", help="Input file name")
    args = ap.parse_args()

    monitor = DropMonitor()

    try:
        session = perf.session(perf.data(args.input), sample=monitor.process_event)
        session.process_events()
    except KeyboardInterrupt:
        print("\nStopping trace...")
    except Exception as e:
        print(f"Error processing events: {e}")
        sys.exit(1)

    monitor.print_drop_table()
