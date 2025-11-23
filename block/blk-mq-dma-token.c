#include <linux/blk-mq-dma-token.h>
#include <linux/dma-resv.h>

struct blk_mq_dma_fence {
	struct dma_fence base;
	spinlock_t lock;
};

static const char *blk_mq_fence_drv_name(struct dma_fence *fence)
{
	return "blk-mq";
}

const struct dma_fence_ops blk_mq_dma_fence_ops = {
	.get_driver_name = blk_mq_fence_drv_name,
	.get_timeline_name = blk_mq_fence_drv_name,
};

static void blk_mq_dma_token_free(struct blk_mq_dma_token *token)
{
	token->q->mq_ops->clean_dma_token(token->q, token);
	dma_buf_put(token->dmabuf);
	kfree(token);
}

static inline void blk_mq_dma_token_put(struct blk_mq_dma_token *token)
{
	if (refcount_dec_and_test(&token->refs))
		blk_mq_dma_token_free(token);
}

static void blk_mq_dma_mapping_free(struct blk_mq_dma_map *map)
{
	struct blk_mq_dma_token *token = map->token;

	if (map->sgt)
		token->q->mq_ops->dma_unmap(token->q, map);

	dma_fence_put(&map->fence->base);
	percpu_ref_exit(&map->refs);
	kfree(map);
	blk_mq_dma_token_put(token);
}

static void blk_mq_dma_map_work_free(struct work_struct *work)
{
	struct blk_mq_dma_map *map = container_of(work, struct blk_mq_dma_map,
						free_work);

	dma_fence_signal(&map->fence->base);
	blk_mq_dma_mapping_free(map);
}

static void blk_mq_dma_map_refs_free(struct percpu_ref *ref)
{
	struct blk_mq_dma_map *map = container_of(ref, struct blk_mq_dma_map, refs);

	INIT_WORK(&map->free_work, blk_mq_dma_map_work_free);
	queue_work(system_wq, &map->free_work);
}

static struct blk_mq_dma_map *blk_mq_alloc_dma_mapping(struct blk_mq_dma_token *token)
{
	struct blk_mq_dma_fence *fence = NULL;
	struct blk_mq_dma_map *map;
	int ret = -ENOMEM;

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return ERR_PTR(-ENOMEM);

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence)
		goto err;

	ret = percpu_ref_init(&map->refs, blk_mq_dma_map_refs_free, 0,
			      GFP_KERNEL);
	if (ret)
		goto err;

	dma_fence_init(&fence->base, &blk_mq_dma_fence_ops, &fence->lock,
			token->fence_ctx, atomic_inc_return(&token->fence_seq));
	spin_lock_init(&fence->lock);
	map->fence = fence;
	map->token = token;
	refcount_inc(&token->refs);
	return map;
err:
	kfree(map);
	kfree(fence);
	return ERR_PTR(ret);
}

static inline
struct blk_mq_dma_map *blk_mq_get_token_map(struct blk_mq_dma_token *token)
{
	struct blk_mq_dma_map *map;

	guard(rcu)();

	map = rcu_dereference(token->map);
	if (unlikely(!map || !percpu_ref_tryget_live_rcu(&map->refs)))
		return NULL;
	return map;
}

static struct blk_mq_dma_map *
blk_mq_create_dma_map(struct blk_mq_dma_token *token)
{
	struct dma_buf *dmabuf = token->dmabuf;
	struct blk_mq_dma_map *map;
	long ret;

	guard(mutex)(&token->mapping_lock);

	map = blk_mq_get_token_map(token);
	if (map)
		return map;

	map = blk_mq_alloc_dma_mapping(token);
	if (IS_ERR(map))
		return NULL;

	dma_resv_lock(dmabuf->resv, NULL);
	ret = dma_resv_wait_timeout(dmabuf->resv, DMA_RESV_USAGE_BOOKKEEP,
				    true, MAX_SCHEDULE_TIMEOUT);
	ret = ret ? ret : -ETIME;
	if (ret > 0)
		ret = token->q->mq_ops->dma_map(token->q, map);
	dma_resv_unlock(dmabuf->resv);

	if (ret)
		return ERR_PTR(ret);

	percpu_ref_get(&map->refs);
	rcu_assign_pointer(token->map, map);
	return map;
}

static void blk_mq_dma_map_remove(struct blk_mq_dma_token *token)
{
	struct dma_buf *dmabuf = token->dmabuf;
	struct blk_mq_dma_map *map;
	int ret;

	dma_resv_assert_held(dmabuf->resv);

	ret = dma_resv_reserve_fences(dmabuf->resv, 1);
	if (WARN_ON_ONCE(ret))
		return;

	map = rcu_dereference_protected(token->map,
					dma_resv_held(dmabuf->resv));
	if (!map)
		return;
	rcu_assign_pointer(token->map, NULL);

	dma_resv_add_fence(dmabuf->resv, &map->fence->base,
			   DMA_RESV_USAGE_KERNEL);
	percpu_ref_kill(&map->refs);
}

blk_status_t blk_rq_assign_dma_map(struct request *rq,
				   struct blk_mq_dma_token *token)
{
	struct blk_mq_dma_map *map;

	map = blk_mq_get_token_map(token);
	if (map)
		goto complete;

	if (rq->cmd_flags & REQ_NOWAIT)
		return BLK_STS_AGAIN;

	map = blk_mq_create_dma_map(token);
	if (IS_ERR(map))
		return BLK_STS_RESOURCE;
complete:
	rq->dma_map = map;
	return BLK_STS_OK;
}

void blk_mq_dma_map_move_notify(struct blk_mq_dma_token *token)
{
	blk_mq_dma_map_remove(token);
}

static void blk_mq_release_dma_mapping(struct dma_token *base_token)
{
	struct blk_mq_dma_token *token = dma_token_to_blk_mq(base_token);
	struct dma_buf *dmabuf = token->dmabuf;

	dma_resv_lock(dmabuf->resv, NULL);
	blk_mq_dma_map_remove(token);
	dma_resv_unlock(dmabuf->resv);

	blk_mq_dma_token_put(token);
}

struct dma_token *blk_mq_dma_map(struct request_queue *q,
				  struct dma_token_params *params)
{
	struct dma_buf *dmabuf = params->dmabuf;
	struct blk_mq_dma_token *token;
	int ret;

	if (!q->mq_ops->dma_map || !q->mq_ops->dma_unmap ||
	    !q->mq_ops->init_dma_token || !q->mq_ops->clean_dma_token)
		return ERR_PTR(-EINVAL);

	token = kzalloc(sizeof(*token), GFP_KERNEL);
	if (!token)
		return ERR_PTR(-ENOMEM);

	get_dma_buf(dmabuf);
	token->fence_ctx = dma_fence_context_alloc(1);
	token->dmabuf = dmabuf;
	token->dir = params->dir;
	token->base.release = blk_mq_release_dma_mapping;
	token->q = q;
	refcount_set(&token->refs, 1);
	mutex_init(&token->mapping_lock);

	if (!blk_get_queue(q)) {
		kfree(token);
		return ERR_PTR(-EFAULT);
	}

	ret = token->q->mq_ops->init_dma_token(token->q, token);
	if (ret) {
		kfree(token);
		blk_put_queue(q);
		return ERR_PTR(ret);
	}
	return &token->base;
}
