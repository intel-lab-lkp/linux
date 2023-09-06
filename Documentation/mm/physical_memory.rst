.. SPDX-License-Identifier: GPL-2.0

===============
Physical Memory
===============

Linux is available for a wide range of architectures so there is a need for an
architecture-independent abstraction to represent the physical memory. This
chapter describes the structures used to manage physical memory in a running
system.

The first principal concept prevalent in the memory management is
`Non-Uniform Memory Access (NUMA)
<https://en.wikipedia.org/wiki/Non-uniform_memory_access>`_.
With multi-core and multi-socket machines, memory may be arranged into banks
that incur a different cost to access depending on the “distance” from the
processor. For example, there might be a bank of memory assigned to each CPU or
a bank of memory very suitable for DMA near peripheral devices.

Each bank is called a node and the concept is represented under Linux by a
``struct pglist_data`` even if the architecture is UMA. This structure is
always referenced by its typedef ``pg_data_t``. A ``pg_data_t`` structure
for a particular node can be referenced by ``NODE_DATA(nid)`` macro where
``nid`` is the ID of that node.

For NUMA architectures, the node structures are allocated by the architecture
specific code early during boot. Usually, these structures are allocated
locally on the memory bank they represent. For UMA architectures, only one
static ``pg_data_t`` structure called ``contig_page_data`` is used. Nodes will
be discussed further in Section :ref:`Nodes <nodes>`

The entire physical address space is partitioned into one or more blocks
called zones which represent ranges within memory. These ranges are usually
determined by architectural constraints for accessing the physical memory.
The memory range within a node that corresponds to a particular zone is
described by a ``struct zone``, typedeffed to ``zone_t``. Each zone has
one of the types described below.

* ``ZONE_DMA`` and ``ZONE_DMA32`` historically represented memory suitable for
  DMA by peripheral devices that cannot access all of the addressable
  memory. For many years there are better more and robust interfaces to get
  memory with DMA specific requirements (Documentation/core-api/dma-api.rst),
  but ``ZONE_DMA`` and ``ZONE_DMA32`` still represent memory ranges that have
  restrictions on how they can be accessed.
  Depending on the architecture, either of these zone types or even they both
  can be disabled at build time using ``CONFIG_ZONE_DMA`` and
  ``CONFIG_ZONE_DMA32`` configuration options. Some 64-bit platforms may need
  both zones as they support peripherals with different DMA addressing
  limitations.

* ``ZONE_NORMAL`` is for normal memory that can be accessed by the kernel all
  the time. DMA operations can be performed on pages in this zone if the DMA
  devices support transfers to all addressable memory. ``ZONE_NORMAL`` is
  always enabled.

* ``ZONE_HIGHMEM`` is the part of the physical memory that is not covered by a
  permanent mapping in the kernel page tables. The memory in this zone is only
  accessible to the kernel using temporary mappings. This zone is available
  only on some 32-bit architectures and is enabled with ``CONFIG_HIGHMEM``.

* ``ZONE_MOVABLE`` is for normal accessible memory, just like ``ZONE_NORMAL``.
  The difference is that the contents of most pages in ``ZONE_MOVABLE`` is
  movable. That means that while virtual addresses of these pages do not
  change, their content may move between different physical pages. Often
  ``ZONE_MOVABLE`` is populated during memory hotplug, but it may be
  also populated on boot using one of ``kernelcore``, ``movablecore`` and
  ``movable_node`` kernel command line parameters. See
  Documentation/mm/page_migration.rst and
  Documentation/admin-guide/mm/memory-hotplug.rst for additional details.

* ``ZONE_DEVICE`` represents memory residing on devices such as PMEM and GPU.
  It has different characteristics than RAM zone types and it exists to provide
  :ref:`struct page <Pages>` and memory map services for device driver
  identified physical address ranges. ``ZONE_DEVICE`` is enabled with
  configuration option ``CONFIG_ZONE_DEVICE``.

It is important to note that many kernel operations can only take place using
``ZONE_NORMAL`` so it is the most performance critical zone. Zones are
discussed further in Section :ref:`Zones <zones>`.

The relation between node and zone extents is determined by the physical memory
map reported by the firmware, architectural constraints for memory addressing
and certain parameters in the kernel command line.

For example, with 32-bit kernel on an x86 UMA machine with 2 Gbytes of RAM the
entire memory will be on node 0 and there will be three zones: ``ZONE_DMA``,
``ZONE_NORMAL`` and ``ZONE_HIGHMEM``::

  0                                                            2G
  +-------------------------------------------------------------+
  |                            node 0                           |
  +-------------------------------------------------------------+

  0         16M                    896M                        2G
  +----------+-----------------------+--------------------------+
  | ZONE_DMA |      ZONE_NORMAL      |       ZONE_HIGHMEM       |
  +----------+-----------------------+--------------------------+


With a kernel built with ``ZONE_DMA`` disabled and ``ZONE_DMA32`` enabled and
booted with ``movablecore=80%`` parameter on an arm64 machine with 16 Gbytes of
RAM equally split between two nodes, there will be ``ZONE_DMA32``,
``ZONE_NORMAL`` and ``ZONE_MOVABLE`` on node 0, and ``ZONE_NORMAL`` and
``ZONE_MOVABLE`` on node 1::


  1G                                9G                         17G
  +--------------------------------+ +--------------------------+
  |              node 0            | |          node 1          |
  +--------------------------------+ +--------------------------+

  1G       4G        4200M          9G          9320M          17G
  +---------+----------+-----------+ +------------+-------------+
  |  DMA32  |  NORMAL  |  MOVABLE  | |   NORMAL   |   MOVABLE   |
  +---------+----------+-----------+ +------------+-------------+


Memory banks may belong to interleaving nodes. In the example below an x86
machine has 16 Gbytes of RAM in 4 memory banks, even banks belong to node 0
and odd banks belong to node 1::


  0              4G              8G             12G            16G
  +-------------+ +-------------+ +-------------+ +-------------+
  |    node 0   | |    node 1   | |    node 0   | |    node 1   |
  +-------------+ +-------------+ +-------------+ +-------------+

  0   16M      4G
  +-----+-------+ +-------------+ +-------------+ +-------------+
  | DMA | DMA32 | |    NORMAL   | |    NORMAL   | |    NORMAL   |
  +-----+-------+ +-------------+ +-------------+ +-------------+

In this case node 0 will span from 0 to 12 Gbytes and node 1 will span from
4 to 16 Gbytes.

.. _nodes:

Nodes
=====

As we have mentioned, each node in memory is described by a ``pg_data_t`` which
is a typedef for a ``struct pglist_data``. When allocating a page, by default
Linux uses a node-local allocation policy to allocate memory from the node
closest to the running CPU. As processes tend to run on the same CPU, it is
likely the memory from the current node will be used. The allocation policy can
be controlled by users as described in
Documentation/admin-guide/mm/numa_memory_policy.rst.

Most NUMA architectures maintain an array of pointers to the node
structures. The actual structures are allocated early during boot when
architecture specific code parses the physical memory map reported by the
firmware. The bulk of the node initialization happens slightly later in the
boot process by free_area_init() function, described later in Section
:ref:`Initialization <initialization>`.


Along with the node structures, kernel maintains an array of ``nodemask_t``
bitmasks called ``node_states``. Each bitmask in this array represents a set of
nodes with particular properties as defined by ``enum node_states``:

``N_POSSIBLE``
  The node could become online at some point.
``N_ONLINE``
  The node is online.
``N_NORMAL_MEMORY``
  The node has regular memory.
``N_HIGH_MEMORY``
  The node has regular or high memory. When ``CONFIG_HIGHMEM`` is disabled
  aliased to ``N_NORMAL_MEMORY``.
``N_MEMORY``
  The node has memory(regular, high, movable)
``N_CPU``
  The node has one or more CPUs

For each node that has a property described above, the bit corresponding to the
node ID in the ``node_states[<property>]`` bitmask is set.

For example, for node 2 with normal memory and CPUs, bit 2 will be set in ::

  node_states[N_POSSIBLE]
  node_states[N_ONLINE]
  node_states[N_NORMAL_MEMORY]
  node_states[N_HIGH_MEMORY]
  node_states[N_MEMORY]
  node_states[N_CPU]

For various operations possible with nodemasks please refer to
``include/linux/nodemask.h``.

Among other things, nodemasks are used to provide macros for node traversal,
namely ``for_each_node()`` and ``for_each_online_node()``.

For instance, to call a function foo() for each online node::

	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);

		foo(pgdat);
	}

Node structure
--------------

The nodes structure ``struct pglist_data`` is declared in
``include/linux/mmzone.h``. Here we briefly describe fields of this
structure:

General
~~~~~~~

``node_zones``
  The zones for this node.  Not all of the zones may be populated, but it is
  the full list. It is referenced by this node's node_zonelists as well as
  other node's node_zonelists.

``node_zonelists``
  The list of all zones in all nodes. This list defines the order of zones
  that allocations are preferred from. The ``node_zonelists`` is set up by
  ``build_zonelists()`` in ``mm/page_alloc.c`` during the initialization of
  core memory management structures.

``nr_zones``
  Number of populated zones in this node.

``node_mem_map``
  For UMA systems that use FLATMEM memory model the 0's node
  ``node_mem_map`` is array of struct pages representing each physical frame.

``node_page_ext``
  For UMA systems that use FLATMEM memory model the 0's node
  ``node_page_ext`` is array of extensions of struct pages. Available only
  in the kernels built with ``CONFIG_PAGE_EXTENSION`` enabled.

``node_start_pfn``
  The page frame number of the starting page frame in this node.

``node_present_pages``
  Total number of physical pages present in this node.

``node_spanned_pages``
  Total size of physical page range, including holes.

``node_size_lock``
  A lock that protects the fields defining the node extents. Only defined when
  at least one of ``CONFIG_MEMORY_HOTPLUG`` or
  ``CONFIG_DEFERRED_STRUCT_PAGE_INIT`` configuration options are enabled.
  ``pgdat_resize_lock()`` and ``pgdat_resize_unlock()`` are provided to
  manipulate ``node_size_lock`` without checking for ``CONFIG_MEMORY_HOTPLUG``
  or ``CONFIG_DEFERRED_STRUCT_PAGE_INIT``.

``node_id``
  The Node ID (NID) of the node, starts at 0.

``totalreserve_pages``
  This is a per-node reserve of pages that are not available to userspace
  allocations.

``first_deferred_pfn``
  If memory initialization on large machines is deferred then this is the first
  PFN that needs to be initialized. Defined only when
  ``CONFIG_DEFERRED_STRUCT_PAGE_INIT`` is enabled

``deferred_split_queue``
  Per-node queue of huge pages that their split was deferred. Defined only when ``CONFIG_TRANSPARENT_HUGEPAGE`` is enabled.

``__lruvec``
  Per-node lruvec holding LRU lists and related parameters. Used only when
  memory cgroups are disabled. It should not be accessed directly, use
  ``mem_cgroup_lruvec()`` to look up lruvecs instead.

Reclaim control
~~~~~~~~~~~~~~~

See also Documentation/mm/page_reclaim.rst.

``kswapd``
  Per-node instance of kswapd kernel thread.

``kswapd_wait``, ``pfmemalloc_wait``, ``reclaim_wait``
  Workqueues used to synchronize memory reclaim tasks

``nr_writeback_throttled``
  Number of tasks that are throttled waiting on dirty pages to clean.

``nr_reclaim_start``
  Number of pages written while reclaim is throttled waiting for writeback.

``kswapd_order``
  Controls the order kswapd tries to reclaim

``kswapd_highest_zoneidx``
  The highest zone index to be reclaimed by kswapd

``kswapd_failures``
  Number of runs kswapd was unable to reclaim any pages

``min_unmapped_pages``
  Minimal number of unmapped file backed pages that cannot be reclaimed.
  Determined by ``vm.min_unmapped_ratio`` sysctl. Only defined when
  ``CONFIG_NUMA`` is enabled.

``min_slab_pages``
  Minimal number of SLAB pages that cannot be reclaimed. Determined by
  ``vm.min_slab_ratio sysctl``. Only defined when ``CONFIG_NUMA`` is enabled

``flags``
  Flags controlling reclaim behavior.

Compaction control
~~~~~~~~~~~~~~~~~~

``kcompactd_max_order``
  Page order that kcompactd should try to achieve.

``kcompactd_highest_zoneidx``
  The highest zone index to be compacted by kcompactd.

``kcompactd_wait``
  Workqueue used to synchronize memory compaction tasks.

``kcompactd``
  Per-node instance of kcompactd kernel thread.

``proactive_compact_trigger``
  Determines if proactive compaction is enabled. Controlled by
  ``vm.compaction_proactiveness`` sysctl.

Statistics
~~~~~~~~~~

``per_cpu_nodestats``
  Per-CPU VM statistics for the node

``vm_stat``
  VM statistics for the node.

.. _zones:

Zones
=====

.. admonition:: Stub

   This section is incomplete. Please list and describe the appropriate fields.

.. _memmap:

Memory map and memory descriptors
=================================

Every physical page frame in the systam has an associated descriptor which
is used to keep track of its status. The collection of these descriptors is
called `memory map` and it is arranged in one or more arrays, depending on
the selection of the memory model. Memory models are described in more
detail in Documentation/mm/memory-model.rst

The basic memory descriptor is called :ref:`struct page <Pages>` and it is
essentially a union of several structures, each representing a page frame
metadata for a paricular usage.

In many cases the entries in the memory map are not treated as `struct page`,
but rather as different types of descriptors such as :ref:`struct folio
<Folios>`, :ref:`struct ptdesc <ptdesc>` or `struct slab`.

.. _pages:

Pages
-----

`struct page` tracks status of a single physical page frame. This structure
is a mixture of several types that represent metadata for different uses of
a page frame. To save memory these types partially overlap so the `struct
page` definition in ``include/linux/mm_types.h`` mixes scalar fields and
unions of structures.

Common fields
~~~~~~~~~~~~~

``flags``
  This field contains flags which describe the status of the page and
  additional information about the page, like, for instance, zone, section
  and node this page belongs to. Several flags determine how the page is
  used, sometimes in combination with ``page_type`` field. Other flags
  determine the state of the page, for instance if it is dirty or should be
  reclaimed, what LRU list this page is on and many others.

  All flags are declared in ``include/linux/page-flags.h``. There are a
  number of macros defined for testing, clearing and setting the flags. Page
  flags should not be accessed directly, but only using these macros.

  The layout of the ``flags`` field depends on the kernel configuration. It
  is affeted by selection of the memory model, section size for SPARSEMEM
  without VMEMMAP, number of zone types, maximal number of nodes and other
  build time parameters, such as ``CONFIG_NUMA_BALANCING``,
  ``CONFIG_KASAN_SW_TAGS`` and ``CONFIG_LRU_GEN``.

  For example, a kernel configured for 64-bit system with
  SPARSEMEM_VMEMMAP, four zone types and maximum of 64 nodes and other
  relevant options disabled layout of ``flags`` will be::

    63   58 57  56 55                  23 22                      0
    +------+------+----------------------+------------------------+
    | node | zone |         ...          |         flags          |
    +------+------+----------------------+------------------------+

  And for the same configuration with enabled ``CONFIG_LRU_GEN`` and
  ``CONFIG_NUMA_BALANCING`` it will be::

    63   58 57  56 55    42 41     39 38      37 36  23 22        0
    +------+------+--------+---------+----------+------+----------+
    | node | zone | cpupid | lru_gen | lru_refs | ...  |  flags   |
    +------+------+--------+---------+----------+------+----------+

  For the exact details refer to ``include/linux/page-flags-layout.h`` and
  ``include/linux/mmzone.h``.

  Although in the above examples the page flags layout includes 23 flags,
  their number may vary with different kernel configurations.

``_refcount``
  Usage count of the `struct page`. Should not be used directly. Use
  accessors defined in ``include/linux/page_ref.h``.

``memcg_data``
  An opaque object used by memory cgroups. Defined only when
  ``CONFIG_MEMCG`` is enabled.

``virtual``
  Virtual address in the kernel direct map. Will be ``NULL`` for highmem
  pages. Only defined for some architectures.

``kmsan_shadow``
  KMSAN shadow page: every bit indicates whether the corresponding bit of
  the original page is initialized (0) or not (1). Defined only when
  ``CONFIG_KMSAN`` is enabled.

``kmsan_origin``
  KMSAN origin page: every 4 bytes contain an id of the stack trace where
  the uninitialized value was created. Defined only when ``CONFIG_KMSAN``
  is enabled.

``_last_cpupid``
  IDs of last CPU and last process that accessed the page. Only enabled if
  there are not enough bits in the ``flags`` field.
  Do not use directly, use accessors defined in ``include/linux/mm.h``

Fields shared between multiple types
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

``_mapcount``
  If the page can be mapped to userspace, encodes the number of times this
  page is referenced by a page table.
  Do not use directly, call page_mapcount().

``page_type``
  If the page is neither ``PageSlab`` nor mappable to userspace, the value
  stored here may help determine what this page is used for. See
  ``include/linux/page-flags.h`` for a list of page types which are
  currently stored here.

``rcu_head``
  You can use this to free a page by RCU. Available for page table pages
  and for page cache and anonymous pages not linked to any of the LRU
  lists.

Page cache and anonymous pages
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The following fields are used to link `struct page` to a linked list and
they overlap with each other:

``lru``
  Linked list pointers for pages on LRU lists, for example active_list
  protected by ``lruvec->lru_lock``. Sometimes used as a generic list by
  the page owner.

For pages on unevictable "LRU list" ``lru`` is overlayed with an anonymous
struct containing two fields:

``__filler``
  A dummy field that must be always even to avoid conflict with compound
  page encoding.

``mlock_count``
  Number of times the page has been pinned by mlock().

Pages on free lists used by the page allocator are linked to the relevant
list with eithter of the two below fields:

``buddy_list``
  Links the page to one of the free lists in the buddy allocator. Overlaps
  with ``lru``.

``pcp_list``
  Links the page to a per-cpu free list. Overlaps with ``lru``.

``mapping``
  The file this page belongs to. Can be pagecache or swapcahe. For
  anonymous memory refers to the `struct anon_vma`.
  See also ``include/linux/page-flags.h`` for ``PAGE_MAPPING_FLAGS``

``index``
  Page offset within mapping. Overlaps with ``share``.

``share``
  Share count for fsdax. Overlaps with ``index``.

``private``
  Mapping-private opaque data. Usually used for buffer_heads if
  PagePrivate. Used for swp_entry_t if PageSwapCache. Indicates order in
  the buddy system if PageBuddy.

Page pool
~~~~~~~~~

The following fields are used by
`page_pool <Documentation/networking/page_pool.rst>`
allocator used by the networking stack.

``pp_magic``
  Magic value to avoid recycling non page_pool allocated pages.

``pp``
  `struct page_pool` holding the page.

``_pp_mapping_pad``
  A padding to avoid collision of page_pool data with ``mapping``.

``dma_addr``
  DMAable address of the page.

``dma_addr_upper``
  Upper part of DMA address on 32-bit architectures that use 64-bit DMA
  addressing. Overlaps with ``pp_frag_count``.

``pp_frag_count``
  Used by sub-page allocations in ``page_pool``. Not supported on 32-bit
  architectures with 64-bit DMA addresses. Overlaps with ``dma_addr_upper``.

Tail pages of compound page
~~~~~~~~~~~~~~~~~~~~~~~~~~~

``compound_head``
  Pointer to the head page of compound page. Bit zero is always set for
  tail pages and cleared for head pages.

ZONE_DEVICE pages
~~~~~~~~~~~~~~~~~

``pgmap``
  Points to the hosting device page map.

``zone_device_data``
  Private data used by the owning device.

.. _folios:

Folios
------

`struct folio` represents a physically, virtually and logically contiguous
set of bytes. It is a power-of-two in size, and it is aligned to that same
power-of-two. It is at least as large as ``PAGE_SIZE``. If it is in the
page cache, it is at a file offset which is a multiple of that
power-of-two. It may be mapped into userspace at an address which is at an
arbitrary page offset, but its kernel virtual address is aligned to its
size.

`struct folio` occupies several consecutive entries in the memory map and
has the following fields:

``flags``
  Identical to the page flags.

``lru``
  Least Recently Used list; tracks how recently this folio was used.

``mlock_count``
  Number of times this folio has been pinned by mlock().

``mapping``
  The file this page belongs to. Can be pagecache or swapcahe. For
  anonymous memory refers to the `struct anon_vma`.

``index``
  Offset within the file, in units of pages. For anonymous memory, this is
  the index from the beginning of the mmap.

``private``
  Filesystem per-folio data (see folio_attach_private()). Used for
  ``swp_entry_t`` if folio is in the swap cache
  (i.e. folio_test_swapcache() is true)

``_mapcount``
  Do not access this member directly. Use folio_mapcount() to find out how
  many times this folio is mapped by userspace.

``_refcount``
  Do not access this member directly. Use folio_ref_count() to find how
  many references there are to this folio.

``memcg_data``
  Memory Control Group data.

``_folio_dtor``
  Which destructor to use for this folio.

``_folio_order``
  The allocation order of a folio. Do not use directly, call folio_order().

``_entire_mapcount``
  How many times the entire folio is mapped as a single unit (for example
  by a PMD or PUD entry). Does not include PTE-mapped subpages. This might
  be useful for debugging, but to find out how many times the folio is
  mapped look at folio_mapcount() or page_mapcount() or total_mapcount()
  instead.
  Do not use directly, call folio_entire_mapcount().

``_nr_pages_mapped``
  The total number of times the folio is mapped.
  Do not use directly, call folio_mapcount().

``_pincount``
  Used to track pinning of the folio for DMA.
  Do not use directly, call folio_maybe_dma_pinned().

``_folio_nr_pages``
  The number of pages in the folio.
  Do not use directly, call folio_nr_pages().

``_hugetlb_subpool``
  HugeTLB subpool the folio beongs to.
  Do not use directly, use accessor in ``include/linux/hugetlb.h``.

``_hugetlb_cgroup``
  Memory Control Group data for a HugeTLB folio.
  Do not use directly, use accessor in ``include/linux/hugetlb_cgroup.h``.

``_hugetlb_cgroup_rsvd``
  Memory Control Group data for a HugeTLB folio.
  Do not use directly, use accessor in ``include/linux/hugetlb_cgroup.h``.

``_hugetlb_hwpoison``
  List of failed (hwpoisoned) pages for a HugeTLB folio.
  Do not use directly, call raw_hwp_list_head().

``_deferred_list``
  Folios to be split under memory pressure.

.. _ptdesc:

Page table descriptors
----------------------

`struct ptdesc` describes the pages used by page tables. It has the
following fields:

``_page_flags``
  Same as page flags. Unused for page tables.

``pt_rcu_head``
  For freeing page table pages using RCU.

``pt_list``
  List of used page tables. Used for s390 and x86.

``pmd_huge_pte``
  Used by THP to track page tables that map huge pages. Protected by
  ``ptdesc->ptl`` or ``mm->page_table_lock``, depending on values of
  ``CONFIG_NR_CPUS`` and ``CONFIG_SPLIT_PTLOCK_CPUS`` configuration
  options.

``pt_mm``
  Pointer to mm_struct owning the page table. Only used for PGDs on x86.

``pt_frag_refcount``
  For fragmented page table tracking. Used on Powerpc and s390 only.

``ptl``
  Page table lock. If the size of `spinlock_t` object is small enough the
  lock is embedded in `struct ptdesc`, otherwise this field points to a
  lock allocated for each page table page.

``_refcount``
  Same as page refcount. Used for s390 page tables.

``pt_memcg_data``
  Memcg data. Tracked for page tables here.

.. _initialization:

Initialization
==============

.. admonition:: Stub

   This section is incomplete. Please list and describe the appropriate fields.
