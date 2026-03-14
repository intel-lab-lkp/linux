.. SPDX-License-Identifier: GPL-2.0

============
Page Reclaim
============

Page reclaim frees memory by evicting pages that can be reloaded from disk
or regenerated.  File-backed pages are dropped (clean) or written back
(dirty); anonymous pages are swapped out.  The bulk of the implementation
is in ``mm/vmscan.c``.

.. contents:: :local:

When Reclaim Runs
=================

Reclaim is triggered in two ways:

- **kswapd**: a per-node kernel thread that runs in the background when
  free pages in any zone drop below the low watermark.  It reclaims until
  free pages reach the high watermark, then sleeps.

- **Direct reclaim**: when an allocation cannot be satisfied even after
  kswapd has been woken, the allocating task reclaims pages synchronously
  in its own context.  This adds latency to the allocation but is necessary
  when background reclaim cannot keep up.

Reclaim Priority
================

The reclaim path operates at decreasing priority levels (from
``DEF_PRIORITY`` down to 0).  At each level, a larger fraction of the LRU
lists is scanned.  At the default priority, only 1/4096th of pages are
considered; at priority 0, the entire list is scanned.

If a full scan at priority 0 still does not free enough memory, the OOM
killer is invoked (see Documentation/mm/oom.rst).  This escalation
prevents the system from spinning indefinitely in reclaim.

Scan Control
============

Each reclaim invocation is parameterized by a ``struct scan_control`` that
captures the allocation context: which GFP flags were used, how many pages
are needed, which node or memory cgroup to reclaim from, and whether
writeback or swap are allowed.  This struct threads through the entire
reclaim stack, ensuring consistent policy at every level.

LRU Lists
=========

Each ``lruvec`` (one per node, or per node and memory cgroup combination)
maintains lists of pages ordered by access recency.

Classic LRU
-----------

The classic scheme uses four LRU lists per lruvec: active and inactive for
both anonymous and file-backed pages.  This approximates a second-chance
(clock) algorithm:

- Pages start on the inactive list when first allocated.
- If accessed again while on the inactive list, they are promoted to the
  active list.
- Reclaim scans the inactive list and evicts pages that have not been
  recently accessed.
- To prevent the active list from growing without bound, pages are
  periodically demoted from active to inactive.

The split between anonymous and file-backed lists allows the reclaim path
to balance eviction pressure between the two types based on their relative
cost.  Swapping anonymous pages is generally more expensive than dropping
clean file pages, so the scanner adjusts the ratio using IO cost
accounting and the ``vm.swappiness`` tunable.

Multi-Gen LRU
-------------

The multi-gen LRU is an alternative reclaim algorithm that groups pages
into generations by access time rather than a simple active/inactive
split.  It is documented separately in Documentation/mm/multigen_lru.rst.

LRU Batching
------------

To avoid taking the lruvec lock on every page access, LRU operations are
batched per-CPU (``mm/swap.c``).  Functions like ``folio_add_lru()`` and
``folio_mark_accessed()`` queue pages into per-CPU folio batches that are
drained to the actual LRU lists periodically or when the batch is full.
This batching is critical for scalability on systems with many CPUs.

Reclaiming Pages
================

The core reclaim loop (``shrink_node()``) divides its work between page
cache / anonymous pages and slab caches.  For each lruvec, it scans the
inactive LRU lists, evaluating each page:

- **Clean file pages** can be dropped immediately — they can be re-read
  from disk.
- **Dirty file pages** are queued for writeback.  Reclaim typically skips
  them and returns later, but under severe pressure it may wait for
  writeback to complete.
- **Anonymous pages** are swapped out if swap space is available and
  ``vm.swappiness`` allows it.
- **Mapped pages** require TLB invalidation (unmapping) before they can
  be freed.  The rmap (reverse mapping) system is used to find and
  remove all page table entries pointing to the page.
- **Unevictable pages** (locked with ``mlock()``) are skipped entirely.
  See Documentation/mm/unevictable-lru.rst.

Memory Cgroup Reclaim
---------------------

When memory cgroup limits are exceeded, reclaim targets only the pages
belonging to that cgroup.  Each memory cgroup has its own lruvec per node,
so the scanner can isolate its pages without disturbing the rest of the
system.  ``try_to_free_mem_cgroup_pages()`` is the entry point for
cgroup-scoped reclaim.

NUMA Demotion
-------------

On systems with tiered memory (e.g., fast DRAM and slower persistent
memory), reclaim can demote pages to a slower tier instead of evicting
them.  This keeps the data in memory but frees the faster tier for
actively accessed pages.

Shrinkers
=========

Besides page cache and anonymous pages, kernel caches (dentries, inodes,
and driver-specific caches) are reclaimed through the shrinker interface
(``mm/shrinker.c``).  A shrinker registers two callbacks:

- ``count_objects()``: report how many objects are reclaimable.
- ``scan_objects()``: free up to a requested number of objects.

The reclaim path calls all registered shrinkers proportionally to the
amount of reclaimable memory they report.  Shrinkers are NUMA-aware: on
NUMA systems, each shrinker is called with the node being reclaimed so it
can prioritize freeing objects local to that node.

Per-memcg shrinker tracking uses bitmap arrays (``shrinker_info``) so that
the reclaim path only invokes shrinkers that actually have objects in the
target cgroup, avoiding unnecessary work when there are many cgroups.

Working Set Detection
=====================

When a page is evicted, a compact shadow entry is stored in its place in
the page cache or swap cache.  The shadow records the eviction timestamp
(in terms of the lruvec's nonresident age counter) and the cgroup and
node that owned the page.

If the page is faulted back in (a "refault"), the shadow entry allows the
kernel to compute the *refault distance* — how many other pages were
activated or evicted between this page's eviction and its refault.  If the
refault distance is shorter than the size of the inactive list, the page
was part of the active working set and is immediately activated rather
than placed on the inactive list.  This reduces thrashing by protecting
frequently accessed pages that would otherwise be repeatedly evicted and
refaulted.

Shadow entries consume a small amount of memory.  To prevent them from
accumulating indefinitely, a shrinker reclaims shadow entries from page
cache radix tree nodes that contain only shadows and no actual pages.

This logic is implemented in ``mm/workingset.c``.
