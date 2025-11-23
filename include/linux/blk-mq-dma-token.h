/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BLK_MQ_DMA_TOKEN_H
#define BLK_MQ_DMA_TOKEN_H

#include <linux/blk-mq.h>
#include <linux/dma_token.h>
#include <linux/percpu-refcount.h>

struct blk_mq_dma_token;
struct blk_mq_dma_fence;

struct blk_mq_dma_map {
	void				*private;

	struct percpu_ref		refs;
	struct sg_table			*sgt;
	struct blk_mq_dma_token		*token;
	struct blk_mq_dma_fence		*fence;
	struct work_struct		free_work;
};

struct blk_mq_dma_token {
	struct dma_token		base;
	enum dma_data_direction		dir;

	void				*private;

	struct dma_buf			*dmabuf;
	struct blk_mq_dma_map __rcu	*map;
	struct request_queue		*q;

	struct mutex			mapping_lock;
	refcount_t			refs;

	atomic_t			fence_seq;
	u64				fence_ctx;
};

static inline
struct blk_mq_dma_token *dma_token_to_blk_mq(struct dma_token *token)
{
	return container_of(token, struct blk_mq_dma_token, base);
}

blk_status_t blk_rq_assign_dma_map(struct request *req,
				   struct blk_mq_dma_token *token);

static inline void blk_rq_drop_dma_map(struct request *rq)
{
	if (rq->dma_map) {
		percpu_ref_put(&rq->dma_map->refs);
		rq->dma_map = NULL;
	}
}

void blk_mq_dma_map_move_notify(struct blk_mq_dma_token *token);
struct dma_token *blk_mq_dma_map(struct request_queue *q,
				 struct dma_token_params *params);

#endif /* BLK_MQ_DMA_TOKEN_H */
