/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_DMA_TOKEN_H
#define _LINUX_DMA_TOKEN_H

#include <linux/dma-buf.h>

struct io_dmabuf_fence;
struct io_dmabuf_token;
struct io_dmabuf_map;

struct io_dmabuf_token_dev_ops {
	/*
	 * Create a new map for the given token. It should be initialised
	 * with io_dmabuf_init_map(). The callback is executed with the
	 * reservation lock held.
	 */
	struct io_dmabuf_map *(*map)(struct io_dmabuf_token *);

	/*
	 * Clean up device specific parts of the map. The callback is
	 * executed with the reservation lock held.
	 */
	void (*unmap)(struct io_dmabuf_token *, struct io_dmabuf_map *);

	/*
	 * The user tries to destroy the token. Release all device specific
	 * parts of the token.
	 */
	void (*release)(struct io_dmabuf_token *);
};

struct io_dmabuf_map {
	/*
	 * Counts attached requests and other users. Device specific unmapping
	 * is deferred until all refs are dropped.
	 */
	struct percpu_ref		refs;

	struct work_struct		release_work;
	struct io_dmabuf_fence		*fence;
	struct io_dmabuf_token		*token;
};

struct io_dmabuf_token {
	struct io_dmabuf_map __rcu	*map;
	struct dma_buf			*dmabuf;
	enum dma_data_direction		dir;

	atomic_t			fence_seq;
	u64				fence_ctx;
	struct work_struct		release_work;
	refcount_t			refs;

	void					*dev_priv;
	const struct io_dmabuf_token_dev_ops	*dev_ops;
};

int io_dmabuf_token_create(struct file *file,
			   struct io_dmabuf_token *token,
			   struct dma_buf *dmabuf,
			   enum dma_data_direction dir);
void io_dmabuf_token_release(struct io_dmabuf_token *token);

struct io_dmabuf_map *io_dmabuf_create_map(struct io_dmabuf_token *token);

static inline struct io_dmabuf_map *io_dmabuf_get_map(struct io_dmabuf_token *token)
{
	struct io_dmabuf_map *map;

	guard(rcu)();

	map = rcu_dereference(token->map);
	if (unlikely(!map || !percpu_ref_tryget_live_rcu(&map->refs)))
		return NULL;

	return map;
}

static inline void io_dmabuf_map_drop(struct io_dmabuf_map *map)
{
	percpu_ref_put(&map->refs);
}

/*
 * Device API
 */

void io_dmabuf_token_invalidate_mappings(struct io_dmabuf_token *token);
int io_dmabuf_init_map(struct io_dmabuf_token *token, struct io_dmabuf_map *map);


#endif
