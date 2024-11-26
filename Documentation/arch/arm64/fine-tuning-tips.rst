.. SPDX-License-Identifier: GPL-2.0

================
fine-tuning tips
================

This file contains some fine-tuning tips for arm64 machines.
These tips do not gurantee that you can get better performance,
but you can try them with your workload.

rodata=noalias
----------------
It can provide us more block mappings and contiguous hits
to map the linear region which minimizes the TLB footprint.

slab_strict_numa
----------------
In NUMA, it will provide the local memory allocation by SLUB.

CONFIG_SCHED_CLUSTER
----------------
Some arm64 machines have cpu core cluster, enable it may
helps you get better performance.
