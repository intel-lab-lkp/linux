// SPDX-License-Identifier: GPL-2.0
/* AF_XDP RX in the device's buffer layout
 *
 * Userspace lends page-sized UMEM chunks through the FILL ring. Each chunk
 * becomes a net_iov of a page pool memory provider, and the device splits it
 * as it splits pages: a chunk may hold several RX buffers, and a packet may
 * span chunks. Each buffer of a packet that XDP redirects to the socket
 * becomes one RX descriptor, at the address the device chose.
 *
 * The socket drops the page pool reference of each buffer it takes. A chunk
 * goes back to userspace once nothing in the kernel uses it, if a buffer of
 * it reached the socket: the descriptor of the buffer that freed it carries
 * XDP_RX_CHUNK_FREE, or, when the device lets go of the chunk later, an
 * empty descriptor at the chunk does:
 *
 *   FILL 0x7000
 *   RX {0x7100, A}
 *   RX {0x7900, B, XDP_RX_CHUNK_FREE}  or  RX {0x7000, 0, XDP_RX_CHUNK_FREE}
 *
 * Such a chunk stays charged to its page pool, which takes it straight back
 * when userspace lends it again. Chunks whose buffers all stayed in the
 * kernel, as for dropped packets, stay in the kernel.
 *
 * The kernel maps the chunks through the UMEM mapping, so XDP programs read
 * and write the packets in place, and their metadata lands in front of the
 * first buffer. The kernel stores none of its own data in the chunks: the
 * shared info of a packet lives in the sink, see %XDP_FLAGS_SINFO_EXT, and
 * the core turns every verdict that would build an skb or a frame in the
 * buffers into a drop.
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
	/* The socket that receives, set once */
	struct xdp_sock *xs;
	/* One node per chunk, on at most one list. Page pools add chunks
	 * that they released, and chunks that they handed off to go back to
	 * userspace, from any context.
	 */
	struct llist_node *nodes;
	struct llist_head released;
	struct llist_head handed;
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

/* Posts the chunks that page pools handed off as empty RX descriptors
 * while the ring has room, and puts the rest back on the list. Their order
 * does not matter, as userspace tells chunks by their address. Only one
 * NAPI runs a queue at a time, so this is the only consumer of the list
 * while the queue runs, and xsk_sink_scrub() the only one after it stopped.
 *
 * Context: the queue NAPI
 */
static void xsk_sink_give_back(struct xsk_sink *sink, struct xdp_sock *xs)
{
	struct llist_node *node, *last;
	u32 idx;

	if (llist_empty(&sink->handed) || !xskq_prod_nb_free(xs->rx, 1))
		return;

	node = llist_del_all(&sink->handed);
	while (node && xskq_prod_nb_free(xs->rx, 1)) {
		idx = node - sink->nodes;
		node = node->next;
		WRITE_ONCE(sink->area.niovs[idx].mp_state, XSK_SINK_USER);
		__xskq_prod_reserve_desc(xs->rx, (u64)idx << PAGE_SHIFT, 0,
					 XDP_RX_CHUNK_FREE);
	}
	if (!node)
		return;

	/* The ring is full, so userspace is behind */
	for (last = node; last->next; last = last->next)
		;
	llist_add_batch(node, last, &sink->handed);
}

/* Drops the reference of the buffer of @niov that the socket took at the
 * last RX descriptor. Only the queue NAPI changes the count of a chunk that
 * delivers to the socket, so this needs no atomic.
 */
static void xsk_sink_drop_buf(struct xsk_sink *sink, struct xdp_sock *xs,
			      struct net_iov *niov)
{
	struct xdp_rxtx_ring *ring;
	atomic_long_t *count;
	long left;

	count = netmem_get_pp_ref_count_ref(net_iov_to_netmem(niov));
	left = atomic_long_read(count) - 1;
	if (left > 0) {
		/* The device still uses it: its last put hands it off */
		atomic_long_set(count, left);
		niov->mp_hand_off = true;
		return;
	}

	/* Free: back to userspace with this descriptor, still charged to its
	 * page pool with the count of a free netmem
	 */
	WARN_ON_ONCE(left < 0);
	atomic_long_set(count, 1);
	niov->mp_hand_off = false;
	WRITE_ONCE(niov->mp_state, XSK_SINK_USER);
	ring = (struct xdp_rxtx_ring *)xs->rx->ring;
	ring->desc[(xs->rx->cached_prod - 1) & xs->rx->ring_mask].options |=
		XDP_RX_CHUNK_FREE;
}

static netmem_ref xsk_sink_alloc_netmems(struct page_pool *pp, gfp_t gfp)
{
	struct xsk_sink *sink = pp->mp_priv;
	/* The NAPI owns the need_wakeup flags */
	bool napi = in_softirq() && !in_hardirq();
	struct xsk_queue *fq;
	struct xdp_sock *xs;
	dma_addr_t dma;
	u32 i, n;

	/* The page pool refills only an empty cache */
	if (WARN_ON_ONCE(pp->alloc.count))
		return 0;

	/* Userspace may wait for chunks the device let go of after the last
	 * flush, and with no packets coming, no flush gives them back. A
	 * refill with BH disabled is the queue NAPI, as process context
	 * refills with BH enabled. Only one NAPI runs a queue at a time, as
	 * a queue restart stops the old queue before the new one runs, so
	 * the refill may produce to the RX ring.
	 */
	/* Pairs with xsk_sink_add_sock() */
	xs = smp_load_acquire(&sink->xs);
	if (xs && napi && !llist_empty(&sink->handed)) {
		xsk_sink_give_back(sink, xs);
		xskq_prod_submit(xs->rx);
		xs->sk.sk_data_ready(&xs->sk);
	}

	spin_lock_bh(&sink->lock);
	n = xsk_sink_take_fill(sink, pp);
	n = xsk_sink_take_released(sink, pp, n);
	fq = sink->fq;
	/* Ask for kicks while FILL runs dry, as zero copy drivers do, and
	 * while chunks wait for room in the RX ring. A kick schedules the
	 * NAPI, which a busy poller would rather run itself, but once a
	 * slow socket has drained its ring, only a NAPI run posts the
	 * chunks that waited. Process context refills new queues only and
	 * leaves the flags to their NAPI and to bind.
	 */
	if (napi && fq && sink->pool->uses_need_wakeup) {
		if (n && llist_empty(&sink->handed))
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

/* The last put of a chunk with a buffer in userspace: it goes back to
 * userspace, still charged to @pp. The next flush or refill of the queue
 * NAPI posts its empty descriptor.
 */
static bool xsk_sink_hand_off(struct page_pool *pp, netmem_ref netmem)
{
	struct net_iov *niov = netmem_to_net_iov(netmem);
	struct xsk_sink *sink = pp->mp_priv;
	struct llist_node *node;

	niov->mp_hand_off = false;
	node = &sink->nodes[net_iov_idx(niov)];
	llist_add(node, &sink->handed);
	return true;
}

static int xsk_sink_nl_fill(void *mp_priv, struct sk_buff *rsp,
			    struct netdev_rx_queue *rxq)
{
	return 0;
}

/* Page pools hold the chunks that went back to userspace until it lends
 * them again. Put them back, so that the page pools can finish their
 * destruction, and handed-off chunks that never made it to userspace too.
 * The queue stopped, so no NAPI takes from the handed list any longer.
 * Device unregister scrubs twice, when it uninstalls the provider and
 * when the socket unbinds.
 */
static void xsk_sink_scrub(struct xsk_sink *sink)
{
	struct llist_node *node;
	struct net_iov *niov;
	netmem_ref netmem;
	u32 idx;

	for (node = llist_del_all(&sink->handed); node; node = node->next)
		sink->area.niovs[node - sink->nodes].mp_state = XSK_SINK_USER;

	for (idx = 0; idx < sink->area.num_niovs; idx++) {
		niov = &sink->area.niovs[idx];
		netmem = net_iov_to_netmem(niov);
		if (niov->mp_state == XSK_SINK_LENT || !netmem_get_pp(netmem))
			continue;

		niov->mp_hand_off = false;
		/* A later scrub must skip it: its page pool may not have
		 * released it yet
		 */
		niov->mp_state = XSK_SINK_LENT;
		page_pool_put_unrefed_netmem(netmem_get_pp(netmem), netmem,
					     -1, false);
	}
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
	xsk_sink_scrub(sink);
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
	.hand_off	= xsk_sink_hand_off,
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
	init_llist_head(&sink->handed);
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

/* The first socket with an RX ring receives; others sharing the pool,
 * such as TX-only sockets, do not.
 *
 * Context: RTNL and the instance lock of the device
 */
void xsk_sink_add_sock(struct xsk_sink *sink, struct xdp_sock *xs)
{
	if (xs->rx && !sink->xs) {
		/* The NAPI acquires the socket once it is set up */
		smp_store_release(&sink->xs, xs);
	}
}

/* The receiving socket, if any.
 * Context: RTNL and the instance lock of the device
 */
struct xdp_sock *xsk_sink_sock(struct xsk_sink *sink)
{
	return sink->xs;
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

/* The net_iov of @netmem if it is one of ours; tells by its address */
static __always_inline struct net_iov *xsk_sink_niov(struct xsk_sink *sink,
						     netmem_ref netmem)
{
	struct net_iov *niov;

	if (!netmem_is_net_iov(netmem))
		return NULL;
	niov = netmem_to_net_iov(netmem);
	if ((unsigned long)(niov - sink->area.niovs) >= sink->area.num_niovs)
		return NULL;
	return niov;
}

/* Puts the buffers of a packet on the RX ring, one descriptor each, with
 * the metadata of the program in front of the first. Returns 0 if the socket
 * took them.
 */
static int xsk_sink_rcv(struct xsk_sink *sink, struct xdp_sock *xs,
			struct xdp_buff *xdp)
{
	struct net_iov *niov, *hniov = NULL, *empty = NULL;
	u32 i, n, len, hlen, nr_frags = 0, headroom;
	struct skb_shared_info *sinfo;
	const skb_frag_t *frag;
	u64 addr, off;
	u32 metalen = 0;
	netmem_ref netmem;

	sinfo = xdp_get_shared_info_from_buff(xdp);
	if (xdp_buff_has_frags(xdp))
		nr_frags = sinfo->nr_frags;

	/* The head sits in the UMEM mapping */
	off = (u8 *)xdp->data - (u8 *)sink->area.xdp_vaddr;
	if (off >= (u64)sink->area.num_niovs << PAGE_SHIFT)
		goto drop;
	niov = &sink->area.niovs[off >> PAGE_SHIFT];
	hlen = xdp->data_end - xdp->data;
	if (!hlen) {
		/* An empty linear part, as mlx5 builds for nonlinear RQs */
		empty = niov;
		headroom = 0;
	} else {
		if ((off & ~PAGE_MASK) + hlen > PAGE_SIZE)
			goto drop;
		hniov = niov;
		headroom = min_t(u64, off & ~PAGE_MASK,
				 xdp->data - xdp->data_hard_start);
	}

	for (i = 0; i < nr_frags; i++)
		if (!xsk_sink_niov(sink, skb_frag_netmem(&sinfo->frags[i])))
			goto drop;

	n = nr_frags + !!hlen;
	if (!n || (n > 1 && !xs->sg))
		goto drop;

	/* The program put metadata in front of the head, which must be the
	 * first buffer, within its chunk
	 */
	if (!xdp_data_meta_unsupported(xdp))
		metalen = xdp->data - xdp->data_meta;
	if (metalen > headroom)
		goto drop;

	if (xskq_prod_nb_free(xs->rx, n) < n) {
		xs->rx_queue_full++;
		return -ENOBUFS;
	}

	for (i = 0; i < n; i++) {
		if (hlen && !i) {
			niov = hniov;
			addr = off;
			len = hlen;
		} else {
			frag = &sinfo->frags[i - !!hlen];
			niov = netmem_to_net_iov(skb_frag_netmem(frag));
			addr = ((u64)(niov - sink->area.niovs) << PAGE_SHIFT) +
			       skb_frag_off(frag);
			len = skb_frag_size(frag);
		}
		__xskq_prod_reserve_desc(xs->rx, addr, len,
					 i + 1 < n ? XDP_PKT_CONTD : 0);
		xsk_sink_drop_buf(sink, xs, niov);
	}

	if (empty) {
		netmem = net_iov_to_netmem(empty);
		page_pool_put_netmem(netmem_get_pp(netmem), netmem, -1, false);
	}
	return 0;

drop:
	xs->rx_dropped++;
	return -EINVAL;
}

/**
 * xsk_sink_flush - give back chunks at the end of a NAPI poll
 * @sink: provider of a socket that received in this poll
 *
 * Context: the queue NAPI, from xdp_do_flush(), before the RX ring is
 * submitted.
 */
void xsk_sink_flush(struct xsk_sink *sink)
{
	/* Pairs with xsk_sink_add_sock() */
	struct xdp_sock *xs = smp_load_acquire(&sink->xs);

	if (!xs)
		return;

	xsk_sink_give_back(sink, xs);
}

/**
 * xsk_sink_redirect - deliver a sink buffer to a socket of its queue
 * @xdp: sink buffer, see xdp_buff_is_sink()
 * @map_type: map of the redirect
 * @fwd: target of the redirect
 *
 * The target must be the receiving socket of the queue in an XSKMAP. Other
 * maps cannot take sink buffers.
 *
 * Context: the queue NAPI, from xdp_do_redirect().
 * Return: 0 if the socket took the buffer.
 */
int xsk_sink_redirect(struct xdp_buff *xdp, enum bpf_map_type map_type,
		      void *fwd)
{
	struct net_iov_area *area = xdp->rxq->sink_area;
	struct xsk_sink *sink;
	struct xdp_sock *xs;
	int err;

	/* Set for every queue with a sink provider, see
	 * PP_FLAG_ALLOW_XDP_ONLY_NETMEM
	 */
	if (unlikely(!area))
		return -EINVAL;
	sink = container_of(area, struct xsk_sink, area);

	if (map_type != BPF_MAP_TYPE_XSKMAP)
		return -EOPNOTSUPP;

	/* Pairs with xsk_sink_add_sock() */
	xs = smp_load_acquire(&sink->xs);
	if (unlikely(!xs || xs != fwd))
		return -EINVAL;

	err = xsk_sink_rcv(sink, xs, xdp);
	if (err)
		return err;

	if (!xs->flush_node.prev)
		list_add(&xs->flush_node, bpf_net_ctx_get_xskmap_flush_list());
	return 0;
}
