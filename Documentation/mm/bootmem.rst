.. SPDX-License-Identifier: GPL-2.0

===========
Boot Memory
===========

The kernel needs a memory allocator long before the page allocator is ready.
The memblock allocator fills this role, managing physical memory from the
earliest stages of boot until the buddy allocator takes over.  The
implementation is in ``mm/memblock.c`` and ``mm/mm_init.c``.

.. contents:: :local:

Memblock
========

Memblock tracks physical memory as two arrays of regions: ``memory`` (all
usable RAM reported by firmware) and ``reserved`` (memory already allocated
or otherwise unavailable).  A free page is one that appears in ``memory``
but not in ``reserved``.  These two arrays, along with global state such as
the allocation direction and address limit, are held in a single
``struct memblock`` instance.

Each region is a ``struct memblock_region`` recording a base address, size,
NUMA node ID, and a set of flags:

- **HOTPLUG**: memory that may be physically removed at runtime.
- **MIRROR**: memory with hardware mirroring for reliability.
- **NOMAP**: memory that should not be directly mapped by the kernel
  (e.g., firmware-reserved ranges that are usable but not mappable).
- **DRIVER_MANAGED**: memory whose lifecycle is managed by a device driver.

Region Management
-----------------

Firmware and architecture code populate the arrays early in boot.
``memblock_add()`` registers a range of usable RAM.  ``memblock_reserve()``
marks a range as taken — this is used for the kernel image itself, device
tree blobs, initrd, and other early allocations.

When regions are added, overlapping ranges are merged automatically.
Internally, ``memblock_add_range()`` handles insertion, overlap detection,
and merging in a single pass.  If the region array is full, it is doubled
in size — using memblock itself to allocate the new array.

``memblock_remove()`` deletes a range from the ``memory`` array (used when
firmware reports memory that turns out to be unusable).
``memblock_phys_free()`` removes a range from ``reserved``, making it
available for allocation again.

Allocation
----------

Memblock allocation scans the ``memory`` array for a range that does not
overlap ``reserved``, respecting NUMA node affinity and a configurable
address limit (``memblock.current_limit``).

The search can run in two directions:

- **Top-down** (default): allocates from the highest available address.
  This keeps low memory free for devices with addressing limitations.
- **Bottom-up**: allocates from the lowest available address.  Used on
  some architectures during early boot to keep allocations predictable.

Once a suitable range is found it is added to ``reserved``.  The main
allocation functions are ``memblock_alloc()`` for virtual addresses and
``memblock_phys_alloc()`` for physical addresses.  Both support NUMA-aware
variants that prefer a specific node.

Iteration
---------

Memblock provides iterator macros for walking memory ranges:

- ``for_each_mem_range()`` iterates over free ranges (memory minus
  reserved).
- ``for_each_reserved_mem_region()`` iterates over reserved ranges.
- ``for_each_mem_pfn_range()`` iterates by page frame number, which is
  used heavily during page and zone initialization.

These iterators handle the subtraction of reserved regions from memory
regions internally, presenting the caller with a simple sequence of
available ranges.

Transition to the Page Allocator
================================

Once the buddy allocator is initialized, memblock releases its free pages
via ``memblock_free_all()``.  This walks all free ranges and hands each
page to the buddy allocator.  After this point memblock is no longer used
for allocation and its data structures can be freed (on systems that
support it, the memblock arrays themselves are returned to the page
allocator via ``memblock_discard()``).

Named Reservations
------------------

The ``reserve_mem`` kernel command line parameter allows firmware or boot
loaders to reserve named memory regions that persist across kexec.  These
are tracked separately and can be looked up by name at runtime with
``reserve_mem_find_by_name()``.

Page and Zone Initialization
============================

``mm/mm_init.c`` bridges memblock and the page allocator.  Its primary
responsibilities are determining zone boundaries and initializing
``struct page`` for every physical page frame.

Zone Topology
-------------

The function ``free_area_init()`` is called by architecture code to set up
nodes and zones.  It calculates zone boundaries based on architectural
constraints (which address ranges can be used for DMA, which are always
mapped, etc.) and kernel command line parameters:

- ``kernelcore=`` sets the amount of memory that must be in non-movable
  zones.
- ``movablecore=`` sets the amount of memory to place in ``ZONE_MOVABLE``.
- ``movable_node`` allows entire NUMA nodes to be treated as movable.
- ``kernelcore=mirror`` restricts non-movable memory to mirrored regions.

These parameters control the boundary between ``ZONE_MOVABLE`` and the
other zones, which in turn affects how much memory is available for
transparent huge pages, memory hot-remove, and CMA.

Struct Page Initialization
--------------------------

Every physical page frame needs an initialized ``struct page`` before the
page allocator can manage it.  On small systems this is done synchronously
during boot.  On large systems with hundreds of gigabytes of RAM, this
initialization can take a significant amount of time.

With ``CONFIG_DEFERRED_STRUCT_PAGE_INIT``, only pages in the boot node's
lower zones are initialized during early boot — enough to get the system
running.  The remaining pages are initialized in parallel by worker threads
(via the padata framework) before they are first needed.  This can save
several seconds of boot time on large NUMA systems.

Each page is initialized by setting its flags, reference count, and links
to the owning node and zone.  Pages in memory holes or ``NOMAP`` regions
are marked as reserved and are never handed to the page allocator.
