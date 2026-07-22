.. SPDX-License-Identifier: GPL-2.0

======================
Cache Aware Scheduling
======================

Cache aware scheduling (``CONFIG_SCHED_CACHE``) aggregates the threads
of a process on a preferred last level cache (LLC) domain when that is
expected to improve cache locality: the scheduler samples the per-LLC
CPU occupancy of every multi-threaded process and biases load balance
towards the LLC the process already uses the most.

Aggregation is skipped for processes that would not benefit from it:
single-threaded processes, processes with more active threads than the
LLC has cores, and processes whose memory footprint exceeds the LLC
size (footprint tracking requires ``CONFIG_NUMA_BALANCING``).

Global control (debugfs)
========================

The feature is controlled globally through
``/sys/kernel/debug/sched/llc_balancing/``:

``enabled``
	Boolean. When on (the default), every process participates
	unless it opted out via ``prctl(PR_SCHED_CACHE)``; when off,
	the feature is inactive for every process.

``aggr_tolerance_nr``
	Scales the number of cores of an LLC before it is compared to
	the process's number of active threads. 0 disables aggregation
	through this gate, 1 is the strict default, values of 100 or
	more disable the thread count check entirely.

``aggr_tolerance_size``
	Scales the LLC size (by 256 per step) before it is compared to
	the process's memory footprint. Same 0/1/100 semantics as
	``aggr_tolerance_nr``. Only effective with
	``CONFIG_NUMA_BALANCING``.

``overaggr_pct``
	The LLC utilization threshold, in percent of the LLC capacity,
	below which the LLC is considered idle enough to aggregate more
	tasks into it (default 50).

``imb_pct``
	Utilization imbalance hysteresis, in percent, used when
	comparing two LLCs (default 20).

``epoch_period``, ``epoch_affinity_timeout``
	Occupancy sampling period (jiffies) and the number of epochs
	after which a process's LLC preference expires.

Per-process control (prctl)
===========================

``prctl(PR_SCHED_CACHE)`` overrides the global knobs for one process.
The attributes live on the process's address space (``mm_struct``):
they are shared by all threads of the process, and a single prctl
command implements both directions::

	prctl(PR_SCHED_CACHE, PR_SCHED_CACHE_SET, attr, value, 0);
	prctl(PR_SCHED_CACHE, PR_SCHED_CACHE_GET, attr, &value, 0);

``PR_SCHED_CACHE_GET`` stores the raw attribute value into the ``int``
pointed to by the fourth argument and returns 0. A value attribute
that was never set reads back as -1 (== ``PR_SCHED_CACHE_DEFAULT``),
meaning "follow the global default"; the ``PR_SCHED_CACHE_INHERIT``
mask reads back as the mask itself (default 0). Setting an attribute
to ``PR_SCHED_CACHE_DEFAULT`` resets it (for the inherit mask this
clears all bits).

Attributes:

``PR_SCHED_CACHE_ENABLE``
	0 or 1. A process can opt out of the globally enabled feature
	with 0; 1 (like the default) participates. It cannot activate
	the feature when the global knob is off or the hardware has a
	single LLC.

``PR_SCHED_CACHE_AGGR_TOLERANCE_NR``
	0..100. Per-process version of ``aggr_tolerance_nr``.

``PR_SCHED_CACHE_AGGR_TOLERANCE_SIZE``
	0..100. Per-process version of ``aggr_tolerance_size``.

``PR_SCHED_CACHE_OVERAGGR_PCT``
	0..1000. Per-process version of ``overaggr_pct``. It applies
	where a task's own migration is admitted; group-level load
	balance statistics keep using the global value, because they
	aggregate tasks of many processes.

``PR_SCHED_CACHE_INHERIT``
	A bitmask selecting which attributes a new address space
	created by execve() keeps: ``PR_SCHED_CACHE_INHERIT_ENABLE``,
	``..._AGGR_TOLERANCE_NR``, ``..._AGGR_TOLERANCE_SIZE`` and
	``..._OVERAGGR_PCT``. The mask is per thread and is itself kept
	across fork() and execve().

Inheritance
===========

fork() always copies all attributes to the child, like other process
properties. execve() resets attributes to the default unless the
corresponding ``PR_SCHED_CACHE_INHERIT`` bit is set in the calling
thread.

This allows a numactl-like launcher to configure a workload::

	prctl(PR_SCHED_CACHE, PR_SCHED_CACHE_SET,
	      PR_SCHED_CACHE_AGGR_TOLERANCE_NR, 100, 0);
	prctl(PR_SCHED_CACHE, PR_SCHED_CACHE_SET,
	      PR_SCHED_CACHE_INHERIT,
	      PR_SCHED_CACHE_INHERIT_AGGR_TOLERANCE_NR, 0);
	execve(workload, ...);

Errors
======

``EINVAL``
	Unknown sub-command or attribute, value out of range, nonzero
	unused argument, calling task has no mm, or the kernel was
	built without ``CONFIG_SCHED_CACHE``.
``EFAULT``
	Invalid pointer passed to ``PR_SCHED_CACHE_GET``.
