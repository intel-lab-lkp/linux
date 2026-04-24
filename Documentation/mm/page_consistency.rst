.. SPDX-License-Identifier: GPL-2.0

=======================
Page Consistency Checker
=======================

The page consistency checker is a debugging feature that uses dual
complementary bitmaps to detect corruption in page allocation tracking.
It maintains the invariant that for every bit position, the primary
bitmap value equals the bitwise complement of the secondary bitmap value.

Overview
========

Memory corruption can silently flip bits in kernel data structures,
leading to difficult-to-diagnose failures. The page consistency checker
addresses this by maintaining redundant tracking of page allocation
state. Any single-bit corruption in either bitmap will cause a detectable
inconsistency, allowing the corruption to be caught rather than causing
silent data corruption or mysterious crashes later.

The bitmaps are flat, covering the entire PFN range from
``memblock_start_of_DRAM()`` to ``memblock_end_of_DRAM()`` including any
holes in physical memory. This is a deliberate design choice: simple
``pfn - min_pfn`` indexing is trivially auditable, which matters for a
safety mechanism. Sparse or section-aware indexing would add auxiliary
data structures that could themselves be subject to corruption. See
`Limitations`_ for a detailed analysis of memory overhead including
holes.

The approach is based on NVIDIA safety research and is
particularly useful for safety-critical systems requiring Freedom From
Interference (FFI) guarantees per ISO 26262 (ASIL-D) and IEC 61508
(SIL-3).

Algorithm
=========

The checker maintains two bitmaps tracking page allocation state:

Primary bitmap
  Bit set to 1 when page is allocated, 0 when free.

Secondary bitmap
  Bit set to 0 when page is allocated, 1 when free.

The invariant that must always hold is::

    primary[bit] == ~secondary[bit]

When a page is allocated, the checker sets the bit in the primary bitmap
and clears it in the secondary bitmap. When freed, it clears in primary
and sets in secondary. If the operation finds the bit already in the
expected final state, a double-allocation or double-free has occurred.

Full validation can be performed by checking that every word in the
primary bitmap equals the bitwise complement of the corresponding word
in the secondary bitmap.

Concurrency Handling
====================

The dual-bitmap update operations (set/clear) modify both bitmaps with
separate atomic operations. This creates a brief window where a concurrent
validation could observe a transient inconsistency.

The implementation handles this by retrying validation when an inconsistency
is detected. Real memory corruption is persistent and will fail all retries.
Transient inconsistencies from concurrent updates resolve quickly and pass
on retry.

Double-Free Detection
=====================

Double-free detection is deferred until the system is fully running. During
boot, free_reserved_area() and free_initmem() release memory pages that were
never allocated through the buddy allocator. These would appear as double-frees
but are expected behavior.

The checker uses ``system_state >= SYSTEM_RUNNING`` to determine when boot
is complete. This state is reached only after all init memory has been freed,
ensuring no false positives from legitimate boot-time freeing. Any attempt to
free a page that is not marked as allocated after this point will be flagged
as a violation.

Configuration
=============

The feature is controlled by two Kconfig options:

``CONFIG_DEBUG_PAGE_CONSISTENCY``
  Enable the page consistency checker. Memory overhead is two bits per
  PFN in the spanned range (start to end of DRAM, including holes),
  roughly 4 MB total for a 64 GB system. When this option is disabled,
  the allocator hooks compile away. When enabled, a static key gates
  tracking until initialization succeeds.

``CONFIG_DEBUG_PAGE_CONSISTENCY_PANIC``
  When enabled, the kernel will panic immediately upon detecting a
  consistency violation. When disabled, a warning with a stack trace
  is emitted and execution continues. Safety-critical systems should
  enable this option.

Debugfs Interface
=================

When CONFIG_DEBUG_FS is enabled, the checker exposes files under
``/sys/kernel/debug/page_consistency/``:

``stats``
  Read-only file showing tracking statistics::

    pages_tracked:       12345
    alloc_count:         67890
    free_count:          55545
    violations_detected: 0
    bitmap_size_bits:    1048576
    pfn_range:           [256-1048831]

``validate``
  Write-only file. Writing any value triggers a full validation of
  all bitmap words. Returns success if all words are consistent,
  or -EIO if any violations are found.

Usage
=====

To use the page consistency checker:

1. Enable ``CONFIG_DEBUG_PAGE_CONSISTENCY`` in your kernel configuration.

2. Optionally enable ``CONFIG_DEBUG_PAGE_CONSISTENCY_PANIC`` if you want
   the kernel to halt immediately upon detecting corruption.

3. Boot the kernel. The checker will automatically initialize and begin
   tracking page allocations.

4. Monitor statistics via debugfs::

     cat /sys/kernel/debug/page_consistency/stats

5. Trigger manual validation::

     echo 1 > /sys/kernel/debug/page_consistency/validate

Limitations
===========

As described in `Overview`_, the bitmaps use a flat layout covering the
entire spanned PFN range, including any holes. Bits corresponding to
holes are initialized to the free state and remain inert; they maintain
the complement invariant and never trigger false positives. The kernel's
own ``pageblock_flags`` bitmaps use the same flat approach, sizing to
``zone->spanned_pages`` which includes holes.

Memory overhead
---------------

The cost is 2 bits per PFN in the range (1 bit per bitmap x 2 bitmaps),
allocated via ``memblock_alloc()`` before the buddy allocator is
available. A hole wastes ``hole_size / PAGE_SIZE / 8`` bytes per bitmap.
In practice the waste from holes is negligible::

  System         Holes    Per-bitmap size   Hole waste   Waste/bitmap
  -----------    ------   ---------------   ----------   ------------
  64 GB, flat    none     2 MB              0            0%
  256 GB, flat   none     8 MB              0            0%
  256 GB         4 GB     8.1 MB            128 KB       1.5%
  1 TB           16 GB    32.5 MB           512 KB       1.5%

On x86_64 the typical hole between low memory (below 4 GB) and high
memory is the largest source of waste. On arm64 with
``memblock_start_of_DRAM()`` typically at 0x80000000 (2 GB), holes
within the DRAM range are generally small or absent.

Other limitations
-----------------

The feature is incompatible with ``CONFIG_MEMORY_HOTPLUG`` because the
bitmaps are sized at boot based on the initial physical memory range.
Hot-added memory would fall outside the tracked PFN range and be silently
ignored.

Boot-time reserved pages are not tracked as allocations. Freeing such a
page before ``SYSTEM_RUNNING`` is expected and is ignored by the
double-free detector. Freeing an untracked reserved page after boot is
reported as a double-free.

The feature detects corruption in the tracking bitmaps themselves, not
corruption in the actual page contents. For page content verification,
see CONFIG_PAGE_POISONING.

Implementation Details
======================

The checker hooks into the page allocator at two points:

- ``post_alloc_hook()`` calls ``page_consistency_alloc()`` after a
  successful allocation.

- ``free_pages_prepare()`` calls ``page_consistency_free()`` when pages
  are being returned to the allocator.

Both hooks use static keys (``static_branch_unlikely``) so the overhead
is a single no-op when the feature is disabled.

The bitmaps are allocated during ``mm_core_init()`` using
``memblock_alloc()`` before ``memblock_free_all()`` releases memblock
memory to the buddy allocator. The secondary bitmap is initialized with
all bits set to 1, establishing the initial complementary relationship
with the zeroed primary bitmap.
