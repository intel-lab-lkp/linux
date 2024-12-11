.. SPDX-License-Identifier: GPL-2.0

================
Netmem
================


Introduction
============

Device memory TCP, and likely more upcoming features, are reliant on netmem
support in the driver. This outlines what drivers need to do to support netmem.


Driver support
==============

1. The driver must support page_pool. The driver must not do its own recycling
   on top of page_pool.

2. The driver must support the tcp-data-split ethtool option.

3. The driver must use the page_pool netmem APIs. The netmem APIs are
   currently 1-to-1 correspond with page APIs. Conversion to netmem should be
   achievable by switching the page APIs to netmem APIs and tracking memory via
   netmem_refs in the driver rather than struct page * :

   - page_pool_alloc -> page_pool_alloc_netmem
   - page_pool_get_dma_addr -> page_pool_get_dma_addr_netmem
   - page_pool_put_page -> page_pool_put_netmem

   Not all page APIs have netmem equivalents at the moment. If your driver
   relies on a missing netmem API, feel free to add and propose to netdev@ or
   reach out to almasrymina@google.com for help adding the netmem API.

4. The driver must use the following PP_FLAGS:

   - PP_FLAG_DMA_MAP: netmem is not dma-mappable by the driver. The driver
     must delegate the dma mapping to the page_pool.
   - PP_FLAG_DMA_SYNC_DEV: netmem dma addr is not necessarily dma-syncable
     by the driver. The driver must delegate the dma syncing to the page_pool.
   - PP_FLAG_ALLOW_UNREADABLE_NETMEM. The driver must specify this flag iff
     tcp-data-split is enabled.

5. The driver must not assume the netmem is readable and/or backed by pages.
   The netmem returned by the page_pool may be unreadable, in which case
   netmem_address() will return NULL. The driver must correctly handle
   unreadable netmem, i.e. don't attempt to handle its contents when
   netmem_address() is NULL.

   Ideally, drivers should not have to check the underlying netmem type via
   helpers like netmem_is_net_iov() or convert the netmem to any of its
   underlying types via netmem_to_page() or netmem_to_net_iov(). In most cases,
   netmem or page_pool helpers that abstract this complexity are provided
   (and more can be added).

6. The driver must use page_pool_dma_sync_netmem_for_cpu() in lieu of
   dma_sync_single_range_for_cpu(). For some memory providers, dma_syncing for
   CPU will be done by the page_pool, for others (particularly dmabuf memory
   provider), dma syncing for CPU is the responsibility of the userspace using
   dmabuf APIs. The driver must delegate the entire dma-syncing operation to
   the page_pool which will do it correctly.
