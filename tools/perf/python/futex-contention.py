#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Measures futex contention."""

import argparse
from collections import defaultdict
from typing import Dict, Tuple
import perf

class LockStats:
    """Aggregate lock contention information."""
    def __init__(self) -> None:
        self.count = 0
        self.total_time = 0
        self.min_time = 0
        self.max_time = 0

    def add(self, duration: int) -> None:
        """Add a new duration measurement."""
        self.count += 1
        self.total_time += duration
        if self.count == 1:
            self.min_time = duration
            self.max_time = duration
        else:
            self.min_time = min(self.min_time, duration)
            self.max_time = max(self.max_time, duration)

    def avg(self) -> float:
        """Return average duration."""
        return self.total_time / self.count if self.count > 0 else 0.0

process_names: Dict[int, str] = {}
start_times: Dict[int, Tuple[int, int]] = {}
session = None
durations: Dict[Tuple[int, int], LockStats] = defaultdict(LockStats)

FUTEX_WAIT = 0
FUTEX_WAKE = 1
FUTEX_PRIVATE_FLAG = 128
FUTEX_CLOCK_REALTIME = 256
FUTEX_CMD_MASK = ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)


def process_event(sample: perf.sample_event) -> None:
    """Process a single sample event."""
    def handle_start(tid: int, uaddr: int, op: int, start_time: int) -> None:
        if (op & FUTEX_CMD_MASK) != FUTEX_WAIT:
            return
        if tid not in process_names:
            try:
                if session:
                    process = session.process(tid)
                    if process:
                        process_names[tid] = process.comm()
            except (TypeError, AttributeError):
                return
        start_times[tid] = (uaddr, start_time)

    def handle_end(tid: int, end_time: int) -> None:
        if tid not in start_times:
            return
        (uaddr, start_time) = start_times[tid]
        del start_times[tid]
        durations[(tid, uaddr)].add(end_time - start_time)

    event_name = str(sample.evsel)
    if event_name == "evsel(syscalls:sys_enter_futex)":
        uaddr = getattr(sample, "uaddr", 0)
        op = getattr(sample, "op", 0)
        handle_start(sample.sample_tid, uaddr, op, sample.sample_time)
    elif event_name == "evsel(syscalls:sys_exit_futex)":
        handle_end(sample.sample_tid, sample.sample_time)


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description="Measure futex contention")
    ap.add_argument("-i", "--input", default="perf.data", help="Input file name")
    args = ap.parse_args()

    session = perf.session(perf.data(args.input), sample=process_event)
    session.process_events()

    for ((t, u), stats) in sorted(durations.items()):
        avg_ns = stats.avg()
        print(f"{process_names.get(t, 'unknown')}[{t}] lock {u:x} contended {stats.count} times, "
              f"{avg_ns:.0f} avg ns [max: {stats.max_time} ns, min {stats.min_time} ns]")
