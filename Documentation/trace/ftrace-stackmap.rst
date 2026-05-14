.. SPDX-License-Identifier: GPL-2.0

======================
Ftrace Stack Map
======================

:Author: Pengfei Li <lipengfei28@xiaomi.com>

Overview
========

The ftrace stack map provides stack trace deduplication for the ftrace
ring buffer. When enabled, instead of storing full kernel stack traces
(typically 80-160 bytes each) in the ring buffer for every event, ftrace
stores only a 4-byte ``stack_id``. The full stacks are maintained in a
separate hash table and exported via tracefs for userspace to resolve.

This is inspired by eBPF's ``BPF_MAP_TYPE_STACK_TRACE`` but integrated
into ftrace's infrastructure, requiring no userspace daemon.

Configuration
=============

Enable ``CONFIG_FTRACE_STACKMAP=y`` in the kernel config.

Kernel command line parameters:

- ``ftrace_stackmap.bits=N`` - Set map capacity to 2^N unique stacks (default: 14, range: 10-20)

Usage
=====

Enable stack deduplication::

    echo 1 > /sys/kernel/debug/tracing/options/stackmap
    echo 1 > /sys/kernel/debug/tracing/options/stacktrace
    echo function > /sys/kernel/debug/tracing/current_tracer

The trace output will show ``<stack_id N>`` instead of full stack traces::

    sh-1234 [006] d.h.. 123.456789: <stack_id 42>

To view the actual stacks::

    cat /sys/kernel/debug/tracing/stack_map

Output format::

    stack_id 42 [ref 1337, depth 8]
      [0] schedule+0x48/0xc0
      [1] schedule_timeout+0x1c/0x30
      ...

To view statistics::

    cat /sys/kernel/debug/tracing/stack_map_stat

Output::

    entries:    2500
    table_size: 5000
    hits:       148923
    drops:      0
    hit_rate:   98%

To reset the stack map::

    echo 0 > /sys/kernel/debug/tracing/stack_map

Tracefs Nodes
=============

``stack_map``
    Text export of all deduplicated stacks with symbol resolution.
    Writing ``0`` or ``reset`` clears all entries.

``stack_map_stat``
    Statistics: entry count, hits, drops, and hit rate.

``stack_map_bin``
    Binary export for efficient userspace consumption. Format:

    - Header (16 bytes): magic(u32) + version(u32) + nr_stacks(u32) + reserved(u32)
    - Per stack: stack_id(u32) + nr(u32) + ref_count(u32) + reserved(u32) + ips(u64 × nr)

    Magic: ``0x464D5342`` ('FSMB'), Version: 2

Design
======

The stack map is modeled after ``tracing_map.c`` (used by hist triggers),
using a lock-free design based on Dr. Cliff Click's non-blocking hash table
algorithm:

- **Lookup/Insert**: Lock-free via ``cmpxchg``, safe in NMI/IRQ/any context
- **Memory**: Pre-allocated element pool, zero allocation on the hot path
  (no GFP_ATOMIC failures under memory pressure)
- **Collision**: Linear probing with a 2x over-provisioned table
- **Per-instance**: Each trace_array has its own stackmap, supporting
  multiple ftrace instances
- **Hash**: 32-bit jhash of stack IPs; full ``memcmp`` confirms matches

Performance
===========

Typical results on ARM64 Android device (function tracer, 2 seconds):

- Unique stacks: ~3000
- Hit rate: 84-98% (depends on workload diversity)
- Ring buffer savings: ~80% for stack data
- Overhead per event: ~50ns (one jhash + hash table lookup)
