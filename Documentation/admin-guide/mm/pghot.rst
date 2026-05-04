.. SPDX-License-Identifier: GPL-2.0

=================================
PGHOT: Hot Page Tracking Tunables
=================================

Overview
========
The PGHOT subsystem tracks frequently accessed pages in lower-tier memory and
promotes them to faster tiers. It uses per-PFN hotness metadata and asynchronous
migration via per-node kernel threads (kmigrated).

This document describes tunables available via **debugfs** and **sysctl** for
PGHOT.

Debugfs Interface
=================
Path: /sys/kernel/debug/pghot/

1. **enabled_sources**
   - Bitmask to enable/disable hotness sources.
   - Bits:
     - 0: Hint faults (value 0x1)
     - 1: Hardware hints (value 0x2)
   - Default: 0 (disabled)
   - Example:
     # echo 0x3 > /sys/kernel/debug/pghot/enabled_sources
     Enables all sources.

2. **target_nid**
   - Toptier NUMA node ID to which hot pages should be promoted when source
     does not provide nid. Used when hotness source can't provide accessing
     NID or when the tracking mode is default.
   - Default: 0
   - Example:
     # echo 1 > /sys/kernel/debug/pghot/target_nid

3. **freq_threshold**
   - Minimum access frequency before a page is marked ready for promotion.
   - Range: 1 to 3
   - Default: 2
   - Example:
     # echo 3 > /sys/kernel/debug/pghot/freq_threshold

4. **kmigrated_sleep_ms**
   - Sleep interval (ms) for kmigrated thread between scans.
   - Default: 100

5. **kmigrated_batch_nr**
   - Maximum number of folios migrated in one batch.
   - Default: 512

Sysctl Interface
================
1. pghot_promote_freq_window_ms

Path: /proc/sys/vm/pghot_promote_freq_window_ms

- Controls the time window (in ms) for counting access frequency. A page is
  considered hot only when **freq_threshold** number of accesses occur with
  this time period.
- Default: 3000 (3 seconds)
- Example:
  # sysctl vm.pghot_promote_freq_window_ms=3000

Vmstat Counters
===============
Following vmstat counters provide some stats about pghot subsystem.

Path: /proc/vmstat

1. **pghot_recorded_accesses**
   - Number of total hot page accesses recorded by pghot.

2. **pghot_recorded_hintfaults**
   - Number of recorded accesses reported by NUMA Balancing based
     hotness source.

3. **pghot_recorded_hwhints**
   - Number of recorded accesses reported by hwhints source.
