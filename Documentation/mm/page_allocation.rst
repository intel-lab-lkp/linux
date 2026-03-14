.. SPDX-License-Identifier: GPL-2.0

===============
Page Allocation
===============

The page allocator is the kernel's primary interface for obtaining and
releasing physical page frames.  It is built on the buddy algorithm and
implemented in ``mm/page_alloc.c``.

.. contents:: :local:

Buddy Allocator
===============

Free pages are grouped by order (power-of-two size) in per-zone
``free_area`` arrays, where order 0 is a single page and the maximum is
``MAX_PAGE_ORDER``.  To satisfy an allocation of order N, the allocator
looks for a free block of that order.  If none is available, it splits a
higher-order block in half repeatedly until one of the right size is
produced.  When a page is freed, the allocator checks whether its "buddy"
(the adjacent block of the same order) is also free; if so, the two are
merged into a block of the next higher order.  This coalescing continues
as high as possible, rebuilding large contiguous blocks over time.

Migratetypes
============

Each pageblock (typically 2MB on x86) carries a migratetype tag that
describes the kind of allocations it serves:

- **MIGRATE_UNMOVABLE**: kernel allocations that cannot be relocated
  (slab objects, page tables).
- **MIGRATE_MOVABLE**: user pages and other content that can be migrated
  or reclaimed (used by compaction and memory hot-remove).
- **MIGRATE_RECLAIMABLE**: caches that can be dropped under pressure
  (page cache, dentries).
- **MIGRATE_CMA**: reserved for the contiguous memory allocator;
  behaves as movable when not in use by CMA.
- **MIGRATE_ISOLATE**: temporarily prevents allocation from a range,
  used during compaction and memory hot-remove.

When a free list for the requested migratetype is empty, the allocator
falls back to other types in a defined order.  It may also "steal" an
entire pageblock from another migratetype if it needs to take pages from
it, changing the pageblock's tag to reduce future fragmentation.  This
fallback and stealing logic is a key mechanism for balancing fragmentation
against allocation success.

Per-CPU Pagesets
================

Most order-0 allocations are served from per-CPU page lists (PCP) rather
than the global ``free_area``.  This avoids taking the zone lock on the
common path, which is critical for scalability on large systems.

Each CPU maintains lists of free pages grouped by migratetype.  Pages are
moved between the per-CPU lists and the buddy in batches.  The batch size
and high watermark for each per-CPU list are tuned based on zone size and
the number of CPUs.

When a per-CPU list is empty, a batch of pages is taken from the buddy.
When it exceeds its high watermark, excess pages are returned.
``lru_add_drain()`` and ``drain_all_pages()`` flush per-CPU lists when
the system needs an accurate count of free pages, such as during memory
hot-remove.

GFP Flags
=========

Every allocation request carries a set of GFP (Get Free Pages) flags,
defined in ``include/linux/gfp.h``, that describe what the allocator is
allowed to do:

Zone selection
  ``__GFP_DMA``, ``__GFP_DMA32``, ``__GFP_HIGHMEM``, ``__GFP_MOVABLE``
  select the highest zone the allocation may use.  ``gfp_zone()`` maps
  flags to a zone type; the allocator then scans the zonelist from that
  zone downward.

Reclaim and compaction
  ``__GFP_DIRECT_RECLAIM`` allows the allocator to invoke direct reclaim.
  ``__GFP_KSWAPD_RECLAIM`` allows it to wake kswapd.  Together these form
  ``GFP_KERNEL``, the most common flag combination.

Retry behavior
  ``__GFP_NORETRY`` gives up after one attempt at reclaim.
  ``__GFP_RETRY_MAYFAIL`` retries as long as progress is being made.
  ``__GFP_NOFAIL`` never fails — the allocator retries indefinitely,
  which is appropriate only for small allocations in contexts that
  cannot handle failure.

Migratetype
  ``__GFP_MOVABLE`` and ``__GFP_RECLAIMABLE`` select the migratetype.
  ``gfp_migratetype()`` maps flags to the appropriate type.

Allocation Path
===============

Fast path
---------

``get_page_from_freelist()`` is the fast path.  It walks the zonelist
(an ordered list of zones across all nodes, starting with the preferred
node) looking for a zone with enough free pages above its watermarks.
When it finds one, it pulls a page from the per-CPU list or buddy.

The fast path also checks NUMA locality, cpuset constraints, and memory
cgroup limits.  If no zone can satisfy the request, control passes to
the slow path.

Slow path
---------

``__alloc_pages_slowpath()`` engages increasingly aggressive measures:

1. Wake kswapd to begin background reclaim.
2. Attempt direct reclaim — the allocating task itself reclaims pages.
3. Attempt direct compaction — migrate pages to create contiguous blocks
   (for high-order allocations).
4. Retry with lowered watermarks if progress was made.
5. As a last resort, invoke the OOM killer (see Documentation/mm/oom.rst).

Each step may succeed, in which case the allocation is retried.  The
``__GFP_NORETRY``, ``__GFP_RETRY_MAYFAIL``, and ``__GFP_NOFAIL`` flags
control how far down this chain the allocator goes.

Watermarks
==========

Each zone maintains min, low, high, and promo watermarks that govern
reclaim behavior:

- **min**: below this level, only emergency allocations (those with
  ``__GFP_MEMALLOC`` or from the OOM victim) can proceed.  Direct reclaim
  may be triggered.
- **low**: when free pages drop below this level, kswapd is woken to
  begin background reclaim.
- **high**: kswapd stops reclaiming when free pages reach this level.
  The zone is considered "balanced."
- **promo**: used for NUMA memory tiering; controls when kswapd stops
  reclaiming when tier promotion is enabled.

The min watermark is derived from ``vm.min_free_kbytes``.  The distance
between watermarks is scaled by ``vm.watermark_scale_factor``.

Watermark boosting temporarily raises watermarks after a pageblock is
stolen from a different migratetype, increasing reclaim pressure to
recover from the fragmentation event.

High-Atomic Reserves
--------------------

The allocator reserves a small number of high-order pageblocks for atomic
(non-sleeping) allocations.  When a high-order atomic allocation succeeds
from unreserved memory, the containing pageblock is moved to the reserve.
When memory pressure is high, unreserved pageblocks are released back to
the general pool.

Compaction
==========

Memory compaction (``mm/compaction.c``) creates contiguous free blocks for
high-order allocations by relocating movable pages.  It runs two scanners
across a zone: one walks from the bottom to find movable in-use pages, the
other walks from the top to find free pages.  Movable pages are migrated
to the free locations, consolidating free space in the middle.

Sync modes
----------

Compaction operates in three modes:

- **ASYNC**: skips pages that require blocking to isolate or migrate.
  Used in the allocation fast path and by kcompactd.
- **SYNC_LIGHT**: allows some blocking but skips pages under writeback.
- **SYNC**: allows full blocking.  Used when direct compaction is the
  last option before OOM.

Deferral
--------

When compaction fails for a given order in a zone, it is deferred for an
exponentially increasing number of attempts to avoid wasting CPU on zones
that are too fragmented.  A successful high-order allocation resets the
deferral.

kcompactd
---------

Each node has a kcompactd kernel thread that performs background
compaction.  It is woken when kswapd finishes reclaiming but high-order
allocations are still failing due to fragmentation.  kcompactd runs at
low priority to avoid interfering with foreground work.

Capture Control
---------------

During direct compaction, the allocator uses a capture mechanism: when
compaction frees a block of the right order, the allocation can claim it
immediately rather than racing with other allocators on the free list.

Page Isolation
==============

``mm/page_isolation.c`` supports marking pageblocks as ``MIGRATE_ISOLATE``
to prevent new allocations from those ranges.  Existing free pages are
moved out; the caller then migrates all in-use pages away.  Once the range
is fully evacuated, it can be used for a contiguous allocation or taken
offline.

This mechanism is used by:

- **CMA** (contiguous memory allocator): reserves regions at boot for
  device drivers that need physically contiguous buffers.  The reserved
  pages serve normal movable allocations until a CMA allocation claims
  the range.
- **Memory hot-remove**: isolates a memory block before offlining it.
- **alloc_contig_range()**: general-purpose contiguous allocation used
  by gigantic huge pages and other subsystems.

The isolation process must handle pageblocks that straddle the requested
range boundaries, compound pages (huge pages, THP) that overlap the
boundary, and unmovable pages that prevent evacuation.
