/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Device memory TCP support
 *
 * Authors:	Mina Almasry <almasrymina@google.com>
 *		Willem de Bruijn <willemb@google.com>
 *		Kaiyuan Zhang <kaiyuanz@google.com>
 *
 */
#ifndef _NET_DEVMEM_H
#define _NET_DEVMEM_H

struct netdev_dmabuf_binding {
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attachment;
	struct sg_table *sgt;
	struct net_device *dev;
	struct gen_pool *chunk_pool;

	/* The user holds a ref (via the netlink API) for as long as they want
	 * the binding to remain alive. Each page pool using this binding holds
	 * a ref to keep the binding alive. Each allocated page_pool_iov holds a
	 * ref.
	 *
	 * The binding undos itself and unmaps the underlying dmabuf once all
	 * those refs are dropped and the binding is no longer desired or in
	 * use.
	 */
	refcount_t ref;

	/* The portid of the user that owns this binding. Used for netlink to
	 * notify us of the user dropping the bind.
	 */
	u32 owner_nlportid;

	/* The list of bindings currently active. Used for netlink to notify us
	 * of the user dropping the bind.
	 */
	struct list_head list;

	/* rxq's this binding is active on. */
	struct xarray bound_rxq_list;

	/* ID of this binding. Globally unique to all bindings currently
	 * active.
	 */
	u32 id;
};

#ifdef CONFIG_DMA_SHARED_BUFFER
void __netdev_dmabuf_binding_free(struct netdev_dmabuf_binding *binding);
int netdev_bind_dmabuf(struct net_device *dev, unsigned int dmabuf_fd,
		       struct netdev_dmabuf_binding **out);
void netdev_unbind_dmabuf(struct netdev_dmabuf_binding *binding);
int netdev_bind_dmabuf_to_queue(struct net_device *dev, u32 rxq_idx,
				struct netdev_dmabuf_binding *binding);
#else
static inline void
__netdev_dmabuf_binding_free(struct netdev_dmabuf_binding *binding)
{
}

static inline int netdev_bind_dmabuf(struct net_device *dev,
				     unsigned int dmabuf_fd,
				     struct netdev_dmabuf_binding **out)
{
	return -EOPNOTSUPP;
}
static inline void netdev_unbind_dmabuf(struct netdev_dmabuf_binding *binding)
{
}

static inline int
netdev_bind_dmabuf_to_queue(struct net_device *dev, u32 rxq_idx,
			    struct netdev_dmabuf_binding *binding)
{
	return -EOPNOTSUPP;
}
#endif

static inline void
netdev_dmabuf_binding_get(struct netdev_dmabuf_binding *binding)
{
	refcount_inc(&binding->ref);
}

static inline void
netdev_dmabuf_binding_put(struct netdev_dmabuf_binding *binding)
{
	if (!refcount_dec_and_test(&binding->ref))
		return;

	__netdev_dmabuf_binding_free(binding);
}

#endif /* _NET_DEVMEM_H */
