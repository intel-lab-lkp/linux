// SPDX-License-Identifier: GPL-2.0

#include <linux/netdevice.h>
#include <linux/sizes.h>
#include <net/netdev_lock.h>
#include <net/netdev_queues.h>
#include <net/netdev_rx_queue.h>
#include <net/page_pool/helpers.h>
#include <net/page_pool/memory_provider.h>
#include <net/xsk_buff_pool.h>
#include <net/xdp_sock.h>
#include <net/xdp_sock_drv.h>

#include "xsk_queue.h"
#include "xdp_umem.h"
#include "xsk.h"

#define ETH_PAD_LEN (ETH_HLEN + 2 * VLAN_HLEN  + ETH_FCS_LEN)
#define XSK_PAGE_POOL_DMA_ATTR	(DMA_ATTR_WEAK_ORDERING | \
				 DMA_ATTR_SKIP_CPU_SYNC)
#define XSK_POOL_RELEASE_RETRY_MAX	(60 * HZ)

static bool xp_pp_teardown(struct xsk_buff_pool *pool);
static void xp_pp_retry_release(struct xsk_buff_pool *pool);
static void xp_destroy_unbound_deferred(struct work_struct *work);

static void __xp_destroy(struct xsk_buff_pool *pool)
{
	kvfree(pool->tx_descs);
	kvfree(pool->heads);
	kvfree(pool);
}

void xp_add_xsk(struct xsk_buff_pool *pool, struct xdp_sock *xs)
{
	if (!xs->tx)
		return;

	spin_lock(&pool->xsk_tx_list_lock);
	list_add_rcu(&xs->tx_list, &pool->xsk_tx_list);
	spin_unlock(&pool->xsk_tx_list_lock);
}

void xp_del_xsk(struct xsk_buff_pool *pool, struct xdp_sock *xs)
{
	if (!xs->tx)
		return;

	spin_lock(&pool->xsk_tx_list_lock);
	list_del_rcu(&xs->tx_list);
	spin_unlock(&pool->xsk_tx_list_lock);
}

void xp_destroy(struct xsk_buff_pool *pool)
{
	if (!pool)
		return;

	/* A failed queue replacement can leave a page_pool waiting for an
	 * in-flight buffer.  Keep the UMEM and pool alive until its provider
	 * destroy callback has run.
	 */
	if (pool->pp) {
		rcu_assign_pointer(pool->pp->fq, NULL);
		WRITE_ONCE(pool->fq, NULL);
		WRITE_ONCE(pool->cq, NULL);
		synchronize_net();
		xdp_get_umem(pool->umem);
		INIT_DELAYED_WORK(&pool->work, xp_destroy_unbound_deferred);
		schedule_delayed_work(&pool->work, 0);
		return;
	}

	__xp_destroy(pool);
}

int xp_alloc_tx_descs(struct xsk_buff_pool *pool, struct xdp_sock *xs,
		      u32 max_segs)
{
	u32 nentries = max(xs->tx->nentries, max_segs);

	pool->tx_descs = kvzalloc_objs(*pool->tx_descs, nentries);
	if (!pool->tx_descs)
		return -ENOMEM;

	pool->tx_descs_nentries = nentries;
	return 0;
}

struct xsk_buff_pool *xp_create_and_assign_umem(struct xdp_sock *xs,
						struct xdp_umem *umem,
						u32 max_segs)
{
	bool unaligned = umem->flags & XDP_UMEM_UNALIGNED_CHUNK_FLAG;
	struct xsk_buff_pool *pool;
	struct xdp_buff_xsk *xskb;
	u32 i, entries;

	entries = unaligned ? umem->chunks : 0;
	pool = kvzalloc_flex(*pool, free_heads, entries);
	if (!pool)
		goto out;

	pool->heads = kvzalloc_objs(*pool->heads, umem->chunks);
	if (!pool->heads)
		goto out;

	if (xs->tx)
		if (xp_alloc_tx_descs(pool, xs, max_segs))
			goto out;

	pool->chunk_mask = ~((u64)umem->chunk_size - 1);
	pool->addrs_cnt = umem->size;
	pool->heads_cnt = umem->chunks;
	pool->free_heads_cnt = umem->chunks;
	pool->headroom = umem->headroom;
	pool->chunk_size = umem->chunk_size;
	pool->chunk_shift = ffs(umem->chunk_size) - 1;
	pool->unaligned = unaligned;
	pool->frame_len = umem->chunk_size - umem->headroom -
		XDP_PACKET_HEADROOM;
	pool->umem = umem;
	pool->addrs = umem->addrs;
	pool->tx_metadata_len = umem->tx_metadata_len;
	pool->tx_sw_csum = umem->flags & XDP_UMEM_TX_SW_CSUM;
	spin_lock_init(&pool->rx_lock);
	INIT_LIST_HEAD(&pool->free_list);
	INIT_LIST_HEAD(&pool->xskb_list);
	INIT_LIST_HEAD(&pool->xsk_tx_list);
	spin_lock_init(&pool->xsk_tx_list_lock);
	spin_lock_init(&pool->cq_prod_lock);
	spin_lock_init(&xs->cq_tmp->cq_cached_prod_lock);
	refcount_set(&pool->users, 1);

	pool->fq = xs->fq_tmp;
	pool->cq = xs->cq_tmp;

	for (i = 0; i < pool->free_heads_cnt; i++) {
		xskb = &pool->heads[i];
		xskb->pool = pool;
		xskb->xdp.frame_sz = umem->chunk_size - umem->headroom;
		INIT_LIST_HEAD(&xskb->list_node);
		if (pool->unaligned)
			pool->free_heads[i] = xskb;
		else
			xp_init_xskb_addr(xskb, pool, (u64)i * pool->chunk_size);
	}

	return pool;

out:
	xp_destroy(pool);
	return NULL;
}

void xp_set_rxq_info(struct xsk_buff_pool *pool, struct xdp_rxq_info *rxq)
{
	u32 i;

	for (i = 0; i < pool->heads_cnt; i++)
		pool->heads[i].xdp.rxq = rxq;
}
EXPORT_SYMBOL(xp_set_rxq_info);

void xp_fill_cb(struct xsk_buff_pool *pool, struct xsk_cb_desc *desc)
{
	u32 i;

	for (i = 0; i < pool->heads_cnt; i++) {
		struct xdp_buff_xsk *xskb = &pool->heads[i];

		memcpy(xskb->cb + desc->off, desc->src, desc->bytes);
	}
}
EXPORT_SYMBOL(xp_fill_cb);

static void xp_disable_drv_zc(struct xsk_buff_pool *pool)
{
	struct netdev_bpf bpf;
	int err;

	ASSERT_RTNL();

	if (pool->umem->zc) {
		bpf.command = XDP_SETUP_XSK_POOL;
		bpf.xsk.pool = NULL;
		bpf.xsk.queue_id = pool->queue_id;

		err = pool->netdev->netdev_ops->ndo_bpf(pool->netdev, &bpf);

		if (err)
			WARN(1, "Failed to disable zero-copy!\n");
	}
}

int xp_assign_dev(struct xsk_buff_pool *pool,
		  struct net_device *netdev, u16 queue_id, u16 flags)
{
	u32 needed = netdev->mtu + ETH_PAD_LEN;
	u32 segs = netdev->xdp_zc_max_segs;
	bool old_zc = pool->umem->zc;
	bool mbuf = flags & XDP_USE_SG;
	bool force_zc, force_copy;
	struct netdev_bpf bpf;
	u32 frame_size;
	int err = 0;

	ASSERT_RTNL();

	force_zc = flags & XDP_ZEROCOPY;
	force_copy = flags & XDP_COPY;

	if (force_zc && force_copy)
		return -EINVAL;

	if (pool->tx_sw_csum && (netdev->priv_flags & IFF_TX_SKB_NO_LINEAR))
		return -EOPNOTSUPP;

	if (xsk_get_pool_from_qid(netdev, queue_id))
		return -EBUSY;

	pool->netdev = netdev;
	pool->queue_id = queue_id;
	err = xsk_reg_pool_at_qid(netdev, pool, queue_id);
	if (err)
		return err;

	if (mbuf)
		pool->umem->flags |= XDP_UMEM_SG_FLAG;

	if (flags & XDP_USE_NEED_WAKEUP)
		pool->uses_need_wakeup = true;
	/* Tx needs to be explicitly woken up the first time.  Also
	 * for supporting drivers that do not implement this
	 * feature. They will always have to call sendto() or poll().
	 */
	pool->cached_need_wakeup = XDP_WAKEUP_TX;

	dev_hold(netdev);

	if (force_copy)
		/* For copy-mode, we are done. */
		return 0;

	if ((netdev->xdp_features & NETDEV_XDP_ACT_XSK) != NETDEV_XDP_ACT_XSK) {
		err = -EOPNOTSUPP;
		goto err_unreg_pool;
	}

	if (mbuf) {
		if (segs == 1) {
			err = -EOPNOTSUPP;
			goto err_unreg_pool;
		}
	} else {
		segs = 1;
	}

	/* open-code xsk_pool_get_rx_frame_size() as pool->dev is not
	 * set yet at this point; we are before getting down to driver
	 */
	frame_size = __xsk_pool_get_rx_frame_size(pool) -
		     xsk_pool_get_tailroom(mbuf);
	frame_size = ALIGN_DOWN(frame_size, XSK_RX_FRAME_SIZE_ALIGN);

	if (needed > frame_size * segs) {
		err = -EINVAL;
		goto err_unreg_pool;
	}

	if (netif_mp_lacks_cap(netdev, MP_CAP_READABLE)) {
		err = -EBUSY;
		goto err_unreg_pool;
	}

	bpf.command = XDP_SETUP_XSK_POOL;
	bpf.xsk.pool = pool;
	bpf.xsk.queue_id = queue_id;

	netdev_assert_locked_ops_compat(netdev);
	err = netdev->netdev_ops->ndo_bpf(netdev, &bpf);
	if (err)
		goto err_unreg_pool;

	/* Record the successful driver install before validating its result so
	 * the error path can issue the matching XDP_SETUP_XSK_POOL teardown.
	 */
	pool->umem->zc = true;
	if (!pool->dma_pages) {
		WARN(1, "Driver did not DMA map zero-copy buffers");
		err = -EINVAL;
		goto err_unreg_xsk;
	}
	pool->xdp_zc_max_segs = netdev->xdp_zc_max_segs;
	return 0;

err_unreg_xsk:
	xp_disable_drv_zc(pool);
	if (pool->pp)
		pool->pp->detached = true;
	pool->umem->zc = old_zc;
err_unreg_pool:
	/* A provider whose page_pool destruction was deferred still owns the
	 * DMA mapping.  Do not turn that failed zero-copy setup into copy mode.
	 */
	if (!force_zc && !pool->pp)
		err = 0; /* fallback to copy mode */
	if (err) {
		xsk_clear_pool_at_qid(netdev, queue_id);
		if (!pool->pp) {
			pool->netdev = NULL;
			dev_put(netdev);
		}
	}
	return err;
}

int xp_assign_dev_shared(struct xsk_buff_pool *pool, struct xdp_sock *umem_xs,
			 struct net_device *dev, u16 queue_id)
{
	u16 flags;
	struct xdp_umem *umem = umem_xs->umem;

	flags = umem->zc ? XDP_ZEROCOPY : XDP_COPY;

	if (umem->flags & XDP_UMEM_SG_FLAG)
		flags |= XDP_USE_SG;

	if (umem_xs->pool->uses_need_wakeup)
		flags |= XDP_USE_NEED_WAKEUP;

	return xp_assign_dev(pool, dev, queue_id, flags);
}

static void xp_pp_force_dma_unmap(struct xsk_buff_pool *pool)
{
	if (!pool->dma_map)
		return;

	pool->dma_map->netdev = NULL;
	xsk_pool_dma_unmap(pool, XSK_PAGE_POOL_DMA_ATTR);
}

void xp_clear_dev(struct xsk_buff_pool *pool, enum xsk_pool_clear_mode mode)
{
	struct net_device *netdev = pool->netdev;
	struct xsk_pp *provider = pool->pp;

	if (!netdev)
		return;
	/* Keep this reference after a normal detach. It keeps the netdev
	 * identity stable for shared DMA-map lookup and lets unregister
	 * invalidate that mapping while the old page_pool drains.
	 */
	if (provider && provider->detached) {
		if (mode != XSK_POOL_CLEAR_FORCE)
			return;
		xp_pp_force_dma_unmap(pool);
		pool->netdev = NULL;
		dev_put(netdev);
		return;
	}

	netdev_lock_ops(netdev);
	xp_disable_drv_zc(pool);
	xsk_clear_pool_at_qid(pool->netdev, pool->queue_id);
	if (provider) {
		provider->detached = true;
		if (mode == XSK_POOL_CLEAR_FORCE) {
			xp_pp_force_dma_unmap(pool);
			pool->netdev = NULL;
		}
	} else {
		pool->netdev = NULL;
	}
	netdev_unlock_ops(netdev);

	if (!provider || mode == XSK_POOL_CLEAR_FORCE)
		dev_put(netdev);
}

static void xp_release_deferred(struct work_struct *work)
{
	struct net_device *netdev = NULL;
	struct xsk_buff_pool *pool;
	bool teardown_done;

	pool = container_of(to_delayed_work(work), struct xsk_buff_pool, work);

	rtnl_lock();
	xp_clear_dev(pool, XSK_POOL_CLEAR_NORMAL);
	teardown_done = xp_pp_teardown(pool);
	if (teardown_done && pool->netdev) {
		netdev = pool->netdev;
		pool->netdev = NULL;
	}
	rtnl_unlock();
	if (!teardown_done) {
		xp_pp_retry_release(pool);
		return;
	}
	if (netdev)
		dev_put(netdev);

	if (pool->fq) {
		xskq_destroy(pool->fq);
		pool->fq = NULL;
	}

	if (pool->cq) {
		xskq_destroy(pool->cq);
		pool->cq = NULL;
	}

	xdp_put_umem(pool->umem, false);
	__xp_destroy(pool);
}

void xp_get_pool(struct xsk_buff_pool *pool)
{
	refcount_inc(&pool->users);
}

bool xp_put_pool(struct xsk_buff_pool *pool)
{
	if (!pool)
		return false;

	if (refcount_dec_and_test(&pool->users)) {
		INIT_DELAYED_WORK(&pool->work, xp_release_deferred);
		schedule_delayed_work(&pool->work, 0);
		return true;
	}

	return false;
}

static struct xsk_dma_map *xp_find_dma_map(struct xsk_buff_pool *pool)
{
	struct xsk_dma_map *dma_map;

	list_for_each_entry(dma_map, &pool->umem->xsk_dma_list, list) {
		if (dma_map->netdev == pool->netdev)
			return dma_map;
	}

	return NULL;
}

static struct xsk_dma_map *xp_create_dma_map(struct device *dev, struct net_device *netdev,
					     u32 nr_pages, struct xdp_umem *umem)
{
	struct xsk_dma_map *dma_map;

	dma_map = kzalloc_obj(*dma_map);
	if (!dma_map)
		return NULL;

	dma_map->dma_pages = kvzalloc_objs(*dma_map->dma_pages, nr_pages);
	if (!dma_map->dma_pages) {
		kfree(dma_map);
		return NULL;
	}

	dma_map->netdev = netdev;
	dma_map->dev = get_device(dev);
	dma_map->dma_pages_cnt = nr_pages;
	refcount_set(&dma_map->users, 1);
	list_add(&dma_map->list, &umem->xsk_dma_list);
	return dma_map;
}

static void xp_destroy_dma_map(struct xsk_dma_map *dma_map)
{
	list_del(&dma_map->list);
	kvfree(dma_map->dma_pages);
	put_device(dma_map->dev);
	kfree(dma_map);
}

static void __xp_dma_unmap(struct xsk_dma_map *dma_map, unsigned long attrs)
{
	dma_addr_t *dma;
	u32 i;

	for (i = 0; i < dma_map->dma_pages_cnt; i++) {
		dma = &dma_map->dma_pages[i];
		if (*dma) {
			*dma &= ~XSK_NEXT_PG_CONTIG_MASK;
			dma_unmap_page_attrs(dma_map->dev, *dma, PAGE_SIZE,
					     DMA_BIDIRECTIONAL, attrs);
			*dma = 0;
		}
	}

	xp_destroy_dma_map(dma_map);
}

void xp_dma_unmap(struct xsk_buff_pool *pool, unsigned long attrs)
{
	struct xsk_dma_map *dma_map = pool->dma_map;

	if (!pool->dma_pages)
		return;

	if (!dma_map) {
		WARN(1, "Could not find dma_map for device");
		return;
	}

	if (refcount_dec_and_test(&dma_map->users))
		__xp_dma_unmap(dma_map, attrs);

	kvfree(pool->dma_pages);
	pool->dma_pages = NULL;
	pool->dma_pages_cnt = 0;
	pool->dma_map = NULL;
	pool->dev = NULL;
}
EXPORT_SYMBOL(xp_dma_unmap);

static void xp_check_dma_contiguity(struct xsk_dma_map *dma_map)
{
	u32 i;

	for (i = 0; i < dma_map->dma_pages_cnt - 1; i++) {
		if (dma_map->dma_pages[i] + PAGE_SIZE == dma_map->dma_pages[i + 1])
			dma_map->dma_pages[i] |= XSK_NEXT_PG_CONTIG_MASK;
		else
			dma_map->dma_pages[i] &= ~XSK_NEXT_PG_CONTIG_MASK;
	}
}

static int xp_init_dma_info(struct xsk_buff_pool *pool, struct xsk_dma_map *dma_map)
{
	if (!pool->unaligned) {
		u32 i;

		for (i = 0; i < pool->heads_cnt; i++) {
			struct xdp_buff_xsk *xskb = &pool->heads[i];
			u64 orig_addr;

			orig_addr = xskb->xdp.data_hard_start - pool->addrs - pool->headroom;
			xp_init_xskb_dma(xskb, pool, dma_map->dma_pages, orig_addr);
		}
	}

	pool->dma_pages = kvzalloc_objs(*pool->dma_pages,
					dma_map->dma_pages_cnt);
	if (!pool->dma_pages)
		return -ENOMEM;

	pool->dev = dma_map->dev;
	pool->dma_map = dma_map;
	pool->dma_pages_cnt = dma_map->dma_pages_cnt;
	memcpy(pool->dma_pages, dma_map->dma_pages,
	       pool->dma_pages_cnt * sizeof(*pool->dma_pages));

	return 0;
}

int xp_dma_map(struct xsk_buff_pool *pool, struct device *dev,
	       unsigned long attrs, struct page **pages, u32 nr_pages)
{
	struct xsk_dma_map *dma_map;
	dma_addr_t dma;
	int err;
	u32 i;

	dma_map = xp_find_dma_map(pool);
	if (dma_map) {
		err = xp_init_dma_info(pool, dma_map);
		if (err)
			return err;

		refcount_inc(&dma_map->users);
		return 0;
	}

	dma_map = xp_create_dma_map(dev, pool->netdev, nr_pages, pool->umem);
	if (!dma_map)
		return -ENOMEM;

	for (i = 0; i < dma_map->dma_pages_cnt; i++) {
		dma = dma_map_page_attrs(dev, pages[i], 0, PAGE_SIZE,
					 DMA_BIDIRECTIONAL, attrs);
		if (dma_mapping_error(dev, dma)) {
			__xp_dma_unmap(dma_map, attrs);
			return -ENOMEM;
		}
		dma_map->dma_pages[i] = dma;
	}

	if (pool->unaligned)
		xp_check_dma_contiguity(dma_map);

	err = xp_init_dma_info(pool, dma_map);
	if (err) {
		__xp_dma_unmap(dma_map, attrs);
		return err;
	}

	return 0;
}
EXPORT_SYMBOL(xp_dma_map);

static void xp_pp_free(struct xsk_buff_pool *pool)
{
	struct xsk_pp *provider = pool->pp;
	u32 idx;

	if (!provider)
		return;
	if (WARN_ON_ONCE(READ_ONCE(provider->nr_pools)))
		return;

	/* A failed queue install may have consumed FILL entries before the
	 * replacement queue failed to start. Preserve those frames for the
	 * direct or copy-mode allocator selected by bind fallback.
	 */
	while (provider->reuse_cnt) {
		idx = provider->reuse[--provider->reuse_cnt];
		xp_free(&pool->heads[idx]);
	}

	spin_lock_bh(&pool->rx_lock);
	WRITE_ONCE(pool->pp, NULL);
	spin_unlock_bh(&pool->rx_lock);
	kvfree(provider->info.area.niovs);
	kvfree(provider);
}

static bool xp_pp_teardown(struct xsk_buff_pool *pool)
{
	struct xsk_pp *provider = pool->pp;
	u32 busy;

	if (!provider)
		return true;
	if (WARN_ON_ONCE(!provider->detached))
		return false;
	/* xp_pp_destroy() touches the provider until it drops the lock. */
	spin_lock_bh(&provider->lock);
	busy = provider->nr_pools;
	spin_unlock_bh(&provider->lock);
	if (busy)
		return false;

	xsk_pool_dma_unmap(pool, XSK_PAGE_POOL_DMA_ATTR);
	xp_pp_free(pool);

	return true;
}

static void xp_pp_retry_release(struct xsk_buff_pool *pool)
{
	struct xsk_pp *provider = pool->pp;
	unsigned long delay = HZ;
	u8 retries;

	if (provider) {
		retries = provider->release_retries;
		if (retries < 6)
			provider->release_retries++;
		delay = min_t(unsigned long, HZ << retries,
			      XSK_POOL_RELEASE_RETRY_MAX);
	}
	schedule_delayed_work(&pool->work, delay);
}

static void xp_destroy_unbound_deferred(struct work_struct *work)
{
	struct xsk_buff_pool *pool;
	struct net_device *netdev;

	pool = container_of(to_delayed_work(work), struct xsk_buff_pool, work);

	rtnl_lock();
	if (!xp_pp_teardown(pool)) {
		rtnl_unlock();
		xp_pp_retry_release(pool);
		return;
	}
	netdev = pool->netdev;
	pool->netdev = NULL;
	rtnl_unlock();

	if (netdev)
		dev_put(netdev);
	xdp_put_umem(pool->umem, false);
	__xp_destroy(pool);
}

static struct xsk_pp *xp_pp_create(struct xsk_buff_pool *pool)
{
	struct xsk_pp *provider;
	struct net_iov *niov;
	dma_addr_t dma;
	u64 addr;
	u32 i;

	/* The provider arrays scale with the UMEM; charge them like it. */
	provider = kvzalloc_flex(*provider, reuse, pool->heads_cnt,
				 GFP_KERNEL_ACCOUNT);
	if (!provider)
		return ERR_PTR(-ENOMEM);

	provider->info.area.niovs =
		kvzalloc_objs(*provider->info.area.niovs, pool->heads_cnt,
			      GFP_KERNEL_ACCOUNT);
	if (!provider->info.area.niovs) {
		kvfree(provider);
		return ERR_PTR(-ENOMEM);
	}
	provider->info.pool = pool;
	RCU_INIT_POINTER(provider->fq, pool->fq);
	spin_lock_init(&provider->lock);
	provider->info.area.num_niovs = pool->heads_cnt;
	provider->info.area.vaddr = pool->addrs;
	provider->info.area.niov_shift = pool->chunk_shift;
	provider->chunk_mask = pool->chunk_mask;
	provider->addrs_cnt = pool->addrs_cnt;
	provider->info.chunk_shift = pool->chunk_shift;
	provider->dma_need_sync = dma_dev_need_sync(pool->dev);

	for (i = 0; i < provider->info.area.num_niovs; i++) {
		niov = &provider->info.area.niovs[i];
		addr = (u64)i << provider->info.chunk_shift;
		dma = (pool->dma_pages[addr >> PAGE_SHIFT] &
		       ~XSK_NEXT_PG_CONTIG_MASK) + (addr & ~PAGE_MASK);

		net_iov_init(niov, &provider->info.area, NET_IOV_XSK);
		if (net_mp_niov_set_dma_addr(niov, dma))
			goto err_free_niovs;
	}

	/* Exclude generic receive. It can run on another CPU after RPS and
	 * would produce into the socket RX ring, which provider delivery
	 * fills without a lock from the queue's poll context.
	 */
	spin_lock_bh(&pool->rx_lock);
	WRITE_ONCE(pool->pp, provider);
	spin_unlock_bh(&pool->rx_lock);
	return provider;

err_free_niovs:
	kvfree(provider->info.area.niovs);
	kvfree(provider);
	return ERR_PTR(-ERANGE);
}

static int xp_pp_init(struct page_pool *pp)
{
	struct xsk_buff_pool *pool = pp->mp_priv;
	struct xsk_pp *provider = pool->pp;
	int err = 0;

	if (!provider || !pool->dma_pages)
		return -EINVAL;
	if (pp->p.order)
		return -E2BIG;
	if (pp->p.dev != pool->dev ||
	    pp->p.dma_dir != DMA_BIDIRECTIONAL)
		return -EINVAL;
	/* Each object is one UMEM chunk. Syncs and device writes described by
	 * the page_pool must stay inside it.
	 */
	if (pp->p.offset + pp->p.max_len > pool->chunk_size)
		return -EINVAL;

	spin_lock_bh(&provider->lock);
	if (provider->detached)
		err = -ENODEV;
	else
		provider->nr_pools++;
	spin_unlock_bh(&provider->lock);

	return err;
}

static void xp_pp_destroy(struct page_pool *pp)
{
	struct xsk_buff_pool *pool = pp->mp_priv;
	struct xsk_pp *provider = pool->pp;

	/* Every object the page_pool held was released through
	 * xp_pp_release_netmem() or handed to userspace, both of which clear
	 * its page_pool association.
	 */
	spin_lock_bh(&provider->lock);
	if (!WARN_ON_ONCE(!provider->nr_pools))
		provider->nr_pools--;
	spin_unlock_bh(&provider->lock);
}

static netmem_ref xp_pp_prepare_netmem(struct page_pool *pp,
				       struct xsk_pp *provider,
				       struct net_iov *niov)
{
	netmem_ref netmem = net_iov_to_netmem(niov);
	dma_addr_t dma = page_pool_get_dma_addr_netmem(netmem);

	if (provider->dma_need_sync)
		dma_sync_single_range_for_device(pp->p.dev, dma,
						 pp->p.offset, pp->p.max_len,
						 pp->p.dma_dir);

	return netmem;
}

/* A set pp marks an object a page pool owns. Repeated FILL addresses must
 * not hand it out twice.
 */
static bool xp_pp_claim(struct page_pool *pp, struct net_iov *niov)
{
	if (unlikely(niov->desc.pp))
		return false;

	niov->desc.pp = pp;
	return true;
}

static unsigned int
xp_pp_alloc_reused(struct page_pool *pp, struct xsk_pp *provider,
		   netmem_ref *netmems, unsigned int max)
{
	unsigned int entries;
	unsigned int allocated = 0;
	struct net_iov *niov;
	unsigned int i;
	u32 idx;

	entries = min(max, provider->reuse_cnt);
	for (i = 0; i < entries; i++) {
		idx = provider->reuse[--provider->reuse_cnt];
		niov = &provider->info.area.niovs[idx];
		if (xp_pp_claim(pp, niov))
			netmems[allocated++] = net_iov_to_netmem(niov);
	}

	/* page_pool normally synced these buffers before ->release_netmem(),
	 * but page_pool teardown disables DMA sync before emptying its caches.
	 * Conservatively prepare both reuse and FQ buffers below.
	 */
	return allocated;
}

static unsigned int xp_pp_alloc_fq(struct page_pool *pp,
				   struct xsk_pp *provider,
				   netmem_ref *netmems, unsigned int max)
{
	struct xsk_buff_pool *pool = provider->info.pool;
	struct xsk_queue *fq;
	struct net_iov *niov;
	u64 addr;
	unsigned int allocated = 0;
	u32 cached_cons, entries;
	u32 cons, idx;
	u32 i;

	rcu_read_lock_bh();
	fq = rcu_dereference_bh(provider->fq);
	if (!fq)
		goto out;

	entries = xskq_cons_nb_entries(fq, max);
	if (!entries && pool->uses_need_wakeup) {
		/* Publish NEED_WAKEUP before checking again. */
		xsk_set_rx_need_wakeup(pool);
		/* Pair the publication with the producer recheck. */
		smp_mb();
		entries = xskq_cons_nb_entries(fq, max);
	}
	if (!entries) {
		fq->queue_empty_descs++;
		goto out;
	}

	cached_cons = fq->cached_cons;
	for (i = 0; i < entries; i++) {
		cons = cached_cons++;
		__xskq_cons_read_addr_unchecked(fq, cons, &addr);
		addr &= provider->chunk_mask;
		if (unlikely(addr >= provider->addrs_cnt)) {
			fq->invalid_descs++;
			continue;
		}

		idx = addr >> provider->info.chunk_shift;
		niov = &provider->info.area.niovs[idx];
		if (unlikely(!xp_pp_claim(pp, niov))) {
			fq->invalid_descs++;
			continue;
		}
		netmems[allocated++] = net_iov_to_netmem(niov);
	}

	xskq_cons_release_n(fq, entries);
	__xskq_cons_release(fq);
out:
	rcu_read_unlock_bh();
	return allocated;
}

static netmem_ref xp_pp_alloc_netmems(struct page_pool *pp, gfp_t gfp)
{
	struct xsk_buff_pool *pool = pp->mp_priv;
	struct xsk_pp *provider = pool->pp;
	netmem_ref *netmems = pp->alloc.cache;
	unsigned int allocated;
	unsigned int i;

	if (WARN_ON_ONCE(pp->alloc.count))
		return 0;

	/* One lock round trip refills a whole page_pool allocation cache. */
	spin_lock_bh(&provider->lock);
	allocated = xp_pp_alloc_reused(pp, provider, netmems,
				       PP_ALLOC_CACHE_REFILL);
	if (allocated < PP_ALLOC_CACHE_REFILL)
		allocated += xp_pp_alloc_fq(pp, provider, netmems + allocated,
					    PP_ALLOC_CACHE_REFILL - allocated);
	spin_unlock_bh(&provider->lock);
	if (!allocated)
		return 0;

	/* The selected netmems now belong to @pp alone. Finish DMA and
	 * page-pool setup without the lock before the driver sees them.
	 */
	for (i = 0; i < allocated; i++) {
		struct net_iov *niov = netmem_to_net_iov(netmems[i]);

		netmems[i] = xp_pp_prepare_netmem(pp, provider, niov);
	}
	net_mp_netmem_set_page_pool_bulk(pp, netmems, allocated);

	allocated--;
	pp->alloc.count = allocated;
	return netmems[allocated];
}

static bool xp_pp_release_netmem(struct page_pool *pp, netmem_ref netmem)
{
	struct xsk_buff_pool *pool = pp->mp_priv;
	struct xsk_pp *provider = pool->pp;
	struct net_iov *niov = netmem_to_net_iov(netmem);
	u32 idx = net_iov_idx(niov);

	/* page_pool calls this on ptr_ring overflow and from its destroy and
	 * release-retry paths, which can run while another page_pool of this
	 * provider allocates. Clear the association before the object becomes
	 * visible to that page_pool.
	 */
	net_mp_niov_clear_page_pool(niov);
	spin_lock_bh(&provider->lock);
	if (likely(provider->reuse_cnt < provider->info.area.num_niovs))
		provider->reuse[provider->reuse_cnt++] = idx;
	spin_unlock_bh(&provider->lock);

	return false;
}

static bool xp_pp_refill_done(struct page_pool *pp, bool full)
{
	struct xsk_buff_pool *pool = pp->mp_priv;
	struct xsk_pp *provider = pool->pp;
	bool need_wakeup = pool->uses_need_wakeup;
	struct xsk_queue *fq;
	bool pending;

	if (likely(full) &&
	    (!need_wakeup || !(pool->cached_need_wakeup & XDP_WAKEUP_RX)))
		return true;

	spin_lock_bh(&provider->lock);
	fq = rcu_dereference_bh(provider->fq);
	if (!fq) {
		spin_unlock_bh(&provider->lock);
		return true;
	}

	if (full) {
		if (need_wakeup)
			xsk_clear_rx_need_wakeup(pool);
		spin_unlock_bh(&provider->lock);
		return true;
	}

	pending = xskq_cons_nb_entries(fq, 1);
	if (!pending && need_wakeup) {
		xsk_set_rx_need_wakeup(pool);
		smp_mb(); /* Order NEED_WAKEUP before the producer recheck. */
		pending = xskq_cons_nb_entries(fq, 1);
	}
	spin_unlock_bh(&provider->lock);

	return pending ? false : need_wakeup;
}

static int xp_pp_nl_fill(void *mp_priv, struct sk_buff *rsp,
			 struct netdev_rx_queue *rxq)
{
	return 0;
}

static void xp_pp_uninstall(void *mp_priv, struct netdev_rx_queue *rxq)
{
	struct xsk_buff_pool *pool = mp_priv;
	struct xsk_pp *provider = pool->pp;
	struct net_device *netdev = pool->netdev;

	if (!netdev)
		goto clear_rxq;

	/* Unregister uninstalls providers before the AF_XDP notifier runs. */
	xsk_clear_pool_at_qid(netdev, pool->queue_id);
	if (provider)
		provider->detached = true;
	else
		WARN_ON_ONCE(1);
	if (pool->dma_map) {
		pool->dma_map->netdev = NULL;
		xsk_pool_dma_unmap(pool, XSK_PAGE_POOL_DMA_ATTR);
	}
	pool->netdev = NULL;
	dev_put(netdev);

clear_rxq:
	/* Existing page_pools retain their private pool pointer until their
	 * deferred destruction; the device queue is no longer operational.
	 */
	memset(&rxq->mp_params, 0, sizeof(rxq->mp_params));
}

static const struct memory_provider_ops xsk_pp_ops = {
	.init			= xp_pp_init,
	.destroy		= xp_pp_destroy,
	.alloc_netmems		= xp_pp_alloc_netmems,
	.release_netmem		= xp_pp_release_netmem,
	.refill_done		= xp_pp_refill_done,
	.nl_fill		= xp_pp_nl_fill,
	.uninstall		= xp_pp_uninstall,
	.caps			= MP_CAP_READABLE,
};

static int xsk_pool_validate_page_pool(struct xsk_buff_pool *pool,
				       struct netlink_ext_ack *extack)
{
	if (PAGE_SIZE != SZ_4K) {
		NL_SET_ERR_MSG(extack,
			       "page pool UMEM requires 4 KiB base pages");
		return -EOPNOTSUPP;
	}
	if (pool->unaligned) {
		NL_SET_ERR_MSG(extack,
			       "page pool UMEM requires aligned chunks");
		return -EOPNOTSUPP;
	}
	/* Each provider object represents one complete UMEM chunk. */
	if (pool->chunk_size != PAGE_SIZE) {
		NL_SET_ERR_MSG(extack,
			       "page pool UMEM requires page-sized chunks");
		return -EOPNOTSUPP;
	}

	return 0;
}

int xsk_pool_setup_page_pool(struct net_device *dev, struct xsk_buff_pool *pool,
			     u16 queue_id, struct netlink_ext_ack *extack)
{
	struct xsk_pp *provider;
	struct device *dma_dev;
	struct pp_memory_provider_params mp = {
		.mp_ops = &xsk_pp_ops,
	};
	int err;

	ASSERT_RTNL();

	if (!pool) {
		pool = xsk_get_pool_from_qid(dev, queue_id);
		if (!pool)
			return -EINVAL;

		mp.mp_priv = pool;
		netif_mp_close_rxq(dev, queue_id, &mp);
		return 0;
	}
	if (xsk_get_pool_from_qid(dev, queue_id) != pool) {
		NL_SET_ERR_MSG(extack,
			       "designated queue has no matching XSK pool");
		return -EINVAL;
	}
	err = xsk_pool_validate_page_pool(pool, extack);
	if (err)
		return err;

	dma_dev = netdev_queue_get_dma_dev(dev, queue_id,
					   NETDEV_QUEUE_TYPE_RX);
	if (!dma_dev) {
		NL_SET_ERR_MSG(extack, "RX queue has no DMA device");
		return -EOPNOTSUPP;
	}

	err = xsk_pool_dma_map(pool, dma_dev, XSK_PAGE_POOL_DMA_ATTR);
	if (err)
		return err;

	provider = xp_pp_create(pool);
	if (IS_ERR(provider)) {
		err = PTR_ERR(provider);
		goto err_unmap;
	}

	mp.mp_priv = pool;
	mp.rx_page_size = pool->chunk_size;
	mp.rx_headroom = xsk_pool_get_headroom(pool);
	err = netif_mp_open_rxq(dev, queue_id, &mp, extack);
	if (err)
		goto err_free_provider;

	return 0;

err_free_provider:
	provider->detached = true;
	/* A deferred page_pool destroy keeps pool->pp installed. That state
	 * suppresses copy-mode fallback in xp_assign_dev(), while xp_destroy()
	 * keeps the UMEM alive until the provider destroy callback completes.
	 */
	xp_pp_teardown(pool);
	return err;
err_unmap:
	xsk_pool_dma_unmap(pool, XSK_PAGE_POOL_DMA_ATTR);
	return err;
}
EXPORT_SYMBOL_GPL(xsk_pool_setup_page_pool);

static bool xp_addr_crosses_non_contig_pg(struct xsk_buff_pool *pool,
					  u64 addr)
{
	return xp_desc_crosses_non_contig_pg(pool, addr, pool->chunk_size);
}

static bool xp_check_unaligned(struct xsk_buff_pool *pool, u64 *addr)
{
	*addr = xp_unaligned_extract_addr(*addr);
	if (*addr >= pool->addrs_cnt ||
	    *addr + pool->chunk_size > pool->addrs_cnt ||
	    xp_addr_crosses_non_contig_pg(pool, *addr))
		return false;
	return true;
}

static bool xp_check_aligned(struct xsk_buff_pool *pool, u64 *addr)
{
	*addr = xp_aligned_extract_addr(pool, *addr);
	return *addr < pool->addrs_cnt;
}

static struct xdp_buff_xsk *xp_get_xskb(struct xsk_buff_pool *pool, u64 addr)
{
	struct xdp_buff_xsk *xskb;

	if (pool->unaligned) {
		xskb = pool->free_heads[--pool->free_heads_cnt];
		xp_init_xskb_addr(xskb, pool, addr);
		if (pool->dma_pages)
			xp_init_xskb_dma(xskb, pool, pool->dma_pages, addr);
	} else {
		xskb = &pool->heads[xp_aligned_extract_idx(pool, addr)];
	}

	return xskb;
}

static struct xdp_buff_xsk *__xp_alloc(struct xsk_buff_pool *pool)
{
	struct xdp_buff_xsk *xskb;
	u64 addr;
	bool ok;

	if (pool->free_heads_cnt == 0)
		return NULL;

	for (;;) {
		if (!xskq_cons_peek_addr_unchecked(pool->fq, &addr)) {
			pool->fq->queue_empty_descs++;
			return NULL;
		}

		ok = pool->unaligned ? xp_check_unaligned(pool, &addr) :
		     xp_check_aligned(pool, &addr);
		if (!ok) {
			pool->fq->invalid_descs++;
			xskq_cons_release(pool->fq);
			continue;
		}
		break;
	}

	xskb = xp_get_xskb(pool, addr);

	xskq_cons_release(pool->fq);
	return xskb;
}

struct xdp_buff *xp_alloc(struct xsk_buff_pool *pool)
{
	struct xdp_buff_xsk *xskb;

	if (!pool->free_list_cnt) {
		xskb = __xp_alloc(pool);
		if (!xskb)
			return NULL;
	} else {
		pool->free_list_cnt--;
		xskb = list_first_entry(&pool->free_list, struct xdp_buff_xsk,
					list_node);
		list_del_init(&xskb->list_node);
	}

	xskb->xdp.data = xskb->xdp.data_hard_start + XDP_PACKET_HEADROOM;
	xskb->xdp.data_meta = xskb->xdp.data;
	xskb->xdp.flags = 0;

	if (pool->dev)
		xp_dma_sync_for_device(pool, xskb->dma, pool->frame_len);

	return &xskb->xdp;
}
EXPORT_SYMBOL(xp_alloc);

static u32 xp_alloc_new_from_fq(struct xsk_buff_pool *pool, struct xdp_buff **xdp, u32 max)
{
	u32 i, cached_cons, nb_entries;

	if (max > pool->free_heads_cnt)
		max = pool->free_heads_cnt;
	max = xskq_cons_nb_entries(pool->fq, max);

	cached_cons = pool->fq->cached_cons;
	nb_entries = max;
	i = max;
	while (i--) {
		struct xdp_buff_xsk *xskb;
		u64 addr;
		bool ok;

		__xskq_cons_read_addr_unchecked(pool->fq, cached_cons++, &addr);

		ok = pool->unaligned ? xp_check_unaligned(pool, &addr) :
			xp_check_aligned(pool, &addr);
		if (unlikely(!ok)) {
			pool->fq->invalid_descs++;
			nb_entries--;
			continue;
		}

		xskb = xp_get_xskb(pool, addr);

		*xdp = &xskb->xdp;
		xdp++;
	}

	xskq_cons_release_n(pool->fq, max);
	return nb_entries;
}

static u32 xp_alloc_reused(struct xsk_buff_pool *pool, struct xdp_buff **xdp, u32 nb_entries)
{
	struct xdp_buff_xsk *xskb;
	u32 i;

	nb_entries = min_t(u32, nb_entries, pool->free_list_cnt);

	i = nb_entries;
	while (i--) {
		xskb = list_first_entry(&pool->free_list, struct xdp_buff_xsk, list_node);
		list_del_init(&xskb->list_node);

		*xdp = &xskb->xdp;
		xdp++;
	}
	pool->free_list_cnt -= nb_entries;

	return nb_entries;
}

static u32 xp_alloc_slow(struct xsk_buff_pool *pool, struct xdp_buff **xdp,
			 u32 max)
{
	int i;

	for (i = 0; i < max; i++) {
		struct xdp_buff *buff;

		buff = xp_alloc(pool);
		if (unlikely(!buff))
			return i;
		*xdp = buff;
		xdp++;
	}

	return max;
}

u32 xp_alloc_batch(struct xsk_buff_pool *pool, struct xdp_buff **xdp, u32 max)
{
	u32 nb_entries1 = 0, nb_entries2;

	if (unlikely(pool->dev && dma_dev_need_sync(pool->dev)))
		return xp_alloc_slow(pool, xdp, max);

	if (unlikely(pool->free_list_cnt)) {
		nb_entries1 = xp_alloc_reused(pool, xdp, max);
		if (nb_entries1 == max)
			return nb_entries1;

		max -= nb_entries1;
		xdp += nb_entries1;
	}

	nb_entries2 = xp_alloc_new_from_fq(pool, xdp, max);
	if (!nb_entries2)
		pool->fq->queue_empty_descs++;

	return nb_entries1 + nb_entries2;
}
EXPORT_SYMBOL(xp_alloc_batch);

bool xp_can_alloc(struct xsk_buff_pool *pool, u32 count)
{
	u32 req_count, avail_count;

	if (pool->free_list_cnt >= count)
		return true;

	req_count = count - pool->free_list_cnt;
	avail_count = xskq_cons_nb_entries(pool->fq, req_count);
	if (!avail_count)
		pool->fq->queue_empty_descs++;

	return avail_count >= req_count;
}
EXPORT_SYMBOL(xp_can_alloc);

void xp_free(struct xdp_buff_xsk *xskb)
{
	if (!list_empty(&xskb->list_node))
		return;

	xskb->pool->free_list_cnt++;
	list_add(&xskb->list_node, &xskb->pool->free_list);
}
EXPORT_SYMBOL(xp_free);

static u64 __xp_raw_get_addr(const struct xsk_buff_pool *pool, u64 addr)
{
	return pool->unaligned ? xp_unaligned_add_offset_to_addr(addr) : addr;
}

static void *__xp_raw_get_data(const struct xsk_buff_pool *pool, u64 addr)
{
	return pool->addrs + addr;
}

void *xp_raw_get_data(struct xsk_buff_pool *pool, u64 addr)
{
	return __xp_raw_get_data(pool, __xp_raw_get_addr(pool, addr));
}
EXPORT_SYMBOL(xp_raw_get_data);

static dma_addr_t __xp_raw_get_dma(const struct xsk_buff_pool *pool, u64 addr)
{
	return (pool->dma_pages[addr >> PAGE_SHIFT] &
		~XSK_NEXT_PG_CONTIG_MASK) +
		(addr & ~PAGE_MASK);
}

dma_addr_t xp_raw_get_dma(struct xsk_buff_pool *pool, u64 addr)
{
	return __xp_raw_get_dma(pool, __xp_raw_get_addr(pool, addr));
}
EXPORT_SYMBOL(xp_raw_get_dma);

/**
 * xp_raw_get_ctx - get &xdp_desc context
 * @pool: XSk buff pool desc address belongs to
 * @addr: desc address (from userspace)
 * @options: desc options (from userspace)
 *
 * Helper for getting desc's DMA address and metadata pointer, if present.
 * Saves one call on hotpath and double calculation of the actual address.
 * Metadata is validated later by xsk_tx_metadata_request().
 *
 * Return: new &xdp_desc_ctx struct containing desc's DMA address and metadata
 * pointer, if it is present (initialized to %NULL otherwise).
 */
struct xdp_desc_ctx xp_raw_get_ctx(const struct xsk_buff_pool *pool, u64 addr,
				   u32 options)
{
	struct xdp_desc_ctx ret;

	addr = __xp_raw_get_addr(pool, addr);

	ret.dma = __xp_raw_get_dma(pool, addr);
	ret.meta = __xsk_buff_get_metadata(pool, __xp_raw_get_data(pool, addr),
					   options);

	return ret;
}
EXPORT_SYMBOL(xp_raw_get_ctx);
