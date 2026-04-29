/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Common infrastructure for supporing dma-buf in the I/O path.
 *
 * Copyright (C) 2026 Pavel Begunkov <asml.silence@gmail.com>
 */
#include <linux/io_dmabuf_token.h>
#include <linux/dma-resv.h>

struct io_dmabuf_fence {
	struct dma_fence base;
	spinlock_t lock;
};

static const char *io_dmabuf_fence_drv_name(struct dma_fence *fence)
{
	/* default fence release kfree's the base pointer */
	BUILD_BUG_ON(offsetof(struct io_dmabuf_fence, base));

	return "DMABUF token";
}

static const char *io_dmabuf_fence_timeline_name(struct dma_fence *fence)
{
	return "DMABUF token";
}

const struct dma_fence_ops io_dmabuf_fence_ops = {
	.get_driver_name = io_dmabuf_fence_drv_name,
	.get_timeline_name = io_dmabuf_fence_timeline_name,
};

static void io_dmabuf_token_destroy_work(struct work_struct *work)
{
	struct io_dmabuf_token *token = container_of(work, struct io_dmabuf_token,
				  release_work);

	if (WARN_ON_ONCE(refcount_read(&token->refs)))
		return;

	token->dev_ops->release(token);
	dma_buf_put(token->dmabuf);
	kfree(token);
}

static void io_dmabuf_map_release_work(struct work_struct *work)
{
	struct io_dmabuf_map *map = container_of(work, struct io_dmabuf_map,
					         release_work);
	struct io_dmabuf_fence *fence = map->fence;
	struct io_dmabuf_token *token = map->token;
	struct dma_buf *dmabuf = token->dmabuf;

	/* the release path must wait for fences */
	if (WARN_ON_ONCE(refcount_read(&token->refs) == 0))
		return;

	/* Prevent from destoying the token while unmapping */
	refcount_inc(&token->refs);

	/*
	 * There are no more requests using the map, we can signal the fence.
	 * It should be done before taking the resv lock as someone could be
	 * waiting for the fence while holding the lock.
	 */
	dma_fence_signal(&fence->base);

	dma_resv_lock(dmabuf->resv, NULL);
	token->dev_ops->unmap(token, map);
	dma_resv_unlock(dmabuf->resv);

	dma_fence_put(&fence->base);
	percpu_ref_exit(&map->refs);
	kfree(map);

	if (refcount_dec_and_test(&token->refs)) {
		/*
		 * Destruction needs to wait for I/O and dma fences. Defer it to
		 * simplify locking.
		 */
		INIT_WORK(&token->release_work, io_dmabuf_token_destroy_work);
		queue_work(system_wq, &token->release_work);
	}
}

static void io_dmabuf_map_refs_release(struct percpu_ref *ref)
{
	struct io_dmabuf_map *map = container_of(ref, struct io_dmabuf_map, refs);

	/* might sleep, use a worker */
	INIT_WORK(&map->release_work, io_dmabuf_map_release_work);
	queue_work(system_wq, &map->release_work);
}

int io_dmabuf_init_map(struct io_dmabuf_token *token, struct io_dmabuf_map *map)
{
	struct io_dmabuf_fence *fence = NULL;
	int ret;

	fence = kzalloc(sizeof(*fence), GFP_KERNEL);
	if (!fence)
		return -ENOMEM;

	ret = percpu_ref_init(&map->refs, io_dmabuf_map_refs_release, 0, GFP_KERNEL);
	if (ret) {
		kfree(fence);
		return ret;
	}

	spin_lock_init(&fence->lock);
	dma_fence_init(&fence->base, &io_dmabuf_fence_ops, &fence->lock,
			token->fence_ctx, atomic_inc_return(&token->fence_seq));
	map->fence = fence;
	map->token = token;
	return 0;
}
EXPORT_SYMBOL_NS_GPL(io_dmabuf_init_map, "DMA_BUF");

struct io_dmabuf_map *io_dmabuf_create_map(struct io_dmabuf_token *token)
{
	struct dma_buf *dmabuf = token->dmabuf;
	struct io_dmabuf_map *map;
	long ret;

retry:
	/*
	 * ->dmabuf_map() will be calling dma_buf_map_attachment(), for which
	 * we'll need to wait for fences. Do a bit nicer and try to wait
	 * without the resv lock first.
	 */
	ret = dma_resv_wait_timeout(dmabuf->resv, DMA_RESV_USAGE_KERNEL,
				    true, MAX_SCHEDULE_TIMEOUT);
	if (!ret)
		ret = -EAGAIN;
	if (ret < 0)
		return ERR_PTR(ret);

	dma_resv_lock(dmabuf->resv, NULL);
	map = io_dmabuf_get_map(token);
	if (map) {
		ret = 0;
		goto out;
	}

	if (dma_resv_wait_timeout(dmabuf->resv, DMA_RESV_USAGE_KERNEL,
				  true, 0) < 0) {
		dma_resv_unlock(dmabuf->resv);
		goto retry;
	}

	map = token->dev_ops->map(token);
	if (IS_ERR(map)) {
		ret = PTR_ERR(map);
		goto out;
	}

	percpu_ref_get(&map->refs);
	rcu_assign_pointer(token->map, map);
out:
	dma_resv_unlock(dmabuf->resv);
	if (ret < 0)
		return ERR_PTR(ret);
	return map;
}

static void io_dmabuf_drop_map(struct io_dmabuf_token *token)
{
	struct dma_buf *dmabuf = token->dmabuf;
	struct io_dmabuf_map *map;
	int ret;

	dma_resv_assert_held(dmabuf->resv);

	map = rcu_dereference_protected(token->map,
					dma_resv_held(dmabuf->resv));
	if (!map)
		return;
	rcu_assign_pointer(token->map, NULL);

	ret = dma_resv_reserve_fences(dmabuf->resv, 1);
	if (WARN_ON_ONCE(ret)) {
		struct dma_fence *fence = &map->fence->base;

		dma_fence_get(fence);
		percpu_ref_kill(&map->refs);
		dma_fence_wait(fence, false);
		dma_fence_put(fence);
		return;
	}

	dma_resv_add_fence(dmabuf->resv, &map->fence->base,
			   DMA_RESV_USAGE_KERNEL);
	/*
	 * Delay destruction until all inflight requests using the map are
	 * gone. It'll also signal the fence then.
	 */
	percpu_ref_kill(&map->refs);
}

void io_dmabuf_token_invalidate_mappings(struct io_dmabuf_token *token)
{
	io_dmabuf_drop_map(token);
}
EXPORT_SYMBOL_NS_GPL(io_dmabuf_token_invalidate_mappings, "DMA_BUF");

static void io_dmabuf_token_release_work(struct work_struct *work)
{
	struct io_dmabuf_token *token = container_of(work, struct io_dmabuf_token,
						  release_work);
	struct dma_buf *dmabuf = token->dmabuf;
	long ret;

	dma_resv_lock(dmabuf->resv, NULL);
	/* Remove the last map, there should be no new ones going forward. */
	io_dmabuf_drop_map(token);
	dma_resv_unlock(dmabuf->resv);

	/* Wait until all maps are destroyed. */
	ret = dma_resv_wait_timeout(dmabuf->resv, DMA_RESV_USAGE_KERNEL,
				    false, MAX_SCHEDULE_TIMEOUT);

	if (WARN_ON_ONCE(ret <= 0))
		return;
	if (WARN_ON_ONCE(rcu_dereference_protected(token->map, true)))
		return;

	if (refcount_dec_and_test(&token->refs))
		io_dmabuf_token_destroy_work(&token->release_work);
}

void io_dmabuf_token_release(struct io_dmabuf_token *token)
{
	/*
	 * Destruction needs to wait for I/O and dma fences. Defer it to
	 * simplify locking.
	 */
	INIT_WORK(&token->release_work, io_dmabuf_token_release_work);
	queue_work(system_wq, &token->release_work);
}

int io_dmabuf_token_create(struct file *file,
			   struct io_dmabuf_token *token,
			   struct dma_buf *dmabuf,
			   enum dma_data_direction dir)
{
	int ret;

	if (!file->f_op->create_dmabuf_token)
		return -EOPNOTSUPP;

	memset(token, 0, sizeof(*token));
	token->fence_ctx = dma_fence_context_alloc(1);
	token->dir = dir;
	token->dmabuf = dmabuf;
	refcount_set(&token->refs, 1);
	get_dma_buf(dmabuf);

	ret = file->f_op->create_dmabuf_token(file, token);
	if (ret) {
		memset(token, 0, sizeof(*token));
		dma_buf_put(dmabuf);
		return ret;
	}

	if (WARN_ON_ONCE(!token->dev_ops ||
			 !token->dev_ops->map ||
			 !token->dev_ops->unmap ||
			 !token->dev_ops->release))
		return -EINVAL;

	return ret;
}
