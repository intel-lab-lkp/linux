.. SPDX-License-Identifier: GPL-2.0

====
Swap
====

Swap allows the kernel to evict anonymous pages (those not backed by a
file) to a swap device so that physical memory can be reused.  When the
pages are needed again, they are read back in.  The swap subsystem spans
several files: ``mm/swapfile.c`` manages swap devices, ``mm/swap_state.c``
implements the swap cache, ``mm/page_io.c`` handles disk I/O, and
``mm/zswap.c`` provides an optional compressed cache layer.

.. contents:: :local:

Swap Entries
============

A swap entry is a compact identifier that encodes which swap device to use
and the offset within that device.  When a page is swapped out, its page
table entry is replaced with a swap entry so that the kernel knows where to
find the data on a subsequent fault.  Swap entries are also used internally
as keys into the swap cache.

Swap Devices
============

A swap device is a disk partition or file registered with the ``swapon()``
system call.  Each device is described by a ``swap_info_struct`` that holds
the device's extent map, cluster state, and per-CPU allocation hints.

The kernel maps virtual swap offsets to disk locations through a tree of
``swap_extent`` structures.  For raw partitions the mapping is trivial
(one extent covering the whole device); for swap files the mapping follows
the file's block layout on disk.

Cluster Allocation
------------------

Swap space is allocated in clusters (groups of contiguous slots, typically
32 pages).  Each cluster tracks which slots are free and whether it has
pending discards.  Per-CPU hints point to the most recently used cluster
so that allocations from the same CPU tend to land in the same cluster,
improving spatial locality for both SSDs and spinning disks.

When a cluster is full, the allocator scans for a new one.  Under heavy
swap pressure, it may also reclaim slots from full clusters if the pages
they reference have since been freed or swapped back in.

TRIM / Discard
--------------

For SSD-backed swap, the kernel can issue discard (TRIM) commands when
swap slots are freed.  This is batched per-cluster: once all slots in a
cluster are free, a single discard is issued for the entire range.  This
avoids the overhead of per-page discards while still informing the device
that the blocks are unused.

Swap counts
-----------

Each swap slot has a reference count tracking how many page table entries
point to it (due to ``fork()`` and copy-on-write).  For slots referenced
by very many processes, a continuation mechanism extends the counter
beyond its inline capacity.

Swap Cache
==========

The swap cache keeps recently swapped-in (or about to be swapped-out)
pages in memory, indexed by their swap entry.  This serves several
purposes:

- **Deduplication**: when multiple processes share a swapped page (via
  ``fork()``), only one copy is read from disk; subsequent faults find
  the page in the swap cache.
- **Write coalescing**: if a page is modified and swapped out again before
  the previous write completes, the swap cache absorbs the update without
  issuing a new write.
- **Readahead**: when one page is swapped in, adjacent swap entries are
  speculatively read to exploit spatial and temporal locality.

The swap cache is implemented as a per-cluster array of pointers
(the "swap table"), providing O(1) lookup by swap entry.
See also Documentation/mm/swap-table.rst.

Readahead
---------

Swap readahead pre-fetches pages from swap before they are faulted in.
Two strategies are used:

- **Cluster readahead**: reads a window of swap entries around the faulting
  entry, betting on spatial locality in the swap device.
- **VMA readahead**: uses the virtual address layout to predict which swap
  entries will be needed next, which is more effective when the access
  pattern follows the process's address space layout rather than the swap
  device layout.

``vm.page-cluster`` controls the readahead window size (as a power of two).

Compressed Swap (zswap)
=======================

zswap (``mm/zswap.c``) is an optional write-behind compressed cache that
sits between the reclaim path and the swap device.  When reclaim evicts a
page, zswap attempts to compress it and store the compressed data in a
RAM-based pool (using the zsmalloc allocator).

If the page is faulted back in before the pool fills, no disk I/O occurs —
the page is decompressed directly from memory.  This is significantly
faster than reading from even an SSD.

Pool Management
---------------

Each zswap pool pairs a compression algorithm (lzo, lz4, zstd, etc.) with
a zsmalloc memory pool.  Per-CPU compression contexts avoid lock
contention during compression and decompression.

When the pool reaches its size limit (controlled by
``/sys/module/zswap/parameters/max_pool_percent``), the oldest entries are
evicted: zswap writes them out to the backing swap device, falling back to
the normal swap I/O path.  An LRU list tracks entries for this purpose.

Writeback
---------

zswap writeback decompresses the page, allocates a swap slot, and writes
the uncompressed page to the swap device.  This is the slow path —
ideally most pages are either faulted back in from the compressed cache
or freed without ever reaching disk.

Zero-Filled Pages
=================

``mm/page_io.c`` maintains a bitmap (``swap_zeromap``) tracking swap slots
that contained zero-filled pages.  When such a page is swapped in, the
kernel returns a zeroed page without performing any I/O.  When a zero
page is swapped out, the bitmap bit is set instead of issuing a write.
This optimization is significant for workloads that allocate large amounts
of memory that is never written to.

Swap I/O
========

``mm/page_io.c`` handles the mechanics of reading and writing pages to
swap.  The I/O path checks three layers in order before falling through to
disk:

1. The zero page bitmap — if the slot is known to be zero-filled, return
   a zeroed page (read) or set the bit (write) with no I/O.
2. zswap — if enabled, attempt to store/load the page in the compressed
   cache.
3. Block I/O — submit a bio to the swap device, using the swap extent
   tree to map the slot to a disk sector.

For swap files (as opposed to raw partitions), the I/O follows the
filesystem's block mapping rather than issuing direct device I/O.
