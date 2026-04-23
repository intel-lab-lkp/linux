#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""
Basic test of Python scripting support for perf.
Ported from tools/perf/scripts/python/check-perf-trace.py
"""

import argparse
import collections
import perf

unhandled: collections.defaultdict[str, int] = collections.defaultdict(int)
session = None

softirq_vecs = {
    0: "HI_SOFTIRQ",
    1: "TIMER_SOFTIRQ",
    2: "NET_TX_SOFTIRQ",
    3: "NET_RX_SOFTIRQ",
    4: "BLOCK_SOFTIRQ",
    5: "IRQ_POLL_SOFTIRQ",
    6: "TASKLET_SOFTIRQ",
    7: "SCHED_SOFTIRQ",
    8: "HRTIMER_SOFTIRQ",
    9: "RCU_SOFTIRQ",
}

def trace_begin() -> None:
    """Called at the start of trace processing."""
    print("trace_begin")

def trace_end() -> None:
    """Called at the end of trace processing."""
    print_unhandled()

def symbol_str(event_name: str, field_name: str, value: int) -> str:
    """Resolves symbol values to strings."""
    if event_name == "irq__softirq_entry" and field_name == "vec":
        return softirq_vecs.get(value, str(value))
    return str(value)

def flag_str(event_name: str, field_name: str, value: int) -> str:
    """Resolves flag values to strings."""
    if event_name == "kmem__kmalloc" and field_name == "gfp_flags":
        return f"0x{value:x}"
    return str(value)

def print_header(event_name: str, sample: perf.sample_event) -> None:
    """Prints common header for events."""
    secs = sample.sample_time // 1000000000
    nsecs = sample.sample_time % 1000000000
    comm = session.process(sample.sample_pid).comm() if session else "[unknown]"
    print(f"{event_name:<20} {sample.sample_cpu:5} {secs:05}.{nsecs:09} "
          f"{sample.sample_pid:8} {comm:<20} ", end=' ')

def print_uncommon(sample: perf.sample_event) -> None:
    """Prints uncommon fields for tracepoints."""
    # Fallback to 0 if field not found (e.g. on older kernels or if not tracepoint)
    pc = getattr(sample, "common_preempt_count", 0)
    flags = getattr(sample, "common_flags", 0)
    lock_depth = getattr(sample, "common_lock_depth", 0)

    print(f"common_preempt_count={pc}, common_flags={flags}, "
          f"common_lock_depth={lock_depth}, ")

def irq__softirq_entry(sample: perf.sample_event) -> None:
    """Handles irq:softirq_entry events."""
    print_header("irq__softirq_entry", sample)
    print_uncommon(sample)
    print(f"vec={symbol_str('irq__softirq_entry', 'vec', sample.vec)}")

def kmem__kmalloc(sample: perf.sample_event) -> None:
    """Handles kmem:kmalloc events."""
    print_header("kmem__kmalloc", sample)
    print_uncommon(sample)

    print(f"call_site={sample.call_site:d}, ptr={sample.ptr:d}, "
          f"bytes_req={sample.bytes_req:d}, bytes_alloc={sample.bytes_alloc:d}, "
          f"gfp_flags={flag_str('kmem__kmalloc', 'gfp_flags', sample.gfp_flags)}")

def trace_unhandled(event_name: str) -> None:
    """Tracks unhandled events."""
    unhandled[event_name] += 1

def print_unhandled() -> None:
    """Prints summary of unhandled events."""
    if not unhandled:
        return
    print("\nunhandled events:\n")
    print(f"{'event':<40} {'count':>10}")
    print("---------------------------------------- -----------")
    for event_name, count in unhandled.items():
        print(f"{event_name:<40} {count:10}")

def process_event(sample: perf.sample_event) -> None:
    """Callback for processing events."""
    event_name = str(sample.evsel)
    if event_name == "evsel(irq:softirq_entry)":
        irq__softirq_entry(sample)
    elif "evsel(kmem:kmalloc)" in event_name:
        kmem__kmalloc(sample)
    else:
        trace_unhandled(event_name)

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("-i", "--input", default="perf.data", help="Input file name")
    args = ap.parse_args()

    trace_begin()
    session = perf.session(perf.data(args.input), sample=process_event)
    session.process_events()
    trace_end()
