/* SPDX-License-Identifier: GPL-2.0
 *
 *	Network memory
 *
 *	Author:	Mina Almasry <almasrymina@google.com>
 */

#ifndef _NET_NETMEM_H
#define _NET_NETMEM_H

#include <linux/mm.h>
#include <net/net_debug.h>
#include <net/netmem_type.h>

/* net_iov */

DECLARE_STATIC_KEY_FALSE(page_pool_mem_providers);

/*  We overload the LSB of the struct page pointer to indicate whether it's
 *  a page or net_iov.
 */
#define NET_IOV 0x01UL

struct net_iov_area {
	/* Array of net_iovs for this area. */
	struct netmem_desc *niovs;
	size_t num_niovs;

	/* Offset into the dma-buf where this chunk starts.  */
	unsigned long base_virtual;
};

static inline struct net_iov_area *net_iov_owner(const struct netmem_desc *niov)
{
	return niov->owner;
}

static inline unsigned int net_iov_idx(const struct netmem_desc *niov)
{
	return niov - net_iov_owner(niov)->niovs;
}

/* netmem */

/**
 * typedef netmem_ref - a nonexistent type marking a reference to generic
 * network memory.
 *
 * A netmem_ref currently is always a reference to a struct page. This
 * abstraction is introduced so support for new memory types can be added.
 *
 * Use the supplied helpers to obtain the underlying memory pointer and fields.
 */
typedef unsigned long __bitwise netmem_ref;

static inline bool netmem_is_net_iov(const netmem_ref netmem)
{
	return (__force unsigned long)netmem & NET_IOV;
}

/**
 * __netmem_to_page - unsafely get pointer to the &page backing @netmem
 * @netmem: netmem reference to convert
 *
 * Unsafe version of netmem_to_page(). When @netmem is always page-backed,
 * e.g. when it's a header buffer, performs faster and generates smaller
 * object code (no check for the LSB, no WARN). When @netmem points to IOV,
 * provokes undefined behaviour.
 *
 * Return: pointer to the &page (garbage if @netmem is not page-backed).
 */
static inline struct page *__netmem_to_page(netmem_ref netmem)
{
	return (__force struct page *)netmem;
}

/* This conversion fails (returns NULL) if the netmem_ref is not struct page
 * backed.
 */
static inline struct page *netmem_to_page(netmem_ref netmem)
{
	if (WARN_ON_ONCE(netmem_is_net_iov(netmem)))
		return NULL;

	return __netmem_to_page(netmem);
}

static inline struct netmem_desc *netmem_to_net_iov(netmem_ref netmem)
{
	if (netmem_is_net_iov(netmem))
		return (struct netmem_desc *)((__force unsigned long)netmem &
					  ~NET_IOV);

	DEBUG_NET_WARN_ON_ONCE(true);
	return NULL;
}

static inline netmem_ref net_iov_to_netmem(struct netmem_desc *niov)
{
	return (__force netmem_ref)((unsigned long)niov | NET_IOV);
}

#define page_to_netmem(p)	(_Generic((p),			\
	const struct page *:	(__force const netmem_ref)(p),	\
	struct page *:		(__force netmem_ref)(p)))

static inline netmem_ref alloc_netmems_node(int nid, gfp_t gfp_mask,
		unsigned int order)
{
	return page_to_netmem(alloc_pages_node(nid, gfp_mask, order));
}

static inline unsigned long alloc_netmems_bulk_node(gfp_t gfp, int nid,
		unsigned long nr_netmems, netmem_ref *netmem_array)
{
	return alloc_pages_bulk_node(gfp, nid, nr_netmems,
			(struct page **)netmem_array);
}

static inline void put_netmem(netmem_ref netmem)
{
	put_page(netmem_to_page(netmem));
}

/**
 * virt_to_netmem - convert virtual memory pointer to a netmem reference
 * @data: host memory pointer to convert
 *
 * Return: netmem reference to the &page backing this virtual address.
 */
static inline netmem_ref virt_to_netmem(const void *data)
{
	return page_to_netmem(virt_to_page(data));
}

static inline int netmem_ref_count(netmem_ref netmem)
{
	/* The non-pp refcount of net_iov is always 1. On net_iov, we only
	 * support pp refcounting which uses the pp_ref_count field.
	 */
	if (netmem_is_net_iov(netmem))
		return 1;

	return page_ref_count(netmem_to_page(netmem));
}

static inline unsigned long netmem_pfn_trace(netmem_ref netmem)
{
	if (netmem_is_net_iov(netmem))
		return 0;

	return page_to_pfn(netmem_to_page(netmem));
}

static inline struct netmem_desc *__netmem_clear_lsb(netmem_ref netmem)
{
	return (struct netmem_desc *)((__force unsigned long)netmem & ~NET_IOV);
}

static inline struct page_pool *netmem_get_pp(netmem_ref netmem)
{
	return __netmem_clear_lsb(netmem)->pp;
}

static inline atomic_long_t *netmem_get_pp_ref_count_ref(netmem_ref netmem)
{
	return &__netmem_clear_lsb(netmem)->pp_ref_count;
}

static inline bool netmem_is_pref_nid(netmem_ref netmem, int pref_nid)
{
	/* NUMA node preference only makes sense if we're allocating
	 * system memory. Memory providers (which give us net_iovs)
	 * choose for us.
	 */
	if (netmem_is_net_iov(netmem))
		return true;

	return page_to_nid(netmem_to_page(netmem)) == pref_nid;
}

static inline netmem_ref netmem_compound_head(netmem_ref netmem)
{
	/* niov are never compounded */
	if (netmem_is_net_iov(netmem))
		return netmem;

	return page_to_netmem(compound_head(netmem_to_page(netmem)));
}

/**
 * __netmem_address - unsafely get pointer to the memory backing @netmem
 * @netmem: netmem reference to get the pointer for
 *
 * Unsafe version of netmem_address(). When @netmem is always page-backed,
 * e.g. when it's a header buffer, performs faster and generates smaller
 * object code (no check for the LSB). When @netmem points to IOV, provokes
 * undefined behaviour.
 *
 * Return: pointer to the memory (garbage if @netmem is not page-backed).
 */
static inline void *__netmem_address(netmem_ref netmem)
{
	return page_address(__netmem_to_page(netmem));
}

static inline void *netmem_address(netmem_ref netmem)
{
	if (netmem_is_net_iov(netmem))
		return NULL;

	return __netmem_address(netmem);
}

/**
 * netmem_is_pfmemalloc - check if @netmem was allocated under memory pressure
 * @netmem: netmem reference to check
 *
 * Return: true if @netmem is page-backed and the page was allocated under
 * memory pressure, false otherwise.
 */
static inline bool netmem_is_pfmemalloc(netmem_ref netmem)
{
	if (netmem_is_net_iov(netmem))
		return false;

	return page_is_pfmemalloc(netmem_to_page(netmem));
}

static inline unsigned long netmem_get_dma_addr(netmem_ref netmem)
{
	return __netmem_clear_lsb(netmem)->dma_addr;
}

#endif /* _NET_NETMEM_H */
