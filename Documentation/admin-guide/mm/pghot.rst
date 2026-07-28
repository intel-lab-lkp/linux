.. SPDX-License-Identifier: GPL-2.0

================================================
pghot: Hot Page Tracking and Promotion Subsystem
================================================

Overview
========
The pghot subsystem tracks frequently accessed pages in lower-tier memory and
promotes them to faster tiers. It uses per-PFN hotness metadata and asynchronous
migration via per-node kernel threads (kmigrated).

This document describes tunables available via **debugfs** and **sysctl** for
pghot.

Debugfs Interface
=================
Path: /sys/kernel/debug/pghot/

1. **kmigrated_sleep_ms**
   - Sleep interval (ms) for kmigrated thread between scans.
   - Default: 100

2. **kmigrated_batch_nr**
   - Maximum number of folios migrated in one batch.
   - Default: 512

Sysctl Interface
================
1. pghot_enabled_sources

Path: /proc/sys/vm/pghot_enabled_sources

- Bitmask to enable/disable hotness sources.
- Bits:
  - 0: Hint faults (value 0x1)
  - 1: Hardware hints (value 0x2)
- Default: 0x1 (hint faults enabled, hardware hints disabled). Hint faults
  are enabled by default so that selecting the NUMA Balancing memory
  tiering mode (kernel.numa_balancing=2) promotes hot pages without any
  additional opt-in. Hint faults are fed to pghot only while tiering mode
  is selected, so this default has no effect otherwise.
- Example:
  # echo 0x3 > /proc/sys/vm/pghot_enabled_sources
  Enables both hint faults and hwhints sources

2. pghot_target_nid

Path: /proc/sys/vm/pghot_target_nid

- Toptier NUMA node ID to which hot pages should be promoted when source
  does not provide nid. Used when hotness source can't provide accessing
  NID or when the tracking mode is default.
- Default: 0
- Example:
  # echo 1 > /proc/sys/vm/pghot_target_nid

3. pghot_freq_threshold

Path: /proc/sys/vm/pghot_freq_threshold

- Minimum access frequency before a page is marked ready for promotion.
  Range is 1 to 3 in default mode and 1 to 7 in precision mode.
- Default: 2
- Example:
  # sysctl vm.pghot_freq_threshold=1

4. pghot_promote_freq_window_ms

Path: /proc/sys/vm/pghot_promote_freq_window_ms

- Controls the time window (in ms) for counting access frequency. A page is
  considered hot only when **pghot_freq_threshold** number of accesses occur
  with this time period.
- Default: 3000 (3 seconds) in default mode and 5000 (5s) in precision mode.
- Example:
  # sysctl vm.pghot_promote_freq_window_ms=3000

pghot Vmstat Counters
=====================
Following vmstat counters provide some stats about pghot subsystem.

Path: /proc/vmstat

1. **pghot_recorded_accesses**
   - Number of total hot page accesses recorded by pghot.

2. **pghot_recorded_hintfaults**
   - Number of recorded accesses reported by NUMA Balancing based
     hotness source.

3. **pghot_recorded_hwhints**
   - Number of recorded accesses reported by hwhints source.

NUMA Hint Faults Source
=======================
The "hint faults" source is the tiering mode of NUMA Balancing
which acts as a source of page hotness to pghot.

It is controlled by the NUMA_BALANCING_TIERING config option, which
gates the memory tiering mode (NUMA_BALANCING_MEMORY_TIERING) of NUMA
Balancing. At runtime that mode must additionally be selected through
the kernel.numa_balancing sysctl.

This source is enabled by default through the hint faults bit (0x1) of
**pghot_enabled_sources**, so selecting the tiering mode alone is enough
to promote hot pages. It can be disabled at runtime by clearing that bit:

# echo 0x0 > /proc/sys/vm/pghot_enabled_sources

