.. SPDX-License-Identifier: GPL-2.0

======================================
Virtually Contiguous Memory Allocation
======================================

``vmalloc()`` allocates memory that is contiguous in kernel virtual address
space but may be backed by physically discontiguous pages.  This is useful
for large allocations where finding a contiguous physical range would be
difficult or impossible.  The implementation is in ``mm/vmalloc.c``.

.. contents:: :local:

How It Works
============

A vmalloc allocation has three steps: reserve a range of kernel virtual
addresses, allocate physical pages (individually, via the page allocator),
and create page table mappings that connect the two.

Virtual Address Management
--------------------------

The kernel reserves a large region of virtual address space for vmalloc
(on x86-64 this is hundreds of terabytes).  Within this region, allocated
and free ranges are tracked by ``struct vmap_area`` nodes organized in two
red-black trees — one sorted by address for the busy areas, and one
augmented with subtree maximum gap size for the free areas.  The augmented
tree allows free-space searches in O(log n) time.

Each allocated area also has a ``struct vm_struct`` that records the
virtual address, size, array of backing ``struct page`` pointers, and flags
indicating how the area was created (``VM_ALLOC`` for vmalloc,
``VM_IOREMAP`` for I/O mappings, ``VM_MAP`` for vmap, etc.).

Guard Pages
-----------

By default, each vmalloc area is surrounded by a guard page — an unmapped
page that causes an immediate fault if code overruns the allocation.  This
costs one page of virtual address space (not physical memory) per
allocation.  The ``VM_NO_GUARD`` flag disables this for internal users that
manage their own safety margins.

Huge Page Support
-----------------

On architectures that support it, vmalloc can use PMD- or PUD-level
mappings instead of individual PTEs, reducing TLB pressure for large
allocations.  ``vmalloc_huge()`` requests this explicitly.  The decision
is per-architecture: each architecture provides callbacks
(``arch_vmap_pmd_supported()``, ``arch_vmap_pud_supported()``) to indicate
which levels are available.

Even when huge pages are requested, the allocator falls back to base pages
transparently if the physical pages cannot be allocated at the required
alignment.

Lazy TLB Flushing
-----------------

Unmapping a vmalloc area requires a global TLB flush (IPI to all CPUs) to
ensure no stale translations remain.  To amortize this cost, vmalloc defers
the flush: page table entries are cleared immediately but the TLB
invalidation is batched across multiple frees.  The flush is forced when
the free area needs to be reused or when ``vm_unmap_aliases()`` is called
explicitly.

Per-CPU Allocations
-------------------

The per-CPU allocator uses vmalloc internally to obtain virtually
contiguous backing for per-CPU variables across all CPUs.  It allocates
multiple vmalloc areas with specific size and alignment requirements in a
single call, ensuring that each CPU's copy is at a consistent offset from
the per-CPU base.

vmap and Temporary Mappings
===========================

Besides vmalloc (which allocates both virtual space and physical pages),
the subsystem provides two related mechanisms:

- **vmap/vunmap**: maps an existing array of ``struct page`` pointers into
  contiguous kernel virtual space.  This is used when pages have already
  been allocated (e.g., by a device driver) and just need a contiguous
  kernel mapping.

- **vm_map_ram/vm_unmap_ram**: lightweight temporary mappings for
  short-lived use, with lower overhead than full vmap.

Freeing
=======

``vfree()`` can be called from any context, including interrupt handlers.
When called from interrupt context the actual work (page table teardown,
TLB flush, page freeing) is deferred to a workqueue.  This is safe because
the virtual address range is immediately removed from the busy tree, so no
new mappings can be created in the freed region.

Page Table Management
=====================

vmalloc maintains its own kernel page tables to map virtual addresses to
the backing physical pages.  On allocation, page table entries are created
at the appropriate level (PTE, PMD, or PUD depending on huge page support).
On free, the entries are cleared.

The page table setup must handle architectures where the kernel page tables
are not shared across all CPUs.  On such systems, a vmalloc fault mechanism
lazily propagates new mappings: when a CPU accesses a vmalloc address for
the first time and takes a fault, the fault handler copies the page table
entry from the reference page table (init_mm) into the CPU's page table.

NUMA Awareness
==============

By default, vmalloc allocates physical pages from any NUMA node.  The
``vmalloc_node()`` and ``vzalloc_node()`` variants prefer a specific node,
which is useful for data structures that are predominantly accessed from
one node.  The pages are still mapped into the global kernel virtual
address space, so they remain accessible from all CPUs regardless of
which node they were allocated from.

KASAN Integration
=================

When KASAN (Kernel Address Sanitizer) is enabled with
``CONFIG_KASAN_VMALLOC``, vmalloc allocates shadow memory to track the
validity of each vmalloc region.  The shadow memory is itself vmalloc'd
and mapped lazily.  This allows KASAN to detect out-of-bounds accesses
and use-after-free bugs in vmalloc'd memory, which is particularly useful
for catching bugs in kernel modules (whose code and data are vmalloc'd).
