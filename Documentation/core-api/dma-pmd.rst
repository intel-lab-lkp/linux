.. SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause

=========================================================
DMA_PMD: PMD_SIZE IOMMU mappings for DMA-coherent devices
=========================================================

Overview
========

DMA_PMD is a transparent enhancement of the current IOMMU modes, that
gives the performance of identity mode, and like strict IOMMU mode
(DMA), guarantees that memory will go back to the buddy allocator only
when all IOMMU mappings have been removed and IOTLB flushed.

The original design aimed at removing IOTLB thrashing, but its mechanism
also give security guarantees comparable to strict IOMMU, and remove the
bounce buffer overhead from Confidential Computing and preemptible VMs.

Background and motivations
==========================

I/O devices typically access at least 3-4 different memory regions on
each transaction (network packet or disk request):

- command, completion and buffer queues
- optionally, header or metadata buffers (e.g., NIC packet headers, NVMe SGL...)
- data buffers (TX socket buffers, NIC RX buffers, disk I/O buffers)

Enabling the IOMMU impacts performance for the following reasons:

- CPU cost to install/remove IOMMU Page Table Entries (PTEs)
  (``dma_map_*()``, ``dma_unmap_*()``)
- CPU/system overhead to flush the IOTLB, ie remove stale PTEs that could give
  access to sensitive data or code when memory is recycled for other purposes.
- Higher DMA read/write latency from IOMMU page walks when the working set
  exceeds the IOTLB size, backpressuring the bus via flow control.
  This is one of the biggest bottleneck for high speed devices:
  typical network devices have very little IOVA locality, and that makes the IOTLB
  almost completely ineffective.

There are known methods to mitigate these problems:

1. Recycling I/O buffers and their IOMMU mappings removes most of the DMA map/unmap
   costs. This is common practice for command, completion and buffer
   queues, as well as disk I/O and network receive buffers.  Network
   TX buffers are a noticeable exception. They normally come from
   ``alloc_pages()`` calls unaware of the leaf device, and require
   mapping/unmapping on each use.

2. Lazily flushing the IOTLB (as implemented by DMA-FQ) amortizes a
   very expensive operation (up to several microseconds to wait for
   completion, necessary for security), but the way it is implemented
   in DMA-FQ compromises on security: IOTLB flush requests are run
   periodically in batches, but the underlying memory is recycled without
   waiting. During that window, the physical memory is still accessible,
   opening the door to exfiltration or corruption of sensitive data/code.

3. The set of PTEs in active use can be significantly reduced by mapping larger
   blocks, e.g. 2MB (called PMD) instead of the usual 4KB (PTE). This is not
   currently pursued by the kernel.

DMA_PMD addresses all the above with a combination of simple, known concepts,
implemented in a way that integrates smoothly with the kernel and drivers:

- intercept calls to the small set of functions used to allocate, map, unmap, and free
  device-accessible memory (rings and buffers)
- return memory backed by physically contiguous 2MB pages (PMD) and 2MB IOMMU
  mappings
- heavily recycle memory and mappings
- wait until the IOTLB has been flushed before returning unused DMA_PMD memory
  to the system pool

Execution is what makes DMA_PMD practical: the implementation intercepts
a small set of kernel APIs so that device will inherit the benefits with
little if any changes, and the design is such that these intercepts have
minimal impact on the original functions.

Furthermore, since DMA_PMD intercepts allocations of device-accessible
memory, it gives two important benefits:
- transparent pinning/unpining, required for preemptible VMs
- transparent configuration of unencrypted mode, required for confidential computing

Limitations
===========

DMA_PMD is currently limited to systems with ``PMD_SIZE`` equal to 2MB, which
is the vast majority of existing platforms. It could be extended to larger
pages without much effort. This is enforced at compile time.

The implementation assumes non-preemptible spinlocks, so DMA_PMD is not
compatible with ``PREEMPT_RT``. This is enforced at compile time.

DMA_PMD can only be used by DMA-coherent devices. This is enforced at runtime.

IOMMU mappings for data buffers are always ``DMA_BIDIRECTIONAL``. This
simplifies the handling of e.g. forwarding between different network
interfaces.

IOMMU mappings for queues are also ``DMA_BIDIRECTIONAL``, though stricter modes
can be implemented trivially.

Implementation details
======================

Most of the code size and design complexity relates to the handling
of exceptional events (device teardown, memory hotplug) and to
the safe release of pages through deferred tasks, RCU and grace
periods.

The implementation lives in ``drivers/iommu/dma-pmd-*.c`` and
``include/linux/dma-pmd.h``, and relies on the components described below.
We use the name "DMA_PMD" to refer to the second-level physically contiguous
pages (typically 2MB) used for I/O memory.

The set of functions that need to be intercepted is relatively small:

- ``page_pool_dev_alloc*()``, ``skb_page_frag_refill()``, ``__free_pages()``
- ``dma_alloc_attrs()``, ``dma_map_*()``, ``dma_unmap_*()``

and most devices will automatically inherit the benefits.

Requirements (for performance and functionality)
================================================

- efficient identification of DMA_PMD pages, based on either their Physical
  Address (PA) or their I/O Virtual Address (IOVA)
- fast access to DMA_PMD page metadata
- fast allocation under highly concurrent usage
- efficient DMA map/unmap
- comprehensive lifetime management
- support for confidential computing (decrypted DMA buffers)
- support for preemptible VMs (pinned DMA buffers)

Internal mechanisms
===================

We use the following components:

- **DMA_PMD metadata** (``struct dma_pmd_meta``):

  Identification of DMA_PMD pages could be done in principle with a flag in
  ``struct page``, but that would not solve the management of per-page
  metadata.

  DMA_PMD addresses both requirements without depending on ``struct page``
  using an idea similar to ``pageblock_flags``. Each 2MB page used as a
  buffer requires only 256B of metadata, or 0.01% overhead. For metadata,
  we reserve a sparse virtual memory region covering of approximate
  size ``max_pfn * 256 / PMD_SIZE``, so the metadata can be accessed
  directly using the 2MB page index.  Metadata pages are populated on
  demand, with a compact bitmap (1 bit per 32MB chunk, or 4KB per 1TB
  of address space) tracking which metadata pages are backed, allowing
  fast lockless validation and direct array indexing. Memory hotplugged
  later is also supported.

  ``struct dma_pmd_meta`` has flags to mark the page status (used as
  DMA_PMD buffer, decrypted for confidential computing, or pinned against
  memory compaction), and tracks which subpages are free, which domains
  have an active IOMMU mapping for the page, and holds list/RCU linkage
  to manage its lifetime.

- **Per-domain reserved DMA_PMD IOVA ranges**:

  Another key requirement is to quickly resolve the PA-IOVA mapping for a given domain. Since
  a DMA_PMD page can be mapped in multiple domains (e.g. a network buffer used
  to route packets between different NICs), it would be too expensive to manage random
  PA-IOVA mappings created on the fly.
  DMA_PMD reserves on each domain an IOVA range
  covering the physical memory range plus an extra region, allowing a
  fixed PA-IOVA offset, possibly different for each domain.
  The IOVA allocation happens the first time a domain is used, hence a
  IOVA within the reserved range will uniquely identify a DMA_PMD page.

- **alloc_pages() compatible dma_pmd_pool allocator for I/O buffers**:

  DMA_PMD implements a dma_pmd_pool object for fixed-order page allocations
  that is a natural fit for the allocation of NIC receive buffers and tx
  socket buffers. The API is similar to ``alloc_pages()`` (in fact, it is an
  almost direct replacement) and dma_pmd_pools are instantiated as follow:
  - per NIC-receive-queue (or page_pool) to provide order-0 pages to each queue
  - per-CPU to provide order-0 and order-3 pages to refill tx socket buffers.

- **dma_pmd_arena allocator to back dma_alloc_attrs()**

  Devices use ``dma_alloc_attrs()`` or variants for long-lived, variable size
  allocations to back device queues and header buffers. dma_pmd_arena
  has the same functions: it both allocates and maps memory in the
  reserved IOVA, and is used exclusively within dma_alloc_attrs() to
  handle suitable requests (4K or larger, GFP_KERNEL, for devices that
  specifically enable DMA_PMD)


- **Fine grained control**

  Both for experimentation and production use, it is useful to have
  some form of control on which devices want to use DMA_PMD and for
  what. DMA_PMD exposes the following controls. Keep in mind that full
  performance can only be achieved if all regions are mapped using DMA_PMD.

  - /sys/devices/*/*/dma_pmd_{rings,rxbuf,tx_hdrs} per-device entries
    that can be used within device drivers to decide whether to use DMA_PMD for each of its regions

  - /proc/sys/net/core/tx_enable_dma_pmd controls whether tx socket buffers should use DMA_PMD

- **Safe release of memory to the system**

  A key feature of DMA_PMD is that memory is not released to the system
  while there are active IOMMU mappings. Release of buffers is managed
  as follows

  1. on ``__free_pages()`` or ``dma_free_attrs()``, blocks are returned to DMA_PMD,
     IOMMU mappings are still active, and they can be recycled

  2. when all blocks in a DMA_PMD page are unused, the page is moved to an
     "idle" list, still with active mappings and available for reuse

  3. upon memory pressure or when above some global threshold, a shrinker
     thread collects idle pages from all pools and starts removing the iommu
     mappings and issues a synchronous IOTLB flush. The pages are not usable for allocations
     but not returned to the pool yet

  4. once the previous step is complete, the system schedules ``call_rcu()``
     and waits for an RCU grace period (and re-encrypts the page if it was
     decrypted) before returning the 2MB page to the buddy allocator.

- **Handling domain destruction**

  When a device/domain is destroyed, all I/O issued by the device must have
  been quiesced and memory released via ``__free_pages()`` or ``dma_free*()``.
  DMA_PMD hooks into the domain destructor and removes all existing mappings
  for the disappearing domain and synchronously flushes the IOTLB, similarly
  to steps #3 and #4 shown before.

- **Allocation and compound splitting**:

  A pool that needs memory calls ``dma_pmd_add_page()`` to allocate a
  physically contiguous 2MB page on the calling CPU's NUMA node, and
  ``split_page_compound()`` to split it into independent blocks of
  ``pool->order``. These blocks can be independently passed through the
  networking stack.

Modified kernel APIs and runtime controls
=========================================

DMA_PMD integrates transparently into existing kernel memory, DMA, and
networking APIs. Allocation of DMA_PMD memory is opt-in via per-device sysfs
attributes or sysctls, while mapping, unmapping, and freeing automatically
detect whether a buffer or IOVA belongs to DMA_PMD:

1. **Streaming DMA map and unmap** (``dma_map_page_attrs()``,
   ``dma_map_single_attrs()``, ``dma_map_sg_attrs()`` and
   ``dma_unmap_page_attrs()``, ``dma_unmap_single_attrs()``,
   ``dma_unmap_sg_attrs()`` via ``drivers/iommu/dma-iommu.c`` and
   ``kernel/dma/direct.c``):

   - On map, any buffer belonging to a DMA_PMD page (``dma_is_pmd_phys(phys)``)
     is mapped via the domain's reserved ``DMA_PMD`` IOVA window with a simple
     addition, typically reusing existing mappings.
     Under ``dma-direct``, decrypted or pinned ``DMA_PMD`` pages
     (``dma_is_pmd_direct(phys)``) can be mapped directly without ``swiotlb`` bounce.

     On unmap, IOVAs can be identified as DMA_PMD buffers with a simple range check
     and that results in a no-operation.

2. **Coherent DMA allocation and free** (``dma_alloc_attrs()`` /
   ``dma_alloc_coherent()`` and ``dma_free_attrs()`` / ``dma_free_coherent()``
   in ``kernel/dma/mapping.c``):

   - DMA_PMD is enabled per device via ``/sys/devices/.../dma_pmd_rings``
     (``dev->dma_pmd_rings``). When set, sleepable ``GFP_KERNEL`` coherent
     allocations are rounded up to 4KB and carved out of DMA_PMD buffers
     ``dma_pmd_arena`` and mapped accordingly.
     ``dma_free_attrs()`` automatically detects arena IOVAs via
     ``dma_pmd_free()`` and returns the sub-blocks to the arena.

3. **Page allocator release** (``__free_pages()`` / ``put_page()`` via
   ``__free_pages_prepare()`` in ``mm/page_alloc.c``):

   - ``dma_pmd_free_page()`` checks
     ``dma_is_pmd_page(page_to_pfn(page))`` and recycles DMA_PMD subpages back to
     their owning ``dma_pmd_pool`` instead of releasing them to the buddy
     allocator while their 2MB IOMMU mappings remain active.

4. **IOMMU domain teardown** (``iommu_put_dma_cookie()`` in
   ``drivers/iommu/dma-iommu.c``):

   - A call to ``dma_pmd_domain_release()`` before freeing a
     domain's IOVA cookie and page tables clears cached domain state across
     all pools. Existing IOMMU mappings and IOTLB flush has already happened.

5. **Networking ``page_pool``**

   - DMA_PMD is enabled per device via ``/sys/devices/.../dma_pmd_rxbuf``
     (``dev->dma_pmd_rxbuf``). Any NIC driver using ``page_pool`` with this
     flag set will back its RX buffers with a per-``page_pool``
     ``dma_pmd_pool``, and opportunistically replaces plain 4KB pages when pooled 2MB
     blocks are available.

6. **Socket TX page fragments** (``skb_page_frag_refill()`` in
   ``net/core/sock.c``):

   - When enabled globally via the ``net.core.tx_enable_dma_pmd`` sysctl
     socket TX fragments are allocated from per-CPU ``dma_pmd_pool`` instances.

7. **NIC driver RX pools and TX header bounce buffers**

   - **RX buffers:** Controlled per device by ``/sys/devices/.../dma_pmd_rxbuf``
     (``dev->dma_pmd_rxbuf``) for queue modes managing their own page rings
     (``idpf``, ``gve`` GQI-RDA, ``gq``).
   - **TX headers:** Controlled per device by
     ``/sys/devices/.../dma_pmd_tx_hdrs`` (``dev->dma_pmd_tx_hdrs``),
     copying linear packet headers into pre-allocated coherent bounce buffers
     (backed by ``dma_pmd_arena`` when ``dma_pmd_rings`` is also enabled).

8. **Global pool limit and observability:**

   - ``/sys/module/kernel/parameters/dma_pmd_max_pages``: Global cap on 2MB
     pages held across all pools (defaults to 1/8th of physical RAM).
   - ``/sys/kernel/debug/dma_pmd/pools``: Debugfs summary of global and per-pool
     allocation, mapping, and fallback counters.
