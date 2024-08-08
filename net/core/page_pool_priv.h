/* SPDX-License-Identifier: GPL-2.0 */

#ifndef __PAGE_POOL_PRIV_H
#define __PAGE_POOL_PRIV_H

s32 page_pool_inflight(const struct page_pool *pool, bool strict);

int page_pool_list(struct page_pool *pool);
void page_pool_detached(struct page_pool *pool);
void page_pool_unlist(struct page_pool *pool);
bool page_pool_set_dma_addr_netmem(netmem_ref netmem, dma_addr_t addr);

static inline bool page_pool_set_dma_addr(struct page *page, dma_addr_t addr)
{
	return page_pool_set_dma_addr_netmem(page_to_netmem(page), addr);
}

#endif
