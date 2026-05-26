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

- ``ftrace_stackmap.bits=N`` - Set map capacity to 2^N unique stacks
  (default: 14 → 16384 stacks; valid range: 10-18).

  At ``bits=18`` the kernel reserves roughly 130 MB of vmalloc memory
  for the element pool. Each ``open()`` of ``stack_map_bin`` may
  briefly allocate a similar amount for a snapshot. The cap is set
  intentionally to bound memory usage.

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

    entries:      2500 / 16384
    table_size:   32768
    successes:    148923
    drops:        0
    success_rate: 100%

To reset the stack map (tracing must be stopped first)::

    echo 0 > /sys/kernel/debug/tracing/tracing_on
    echo 0 > /sys/kernel/debug/tracing/stack_map

Reset returns ``-EBUSY`` if tracing is currently active, or if another
reset is already in progress.

Boot-time activation
====================

The stackmap option can be enabled from the kernel command line::

    trace_options=stackmap,stacktrace

Trace events that fire before the tracefs filesystem is initialized
(``fs_initcall`` time) fall back to recording full stack traces; once
``ftrace_stackmap_create()`` runs, subsequent events are deduplicated.
The crossover is automatic and lossless — no events are dropped, but
early-boot stacks recorded before the crossover are not deduplicated.

Tracefs Nodes
=============

The stack_map files are owned by root and not world-readable
(``stack_map``: 0640; ``stack_map_stat`` and ``stack_map_bin``: 0440).

``stack_map``
    Text export of all deduplicated stacks with symbol resolution.
    Writing ``0`` or ``reset`` clears all entries (only when tracing
    is stopped).

``stack_map_stat``
    Statistics: entries (allocated unique stacks), table_size,
    successes (events served), drops (events that fell back to
    full-stack recording), and success_rate. Drops accumulate when
    the element pool is exhausted; once that happens, slots that
    won the cmpxchg but failed to allocate an element remain
    "claimed but empty" and increase probe pressure for any future
    insert hashing to the same bucket. Reset (when tracing is
    stopped) clears these gravestones.

``stack_map_bin``
    Binary export for efficient userspace consumption. Format:

    - Header (16 bytes): magic(u32) + version(u32) + nr_stacks(u32) + reserved(u32)
    - Per stack: stack_id(u32) + nr(u32) + ref_count(u32) + reserved(u32) + ips(u64 × nr)

    All fields are written in the kernel's native byte order.
    Userspace tools detect endianness by reading the magic value.
    Magic: ``0x464D5342`` ('FSMB'), Version: 2.

    The export is a best-effort snapshot allocated at ``open()``;
    concurrent inserts during the snapshot may be truncated. A
    bounds check ensures no overflow.

Design
======

The stack map is modeled after ``tracing_map.c`` (used by hist triggers),
using a lock-free design based on Dr. Cliff Click's non-blocking hash table
algorithm:

- **Lookup/Insert**: Lock-free via ``cmpxchg``, safe in NMI/IRQ/any context
- **Memory**: Pre-allocated element pool, zero allocation on the hot path
  (no GFP_ATOMIC failures under memory pressure)
- **Collision**: Linear probing with a 2x over-provisioned table; probe
  length is bounded so worst-case insert/lookup is O(1)
- **Scope**: Currently supports the global trace instance
- **Hash**: 32-bit jhash with a per-instance random seed; full ``memcmp``
  confirms matches

Deduplication is best-effort, not strict: if two CPUs race in the
insert path with the same ``key_hash`` (i.e. the same stack), the
``cmpxchg`` loser advances by one slot and may insert the same stack
again. Under heavy contention this can produce a small number of
duplicate entries for the same stack; ``ref_count`` is then split
across the duplicates. Total memory is still bounded by the element
pool size, and lookup correctness is unaffected (each duplicate is
a self-consistent entry with its own ``stack_id``). The trade-off is
intentional and keeps the hot path lock-free.

Performance
===========

Typical results on an aarch64 SMP system (function tracer, 2 seconds):

- Unique stacks: ~3000
- Dedup rate: 84-98% (depends on workload diversity)
- Ring buffer savings: ~80% for stack data
- Overhead per event: ~50ns (one jhash + hash table lookup)
