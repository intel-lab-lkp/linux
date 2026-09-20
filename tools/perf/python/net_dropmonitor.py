#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Monitor the system for dropped packets and produce a report of drop locations and counts.
Ported from tools/perf/scripts/python/net_dropmonitor.py
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import os
import sys
from typing import Tuple
import perf


class DropMonitor:
    """Monitors dropped packets and aggregates counts by location."""

    def __init__(self, kallsyms_path: str | None = None) -> None:
        self.drop_log: dict[int, int] = defaultdict(int)
        self.kallsyms: list[Tuple[int, str]] = []
        self.resolved_syms: dict[int, Tuple[str, int]] = {}
        self.callchain_syms: dict[int, str] = {}
        self.kallsyms_path = (
            kallsyms_path
            or os.environ.get("PERF_SYMBOL_KALLSYMS")
            or "/proc/kallsyms"
        )

    def _parse_kallsyms(self) -> None:
        """Parse the kallsyms file and map kernel addresses to function symbols."""
        try:
            with open(self.kallsyms_path, "r", encoding="utf-8") as f:
                for line in f:
                    parts = line.split()
                    if len(parts) >= 3 and parts[1] in ('t', 'T', 'w', 'W'):
                        addr = int(parts[0], 16)
                        if addr > 0:
                            self.kallsyms.append((addr, parts[2]))
            self.kallsyms.sort(key=lambda x: x[0])
        except (FileNotFoundError, PermissionError):
            print(f"Failed to read {self.kallsyms_path}. Symbols will not be resolved.")

    def _get_sym(self, loc: int) -> Tuple[str, int]:
        """Resolve a memory location using session symbols or the kallsyms map."""
        if loc in self.resolved_syms:
            return self.resolved_syms[loc]
        if not self.kallsyms:
            if loc in self.callchain_syms:
                return self.callchain_syms[loc], 0
            return str(loc), 0

        start = 0
        end = len(self.kallsyms) - 1
        while start < end:
            mid = (start + end) // 2
            if self.kallsyms[mid][0] <= loc < self.kallsyms[mid+1][0]:
                start = mid
                break
            if loc < self.kallsyms[mid][0]:
                end = mid - 1
            else:
                start = mid + 1

        sym_addr, sym_name = self.kallsyms[start]
        if loc >= sym_addr:
            return sym_name, loc - sym_addr
        if loc in self.callchain_syms:
            return self.callchain_syms[loc], 0
        return str(loc), 0

    def print_drop_table(self) -> None:
        """Print aggregated results."""
        if not self.drop_log:
            print(f"{'LOCATION':>25} {'OFFSET':>25} {'COUNT':>25}")
            return

        if len(self.resolved_syms) < len(self.drop_log):
            print("Gathering kallsyms data")
            self._parse_kallsyms()

        print(f"{'LOCATION':>25} {'OFFSET':>25} {'COUNT':>25}")
        sorted_keys = sorted(self.drop_log.keys())
        for sloc in sorted_keys:
            sym, off = self._get_sym(sloc)
            print(f"{sym:>25} {off:>25d} {self.drop_log[sloc]:>25d}")

    def process_event(self, sample: perf.sample_event) -> None:
        """Process a single sample event."""
        if "skb:kfree_skb" not in str(sample.evsel):
            return

        location = getattr(sample, "location", None)
        if location is not None:
            self.drop_log[location] += 1
            if location not in self.resolved_syms:
                if getattr(sample, "sample_ip", 0) == location and getattr(sample, "symbol", None):
                    self.resolved_syms[location] = (
                        sample.symbol,
                        getattr(sample, "sym_offset", 0) or 0,
                    )
                else:
                    for entry in getattr(sample, "callchain", []) or []:
                        if isinstance(entry, dict):
                            entry_ip = entry.get("ip")
                            entry_sym = entry.get("sym")
                            sym_name = (
                                entry_sym.get("name") if isinstance(entry_sym, dict) else None
                            )
                            sym_start = (
                                entry_sym.get("start")
                                if isinstance(entry_sym, dict)
                                else None
                            )
                        else:
                            entry_ip = getattr(entry, "ip", None)
                            entry_sym = getattr(entry, "sym", None)
                            sym_name = (
                                getattr(entry_sym, "name", None)
                                or getattr(entry, "symbol", None)
                            )
                            sym_start = getattr(entry_sym, "start", None)
                        if entry_ip == location and sym_name:
                            if sym_start is not None:
                                self.resolved_syms[location] = (
                                    sym_name,
                                    max(0, location - sym_start),
                                )
                            else:
                                self.callchain_syms[location] = sym_name
                            break


if __name__ == "__main__":
    ap = argparse.ArgumentParser(
        description="Monitor the system for dropped packets and produce a "
                    "report of drop locations and counts.")
    ap.add_argument("-i", "--input", default="perf.data", help="Input file name")
    ap.add_argument("-k", "--kallsyms", default=None,
                    help="Path to kallsyms file for offline symbol resolution")
    args = ap.parse_args()

    monitor = DropMonitor(kallsyms_path=args.kallsyms)

    try:
        session = perf.session(perf.data(args.input), sample=monitor.process_event,
                               kallsyms=args.kallsyms)
        session.process_events()
    except KeyboardInterrupt:
        print("\nStopping trace...")
    except (OSError, ValueError, KeyError, RuntimeError, TypeError, AttributeError) as e:
        print(f"Error processing events: {e}")
        sys.exit(1)

    monitor.print_drop_table()
