// SPDX-License-Identifier: GPL-2.0 or BSD-3-Clause
/*
 * Copyright(c) 2025-2026 Cornelis Networks, Inc.
 */

#include <linux/types.h>

#include "hfi2.h"
#include "common.h"
#include "pinning.h"
#include "mmu_rb.h"
#include "user_sdma.h"
#include "trace.h"

struct sdma_mmu_node {
	struct mmu_rb_node rb;
	struct hfi2_user_sdma_pkt_q *pq;
	struct page **pages;
	unsigned int npages;
};

static bool sdma_rb_filter(struct mmu_rb_node *node, unsigned long addr,
			   unsigned long len);
static int sdma_rb_evict(void *arg, struct mmu_rb_node *mnode, void *arg2,
			 bool *stop);
static void sdma_rb_remove(void *arg, struct mmu_rb_node *mnode);

static const struct mmu_rb_ops sdma_rb_ops = {
	.filter = sdma_rb_filter,
	.evict = sdma_rb_evict,
	.remove = sdma_rb_remove,
};

static int init_system_pinning(struct hfi2_user_sdma_pkt_q *pq)
{
	struct hfi2_devdata *dd = pq->dd;
	struct mmu_rb_handler **handler = (struct mmu_rb_handler **)
		&PINNING_STATE(pq, HFI2_MEMINFO_TYPE_SYSTEM);
	int ret;

	ret = hfi2_mmu_rb_register(pq, &sdma_rb_ops, dd->hfi2_wq,
				   handler);
	if (ret)
		dd_dev_err(dd,
			   "[%u:%u] Failed to register system memory DMA support with MMU: %d\n",
			   pq->ctxt, pq->subctxt, ret);
	return ret;
}

static void free_system_pinning(struct hfi2_user_sdma_pkt_q *pq)
{
	struct mmu_rb_handler *handler =
		PINNING_STATE(pq, HFI2_MEMINFO_TYPE_SYSTEM);

	if (handler)
		hfi2_mmu_rb_unregister(handler);
}

static u32 sdma_cache_evict(struct hfi2_user_sdma_pkt_q *pq, u32 npages)
{
	struct evict_data evict_data;
	struct mmu_rb_handler *handler =
		PINNING_STATE(pq, HFI2_MEMINFO_TYPE_SYSTEM);

	evict_data.cleared = 0;
	evict_data.target = npages;
	hfi2_mmu_rb_evict(handler, &evict_data);
	return evict_data.cleared;
}

static void unpin_vector_pages(struct mm_struct *mm, struct page **pages,
			       unsigned int start, unsigned int npages)
{
	hfi2_release_user_pages(mm, pages + start, npages, false);
	kfree(pages);
}

static inline struct mm_struct *mm_from_sdma_node(struct sdma_mmu_node *e)
{
	return e->rb.handler->mn.mm;
}

static void free_system_node(struct sdma_mmu_node *e)
{
	if (e->npages) {
		trace_unpin_sdma_mem(mm_from_sdma_node(e), HFI2_MEMINFO_TYPE_SYSTEM, 0,
				     e, e->rb.addr, e->rb.len,
				     0, (e->npages * PAGE_SIZE));
		unpin_vector_pages(mm_from_sdma_node(e), e->pages, 0,
				   e->npages);
		atomic_sub(e->npages, &e->pq->n_locked);
	}
	kfree(e);
}

/*
 * A valid @n covers the @start and at least some of [@start, @end)
 */
static bool covered_by(struct mmu_rb_node *n, unsigned long start,
		       unsigned long end)
{
	return n->addr <= start && start < (n->addr + n->len);
}

/*
 * kref from kref_get() in here will be transferred to the struct
 * user_sdma_txreq to which the retval struct sdma_mmu_node* is being used for.
 */
static struct sdma_mmu_node *find_system_node(struct mmu_rb_handler *handler,
					      unsigned long start,
					      unsigned long end)
{
	struct mmu_rb_node *rb_node;
	unsigned long flags;

	spin_lock_irqsave(&handler->lock, flags);
	rb_node = hfi2_mmu_rb_get_first(handler, start, (end - start));
	if (!rb_node) {
		handler->misses++;
		spin_unlock_irqrestore(&handler->lock, flags);
		return NULL;
	}

	/* This kref will become the user_sdma_request's kref */
	kref_get(&rb_node->refcount);
	handler->hits++;
	spin_unlock_irqrestore(&handler->lock, flags);

	return container_of(rb_node, struct sdma_mmu_node, rb);
}

/*
 * @start on page boundary.
 */
static int pin_system_pages(struct user_sdma_request *req, struct sdma_mmu_node *e,
			    uintptr_t start, int npages)
{
	struct hfi2_user_sdma_pkt_q *pq = req->pq;
	int pinned, cleared;
	struct page **pages;

	pages = kcalloc(npages, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

retry:
	if (!hfi2_can_pin_pages(pq->dd, current->mm, atomic_read(&pq->n_locked),
				npages)) {
		SDMA_DBG(req, "Evicting: nlocked %u npages %u",
			 atomic_read(&pq->n_locked), npages);
		cleared = sdma_cache_evict(pq, npages);
		if (cleared >= npages)
			goto retry;
	}

	pinned = hfi2_acquire_user_pages(current->mm, start, npages, 0, pages);
	trace_pin_sdma_mem(current->mm, HFI2_MEMINFO_TYPE_SYSTEM, start,
			   npages * PAGE_SIZE, pinned, false, e,
			   0, max(pinned, 0) * PAGE_SIZE);
	if (pinned < 0) {
		kfree(pages);
		return pinned;
	}
	if (pinned != npages) {
		unpin_vector_pages(current->mm, pages, 0, pinned);
		return -EFAULT;
	}
	e->rb.addr = start;
	e->rb.len = (npages * PAGE_SIZE);
	e->pages = pages;
	e->npages = npages;
	atomic_add(pinned, &pq->n_locked);
	return 0;
}

/*
 * kref refcount on returned node will be 2 on successful addition: one kref
 * from kref_init() for mmu_rb_handler and one kref to prevent the returned
 * node from being released until after the returned node is assigned to an
 * SDMA descriptor (struct sdma_desc) under add_from_iovec(), even if the
 * virtual address range for the returned node is invalidated between now and
 * then.
 *
 * @return ERR_PTR() or struct sdma_mmu_node *
 */
static struct sdma_mmu_node *
add_system_pinning(struct user_sdma_request *req, unsigned long start,
		   unsigned long len)

{
	struct hfi2_user_sdma_pkt_q *pq = req->pq;
	struct sdma_mmu_node *e;
	int ret;

	e = kzalloc_obj(*e, GFP_KERNEL);
	if (!e)
		return ERR_PTR(-ENOMEM);

	/* First kref becomes the mmu_rb_handler's kref */
	kref_init(&e->rb.refcount);

	/* This kref will become the user_sdma_request's kref */
	kref_get(&e->rb.refcount);

	e->pq = pq;
	ret = pin_system_pages(req, e, start, PFN_DOWN(len));
	if (!ret) {
		ret = hfi2_mmu_rb_insert(PINNING_STATE(pq, HFI2_MEMINFO_TYPE_SYSTEM), &e->rb);
		if (ret) {
			free_system_node(e);
			return ERR_PTR(ret);
		}
		return e;
	}
	kfree(e);
	return ERR_PTR(ret);
}

/*
 * @return ERR_PTR() or struct sdma_mmu_node *
 */
static struct sdma_mmu_node *
get_system_cache_entry(struct user_sdma_request *req, size_t req_start,
		       size_t req_len)
{
	struct hfi2_user_sdma_pkt_q *pq = req->pq;
	u64 start = ALIGN_DOWN(req_start, PAGE_SIZE);
	u64 end = PFN_ALIGN(req_start + req_len);
	struct mmu_rb_handler *handler =
		PINNING_STATE(pq, HFI2_MEMINFO_TYPE_SYSTEM);

	if ((end - start) == 0) {
		SDMA_DBG(req,
			 "Request for empty cache entry req_start %lx req_len %lx start %llx end %llx",
			 req_start, req_len, start, end);
		return ERR_PTR(-EINVAL);
	}

	SDMA_DBG(req, "req_start %lx req_len %lu", req_start, req_len);

	while (1) {
		struct sdma_mmu_node *e =
			find_system_node(handler, start, end);
		u64 prepend_len = 0;

		SDMA_DBG(req, "e %p start %llx end %llx", e, start, end);
		if (!e) {
			e = add_system_pinning(req, start, end - start);
			if (IS_ERR(e) && PTR_ERR(e) == -EEXIST) {
				/*
				 * Another execution context has inserted a
				 * conficting entry first.
				 */
				continue;
			}
			return e;
		}

		if (e->rb.addr <= start) {
			/*
			 * This entry covers at least part of the region. If it doesn't extend
			 * to the end, then this will be called again for the next segment.
			 */
			return e;
		}

		SDMA_DBG(req, "prepend: e->rb.addr %lx, e->rb.refcount %d",
			 e->rb.addr, kref_read(&e->rb.refcount));
		prepend_len = e->rb.addr - start;

		/*
		 * e will not be returned, instead a new node will be. So
		 * release the reference.
		 */
		kref_put(&e->rb.refcount, hfi2_mmu_rb_release);

		/* Prepend a node to cover the beginning of the allocation */
		e = add_system_pinning(req, start, prepend_len);
		if (IS_ERR(e) && PTR_ERR(e) == -EEXIST) {
			/* Another execution context has inserted a conficting entry first. */
			continue;
		}
		return e;
	}
}

static void sdma_mmu_node_put(void *ctx)
{
	struct sdma_mmu_node *n = ctx;

	kref_put(&n->rb.refcount, hfi2_mmu_rb_release);
}

static int add_from_entry(struct user_sdma_request *req,
			  struct user_sdma_txreq *tx,
			  struct sdma_mmu_node *e,
			  size_t start,
			  size_t from_entry)
{
	struct hfi2_user_sdma_pkt_q *pq = req->pq;
	unsigned int page_offset;
	unsigned int from_page;
	size_t page_index;

	/*
	 * Because the cache may be more fragmented than the memory that is being accessed,
	 * it's not strictly necessary to have a descriptor per cache entry.
	 */
	while (from_entry) {
		int ret;

		page_index = PFN_DOWN(start - e->rb.addr);
		if (page_index >= e->npages) {
			SDMA_DBG(req,
				 "Request for page_index %zu >= e->npages %u",
				 page_index, e->npages);
			return -EINVAL;
		}

		page_offset = start - ALIGN_DOWN(start, PAGE_SIZE);
		from_page = PAGE_SIZE - page_offset;
		if (from_page >= from_entry)
			from_page = from_entry;

		ret = sdma_txadd_page(pq->dd, &tx->txreq,
				      e->pages[page_index],
				      page_offset, from_page);
		if (ret) {
			/*
			 * When there's a failure, the entire request is freed by
			 * user_sdma_send_pkts().
			 */
			SDMA_DBG(req,
				 "sdma_txadd_page failed %d page_index %lu page_offset %u from_page %u",
				 ret, page_index, page_offset, from_page);
			return ret;
		}
		start += from_page;
		from_entry -= from_page;
	}
	return 0;
}

/*
 * On success, prior to returning, adjusts @remaining, @req->iov_idx,
 * and @req->iov[req->iov_idx].offset to reflect the data that has
 * been consumed.
 *
 * @remaining: as input, maximum amount of data to add from iovecs at
 *   @iov and after. As output, the amount of data remaining after
 *   data was added to packet.
 */
static int add_to_txreq(struct user_sdma_request *req,
			struct user_sdma_txreq *tx,
			struct user_sdma_iovec *iov,
			u32 *remaining)
{
	struct mmu_rb_handler *cache =
		PINNING_STATE(req->pq, HFI2_MEMINFO_TYPE_SYSTEM);
	struct user_sdma_pinref *ht =
		hfi2_user_sdma_mru_ref(tx, HFI2_MEMINFO_TYPE_SYSTEM);
	struct sdma_mmu_node *e = (ht ? ht->ptr : NULL);
	u32 rem = *remaining;
	int ret = 0;

	while (rem && iov->type == HFI2_MEMINFO_TYPE_SYSTEM) {
		u64 start = (uintptr_t)iov->iov.iov_base + iov->offset;
		u64 end = (uintptr_t)iov->iov.iov_base + iov->iov.iov_len;
		u64 from_this;

		/* Keep using e as long as it covers [start,end) */
		if (!e || !covered_by(&e->rb, start, end)) {
			if (ht)
				cache->hint_misses++;
			e = get_system_cache_entry(req, start, end - start);
			if (IS_ERR(e))
				return PTR_ERR(e);
			/* transfer e's kref to tx */
			ret = hfi2_user_sdma_add_ref(tx, e, iov->type);
			if (ret) {
				sdma_mmu_node_put(e);
				return ret;
			}
		} else if (ht) {
			hfi2_user_sdma_touch_ref(tx, ht);
			cache->hint_hits++;
		}
		ht = NULL;

		/* Limit by remaining data in e or iovec, then by caller remaining */
		from_this = min((e->rb.addr + e->rb.len) - start, end - start);
		from_this = min_t(u64, from_this, rem);
		ret = add_from_entry(req, tx, e, start, from_this);
		SDMA_DBG(req, "iov %p iov_range [%llx,%llx) e %p e_range [%lx,%lx) from_this %llu ret %d",
			 iov, start, end, e, e->rb.addr, e->rb.addr + e->rb.len,
			 from_this, ret);
		if (ret) {
			/* tx destructor will kref_put() e */
			return ret;
		}

		rem -= from_this;
		iov->offset += from_this;
		if ((u64)iov->offset >= iov->iov.iov_len) {
			req->iov_idx++;
			iov++;
		}
	}
	*remaining = rem;
	return 0;
}

static void add_system_stats(const struct mmu_rb_node *e, void *arg)
{
	struct hfi2_pin_stats *stats = arg;

	stats->cache_entries++;
	/*
	 * '- 1' to account for kref held by mmu_rb_handler.
	 *
	 * We're assured that mmu_rb_handler has kref for e because:
	 * - This function is called in a for-each loop over mmu_rb_handler's rb_nodes
	 * - That loop is called inside of an mmu_rb_handler->lock critical section
	 * - Once added to an mmu_rb_handler's cache, mmu_rb_nodes can only be
	 *    destroyed or queued for destruction inside an mmu_rb_handler->lock
	 *    critical section
	 *
	 * So the fact that e was passed in here means it is still in an mmu_rb_handler's cache.
	 *
	 * That said, kref_read() here can overcount the number of actual
	 * sdma_descs holding references to e. That is because user_sdma
	 * and mmu_rb_handler code take additional krefs to prevent
	 * mmu_rb_nodes from being destroyed after the user SDMA request is
	 * submitted and gets as far as pinning pages, even if the userspace
	 * virtual address range is invalidated in the meantime. I.e. once the
	 * user SDMA request gets as far as pinning pages, those pages will
	 * remain resident up until the SDMA engine completes the request.
	 */
	stats->total_refcounts += kref_read(&e->refcount) - 1;
	stats->total_bytes += e->len;
}

static int get_system_stats(struct hfi2_user_sdma_pkt_q *pq, int index,
			    struct hfi2_pin_stats *stats)
{
	struct mmu_rb_handler *handler =
		PINNING_STATE(pq, HFI2_MEMINFO_TYPE_SYSTEM);
	unsigned long next = 0;

	if (index == -1) {
		stats->index = 1;
		return 0;
	}

	if (index != 0)
		return -EINVAL;

	stats->memtype = HFI2_MEMINFO_TYPE_SYSTEM;
	stats->id = 0;
	while (next != ~0UL) {
		unsigned long flags;

		spin_lock_irqsave(&handler->lock, flags);
		/* Take stats on 100 nodes at a time.
		 * This is a balance between time/cost of the operation and
		 * the latency of other operations waiting for the lock.
		 */
		next = hfi2_mmu_rb_for_n(handler, next, 100, add_system_stats,
					 stats);
		spin_unlock_irqrestore(&handler->lock, flags);
		/* This is to allow the lock to be acquired from other places. */
		ndelay(100);
	}

	stats->hits = handler->hits;
	stats->misses = handler->misses;
	stats->hint_hits = handler->hint_hits;
	stats->hint_misses = handler->hint_misses;
	stats->internal_evictions = handler->internal_evictions;
	stats->external_evictions = handler->external_evictions;

	return 0;
};

static struct pinning_interface system_pinning_interface = {
	.init = init_system_pinning,
	.free = free_system_pinning,
	.add_to_sdma_packet = add_to_txreq,
	.put = sdma_mmu_node_put,
	.get_stats = get_system_stats,
};

void hfi2_register_system_pinning_interface(void)
{
	hfi2_register_pinning_interface(HFI2_MEMINFO_TYPE_SYSTEM,
				   &system_pinning_interface);
	pr_info("%s System memory DMA support enabled\n", "hfi2");
}

void hfi2_deregister_system_pinning_interface(void)
{
	hfi2_deregister_pinning_interface(HFI2_MEMINFO_TYPE_SYSTEM);
}

static bool sdma_rb_filter(struct mmu_rb_node *e, unsigned long addr,
			   unsigned long len)
{
	return (bool)(e->addr == addr);
}

/*
 * Return 1 to remove the node from the rb tree and call the remove op.
 *
 * Called with the rb tree lock held.
 */
static int sdma_rb_evict(void *arg, struct mmu_rb_node *mnode,
			 void *evict_arg, bool *stop)
{
	struct sdma_mmu_node *e =
		container_of(mnode, struct sdma_mmu_node, rb);
	struct evict_data *evict_data = evict_arg;

	/* e will be evicted, add its pages to our count */
	evict_data->cleared += e->npages;

	/* have enough pages been cleared? */
	if (evict_data->cleared >= evict_data->target)
		*stop = true;

	return 1; /* remove this node */
}

static void sdma_rb_remove(void *arg, struct mmu_rb_node *mnode)
{
	struct sdma_mmu_node *e =
		container_of(mnode, struct sdma_mmu_node, rb);

	free_system_node(e);
}
