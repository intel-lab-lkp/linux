// SPDX-License-Identifier: GPL-2.0
/* AF_XDP RX in the device's buffer layout
 *
 * Userspace lends page-sized UMEM chunks through the FILL ring. Each chunk
 * becomes a net_iov of a page pool memory provider, and the device splits it
 * as it splits pages: a chunk may hold several RX buffers, and a packet may
 * span chunks.
 *
 * The kernel maps the chunks through the UMEM mapping, so XDP programs read
 * and write the packets in place. The kernel stores none of its own data in
 * the chunks: the shared info of a packet lives in the sink, see
 * %XDP_FLAGS_SINFO_EXT, and the core turns every verdict that would build an
 * skb or a frame in the buffers into a drop.
 */

#include <linux/nospec.h>
#include <net/netdev_queues.h>
#include <net/netdev_rx_queue.h>
#include <net/page_pool/helpers.h>
#include <net/page_pool/memory_provider.h>
#include <net/xdp_sock_drv.h>

#include "../core/dev.h"
#include "xsk_queue.h"
#include "xsk_sink.h"

#define XSK_SINK_DMA_ATTR (DMA_ATTR_WEAK_ORDERING | DMA_ATTR_SKIP_CPU_SYNC)
/* Chunks a refill tops up to with released ones */
#define XSK_SINK_RESERVE	32

/* &net_iov.mp_state: whether userspace has the chunk or lent it */
enum {
	XSK_SINK_USER,
	XSK_SINK_LENT,
};

struct xsk_sink {
	struct net_iov_area area;
	struct device *dev;
	/* Valid until the socket unbinds */
	struct xsk_buff_pool *pool;
	/* NULL once no queue uses the provider */
	struct netdev_rx_queue *rxq;
	/* Protects fq, which stays set while the UMEM is mapped, and keeps
	 * refills the only taker of released chunks. Besides the queue NAPI,
	 * drivers refill from process context, as bnxt fills the rings of a
	 * new queue while the old one still runs.
	 */
	spinlock_t lock;
	struct xsk_queue *fq;
	refcount_t refs;
	/* One node per chunk: chunks that page pools released, from any
	 * context
	 */
	struct llist_node *nodes;
	struct llist_head released;
};

static void xsk_sink_free(struct xsk_sink *sink)
{
	kvfree(sink->nodes);
	kvfree(sink->area.niovs);
	kfree(sink);
}

static void xsk_sink_put(struct xsk_sink *sink)
{
	if (refcount_dec_and_test(&sink->refs))
		xsk_sink_free(sink);
}

static int xsk_sink_init(struct page_pool *pp)
{
	struct xsk_sink *sink = pp->mp_priv;

	/* Each net_iov is one chunk, mapped for one device */
	if (pp->p.order)
		return -E2BIG;
	if (pp->p.dev != sink->dev || pp->p.offset + pp->p.max_len > PAGE_SIZE)
		return -EINVAL;
	/* Rings that keep every chunk posted leave none to replace the ones
	 * the device filled: mlx5 posts about 16K pages per queue at MTU
	 * 9000.
	 */
	if (pp->p.pool_size + PP_ALLOC_CACHE_SIZE >= sink->area.num_niovs)
		return -ENOSPC;

	refcount_inc(&sink->refs);
	return 0;
}

static void xsk_sink_destroy(struct page_pool *pp)
{
	xsk_sink_put(pp->mp_priv);
}

/* Takes the chunks that FILL entries lend into the cache of @pp, which still
 * holds those it gave back before. A chunk that an earlier page pool still
 * holds goes back to it. Returns the number of chunks taken.
 */
static u32 xsk_sink_take_fill(struct xsk_sink *sink, struct page_pool *pp)
{
	struct net_iov *niov, *niovs = sink->area.niovs;
	u32 i, entries, cons, mask, nr, n = 0;
	struct xsk_queue *fq = sink->fq;
	struct xdp_umem_ring *ring;
	struct page_pool *owner;
	netmem_ref netmem;
	u64 addr;

	lockdep_assert_held(&sink->lock);

	/* One cache worth, bounding NAPI work */
	entries = fq ? xskq_cons_nb_entries(fq, PP_ALLOC_CACHE_SIZE) : 0;
	if (!entries)
		return 0;

	nr = sink->area.num_niovs;
	ring = (struct xdp_umem_ring *)fq->ring;
	mask = fq->ring_mask;
	cons = fq->cached_cons;
	for (i = 0; i < entries; i++) {
		addr = ring->desc[(cons + i) & mask];
		niov = &niovs[array_index_nospec((u32)(addr >> PAGE_SHIFT),
						 nr)];
		/* A chunk lent twice would be in two places at once */
		if (unlikely(addr >= (u64)nr << PAGE_SHIFT ||
			     READ_ONCE(niov->mp_state) == XSK_SINK_LENT)) {
			fq->invalid_descs++;
			continue;
		}

		WRITE_ONCE(niov->mp_state, XSK_SINK_LENT);
		netmem = net_iov_to_netmem(niov);
		owner = netmem_get_pp(netmem);
		if (owner && owner != pp) {
			page_pool_put_unrefed_netmem(owner, netmem, -1, false);
			continue;
		}
		if (!owner)
			net_mp_niov_set_page_pool(pp, niov);
		pp->alloc.cache[n++] = netmem;
	}

	xskq_cons_release_n(fq, entries);
	__xskq_cons_release(fq);
	return n;
}

/* Tops up the cache of @pp, which holds @n chunks, with chunks that page
 * pools released. A reserve, not a source. Each chunk it adds stays in
 * circulation, so take only what FILL could not give. The lock keeps this
 * the only consumer of the list, as llist_del_first() needs.
 */
static u32 xsk_sink_take_released(struct xsk_sink *sink, struct page_pool *pp,
				  u32 n)
{
	struct llist_node *node;
	struct net_iov *niov;

	lockdep_assert_held(&sink->lock);

	/* The page pool cache has 8 entries with 64K pages */
	while (n < min(XSK_SINK_RESERVE, PP_ALLOC_CACHE_SIZE)) {
		node = llist_del_first(&sink->released);
		if (!node)
			break;
		niov = &sink->area.niovs[node - sink->nodes];
		net_mp_niov_set_page_pool(pp, niov);
		pp->alloc.cache[n++] = net_iov_to_netmem(niov);
	}
	return n;
}

static netmem_ref xsk_sink_alloc_netmems(struct page_pool *pp, gfp_t gfp)
{
	struct xsk_sink *sink = pp->mp_priv;
	/* The NAPI owns the need_wakeup flags */
	bool napi = in_softirq() && !in_hardirq();
	struct xsk_queue *fq;
	dma_addr_t dma;
	u32 i, n;

	/* The page pool refills only an empty cache */
	if (WARN_ON_ONCE(pp->alloc.count))
		return 0;

	spin_lock_bh(&sink->lock);
	n = xsk_sink_take_fill(sink, pp);
	n = xsk_sink_take_released(sink, pp, n);
	fq = sink->fq;
	/* Ask for kicks only while FILL runs dry, as zero copy drivers do: a
	 * kick schedules the NAPI, which a busy poller would rather run
	 * itself. Process context refills new queues only and leaves the
	 * flags to their NAPI and to bind.
	 */
	if (napi && fq && sink->pool->uses_need_wakeup) {
		if (n)
			xsk_clear_rx_need_wakeup(sink->pool);
		else
			xsk_set_rx_need_wakeup(sink->pool);
	}
	if (fq && !n)
		fq->queue_empty_descs++;
	spin_unlock_bh(&sink->lock);
	if (!n)
		return 0;

	for (i = 0; i < n; i++) {
		dma = page_pool_get_dma_addr_netmem(pp->alloc.cache[i]);
		dma_sync_single_range_for_device(pp->p.dev, dma, pp->p.offset,
						 pp->p.max_len, pp->p.dma_dir);
	}

	pp->alloc.count = n - 1;
	return pp->alloc.cache[n - 1];
}

static bool xsk_sink_release_netmem(struct page_pool *pp, netmem_ref netmem)
{
	struct net_iov *niov = netmem_to_net_iov(netmem);
	struct xsk_sink *sink = pp->mp_priv;

	net_mp_niov_clear_page_pool(niov);
	llist_add(&sink->nodes[net_iov_idx(niov)], &sink->released);
	return false;
}

static int xsk_sink_nl_fill(void *mp_priv, struct sk_buff *rsp,
			    struct netdev_rx_queue *rxq)
{
	return 0;
}

/* No queue DMAs to the UMEM any longer */
static void xsk_sink_stop(struct xsk_sink *sink)
{
	bool unmap;

	spin_lock_bh(&sink->lock);
	WRITE_ONCE(sink->rxq, NULL);
	unmap = sink->fq;
	sink->fq = NULL;
	spin_unlock_bh(&sink->lock);

	/* Wait for wakeups that found the sink or the queue */
	synchronize_net();
	if (unmap)
		xp_dma_unmap(sink->pool, XSK_SINK_DMA_ATTR);
}

/* Device unregister, after the queue stopped */
static void xsk_sink_uninstall(void *mp_priv, struct netdev_rx_queue *rxq)
{
	memset(&rxq->mp_params, 0, sizeof(rxq->mp_params));
	xsk_sink_stop(mp_priv);
}

static const struct memory_provider_ops xsk_sink_ops = {
	.alloc_netmems	= xsk_sink_alloc_netmems,
	.release_netmem	= xsk_sink_release_netmem,
	.init		= xsk_sink_init,
	.destroy	= xsk_sink_destroy,
	.nl_fill	= xsk_sink_nl_fill,
	.uninstall	= xsk_sink_uninstall,
	.flags		= MP_XDP_ONLY,
};

static struct xsk_sink *xsk_sink_create(struct xsk_buff_pool *pool,
					struct device *dev)
{
	u32 i, nr = pool->umem->chunks;
	struct xsk_sink *sink;
	struct net_iov *niov;
	dma_addr_t dma;

	sink = kzalloc_obj(*sink, GFP_KERNEL_ACCOUNT);
	if (!sink)
		return ERR_PTR(-ENOMEM);

	/* See &xdp_rxq_info.sink_area */
	BUILD_BUG_ON(offsetof(struct xsk_sink, area));

	sink->area.niovs = kvcalloc(nr, sizeof(*niov), GFP_KERNEL_ACCOUNT);
	sink->nodes = kvcalloc(nr, sizeof(*sink->nodes), GFP_KERNEL_ACCOUNT);
	if (!sink->area.niovs || !sink->nodes) {
		xsk_sink_free(sink);
		return ERR_PTR(-ENOMEM);
	}

	sink->area.num_niovs = nr;
	/* Chunks are pages, see XDP_UMEM_DRV_LAYOUT */
	sink->area.xdp_vaddr = pool->addrs;
	sink->area.xdp_vsize = (size_t)nr << PAGE_SHIFT;
	sink->area.xdp_shift = PAGE_SHIFT;
	sink->dev = dev;
	sink->pool = pool;
	sink->fq = pool->fq;
	spin_lock_init(&sink->lock);
	init_llist_head(&sink->released);
	refcount_set(&sink->refs, 1);

	/* Chunks are pages, see XDP_UMEM_DRV_LAYOUT */
	for (i = 0; i < nr; i++) {
		niov = &sink->area.niovs[i];
		dma = pool->dma_pages[i] & ~XSK_NEXT_PG_CONTIG_MASK;
		net_iov_init(niov, &sink->area, NET_IOV_XSK);
		if (net_mp_niov_set_dma_addr(niov, dma)) {
			xsk_sink_free(sink);
			return ERR_PTR(-ERANGE);
		}
	}

	return sink;
}

/* Sockets sharing the pool find no sink after the grace period of
 * xsk_sink_stop(), see xsk_wakeup()
 */
static void xsk_sink_detach(struct xsk_buff_pool *pool)
{
	struct xsk_sink *sink = pool->sink;

	WRITE_ONCE(pool->sink, NULL);
	xsk_sink_stop(sink);
	xsk_sink_put(sink);
	static_branch_dec(&xdp_sink_key);
}

/**
 * xsk_sink_bind - install the UMEM of @pool as the provider of an RX queue
 * @pool: buffer pool with an XDP_UMEM_DRV_LAYOUT UMEM
 * @dev: device of the queue
 * @queue_id: RX queue
 * @flags: bind flags
 *
 * Context: RTNL and the instance lock of @dev.
 * Return: 0 on success. No copy mode takes over on error.
 */
int xsk_sink_bind(struct xsk_buff_pool *pool, struct net_device *dev,
		  u16 queue_id, u16 flags)
{
	u32 needed = NETDEV_XDP_ACT_BASIC | NETDEV_XDP_ACT_REDIRECT;
	struct pp_memory_provider_params p = { .mp_ops = &xsk_sink_ops };
	struct device *dma_dev;
	struct xsk_sink *sink;
	int err;

	if (flags & XDP_USE_SG)
		needed |= NETDEV_XDP_ACT_RX_SG;
	if (flags & XDP_COPY || !dev->queue_mgmt_ops ||
	    (dev->xdp_features & needed) != needed ||
	    queue_id >= dev->real_num_rx_queues)
		return -EOPNOTSUPP;

	/* The program of another device would see a leased queue's packets */
	if (netif_rxq_is_leased(dev, queue_id))
		return -EOPNOTSUPP;

	/* The device of the queue's page pools, which may differ per queue */
	dma_dev = netdev_queue_get_dma_dev(dev, queue_id, NETDEV_QUEUE_TYPE_RX);
	if (!dma_dev)
		return -EOPNOTSUPP;

	/* Without a program, drivers would build skbs from the buffers */
	if (!dev_xdp_drv_prog_held(dev))
		return -ENOENT;

	err = xp_dma_map(pool, dma_dev, XSK_SINK_DMA_ATTR, pool->umem->pgs,
			 pool->umem->npgs);
	if (err)
		return err;

	sink = xsk_sink_create(pool, dma_dev);
	if (IS_ERR(sink)) {
		xp_dma_unmap(pool, XSK_SINK_DMA_ATTR);
		return PTR_ERR(sink);
	}

	/* Until the first refill of the NAPI: the queue may need FILL to
	 * start. Set before the NAPI runs, which owns the flags after that.
	 */
	if (pool->uses_need_wakeup)
		xsk_set_rx_need_wakeup(pool);

	/* The core keeps sink buffers to the socket once the queue runs */
	static_branch_inc(&xdp_sink_key);
	sink->rxq = __netif_get_rx_queue(dev, queue_id);
	pool->sink = sink;
	p.mp_priv = sink;
	err = netif_mp_open_rxq(dev, queue_id, &p, NULL);
	if (err) {
		xsk_sink_detach(pool);
		return err;
	}

	return 0;
}

/* Context: RTNL and the instance lock of the device */
void xsk_sink_unbind(struct xsk_buff_pool *pool)
{
	struct xsk_sink *sink = pool->sink;
	struct pp_memory_provider_params p = {
		.mp_ops = &xsk_sink_ops,
		.mp_priv = sink,
	};

	/* Unregister may have uninstalled the provider already. A queue that
	 * cannot restart without it may still use the UMEM: stop the device.
	 */
	if (READ_ONCE(sink->rxq) &&
	    netif_mp_close_rxq(pool->netdev, pool->queue_id, &p))
		netif_close(pool->netdev);
	xsk_sink_detach(pool);
}
