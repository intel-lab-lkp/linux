// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/f2fs_fs.h>
#include <linux/refcount.h>
#include <linux/slab.h>

#include "f2fs.h"
#include "node_cache_compress.h"

static const u32 f2fs_nc_bucket_sizes[] = {
	F2FS_NC_BUCKET_256_SIZE,
	F2FS_NC_BUCKET_512_SIZE,
	F2FS_NC_BUCKET_1024_SIZE,
};

/* One private slab and active-object count for each compressed bucket. */
struct f2fs_nc_store {
	struct kmem_cache *caches[ARRAY_SIZE(f2fs_nc_bucket_sizes)];
	atomic_long_t objects[ARRAY_SIZE(f2fs_nc_bucket_sizes)];
};

static int f2fs_nc_store_bucket_from_size(u32 alloc_size)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(f2fs_nc_bucket_sizes); i++)
		if (alloc_size == f2fs_nc_bucket_sizes[i])
			return i;
	return -EINVAL;
}

static struct f2fs_nc_store *f2fs_nc_store_create(struct f2fs_sb_info *sbi)
{
	struct f2fs_nc_store *store;
	char name[64];
	int i;

	store = kzalloc_obj(*store, GFP_NOFS);
	if (!store)
		return NULL;
	for (i = 0; i < ARRAY_SIZE(f2fs_nc_bucket_sizes); i++) {
		if (snprintf(name, sizeof(name), "f2fs-nc-%u-%u-%u",
			     MAJOR(sbi->sb->s_dev), MINOR(sbi->sb->s_dev),
			     f2fs_nc_bucket_sizes[i]) >= sizeof(name))
			goto fail;
		store->caches[i] = kmem_cache_create(name,
						     f2fs_nc_bucket_sizes[i], 0,
				SLAB_RECLAIM_ACCOUNT | SLAB_NO_MERGE, NULL);
		if (!store->caches[i])
			goto fail;
	}
	return store;

fail:
	while (i--)
		kmem_cache_destroy(store->caches[i]);
	kfree(store);
	return NULL;
}

static void f2fs_nc_store_destroy(struct f2fs_nc_store *store)
{
	int i;

	if (!store)
		return;
	for (i = ARRAY_SIZE(f2fs_nc_bucket_sizes); i-- > 0;) {
		WARN_ON_ONCE(atomic_long_read(&store->objects[i]));
		kmem_cache_destroy(store->caches[i]);
	}
	kfree(store);
}

static void f2fs_nc_store_free(struct f2fs_nc_store *store, void *object,
			       u32 alloc_size)
{
	int bucket = f2fs_nc_store_bucket_from_size(alloc_size);

	if (WARN_ON_ONCE(!store || !object || bucket < 0))
		return;
	kmem_cache_free(store->caches[bucket], object);
	atomic_long_dec(&store->objects[bucket]);
}

/*
 * Per-superblock compression state.  Compressed objects hold references, so
 * the context can outlive its mount until every detached object is freed.
 */
struct f2fs_nc_ctx {
	struct f2fs_sb_info *sbi;
	struct f2fs_nc_store *store;
	/* Compressed queues only; raw entries use NODE_CACHE()->lru_list. */
	struct list_head queues[F2FS_NC_NR_QUEUES - 1];
	/* Current population and compressed bytes by queue. */
	atomic_long_t attached[F2FS_NC_NR_QUEUES];
	atomic64_t attached_payload[F2FS_NC_NR_QUEUES - 1];
	atomic64_t attached_slot_bytes[F2FS_NC_NR_QUEUES - 1];
	/* Cumulative detach statistics since mount. */
	atomic64_t detached[F2FS_NC_NR_QUEUES];
	/* Mount reference plus references held by compressed objects. */
	refcount_t refs;
};

static struct f2fs_node_cached_block *f2fs_nc_node_entry(struct f2fs_cached_block *entry)
{
	return container_of(entry, struct f2fs_node_cached_block, base);
}

static void f2fs_nc_ctx_release(struct f2fs_nc_ctx *ctx)
{
	int i;

	for (i = 0; i < F2FS_NC_NR_QUEUES; i++)
		WARN_ON_ONCE(atomic_long_read(&ctx->attached[i]));
	f2fs_nc_store_destroy(ctx->store);
	kfree(ctx);
}

static void f2fs_nc_ctx_put(struct f2fs_nc_ctx *ctx)
{
	if (refcount_dec_and_test(&ctx->refs))
		f2fs_nc_ctx_release(ctx);
}

static void f2fs_nc_account_add(struct f2fs_nc_ctx *ctx, unsigned int queue,
				u32 len, u32 alloc_size)
{
	atomic_long_inc(&ctx->attached[queue]);
	if (queue == F2FS_NC_RAW)
		return;
	atomic64_add(len, &ctx->attached_payload[queue - 1]);
	atomic64_add(alloc_size, &ctx->attached_slot_bytes[queue - 1]);
}

static void f2fs_nc_account_del(struct f2fs_nc_ctx *ctx, unsigned int queue,
				u32 len, u32 alloc_size)
{
	WARN_ON_ONCE(atomic_long_read(&ctx->attached[queue]) <= 0);
	atomic_long_dec(&ctx->attached[queue]);
	if (queue == F2FS_NC_RAW)
		return;
	WARN_ON_ONCE(atomic64_read(&ctx->attached_payload[queue - 1]) < len);
	WARN_ON_ONCE(atomic64_read(&ctx->attached_slot_bytes[queue - 1]) <
			alloc_size);
	atomic64_sub(len, &ctx->attached_payload[queue - 1]);
	atomic64_sub(alloc_size, &ctx->attached_slot_bytes[queue - 1]);
}

size_t f2fs_nc_entry_alloc_size(struct f2fs_cached_block_list *cache)
{
	if (IS_NODE_CACHE(cache) && cache->sbi->node_compress)
		return sizeof(struct f2fs_node_cached_block);
	return sizeof(struct f2fs_cached_block);
}

void f2fs_nc_init(struct f2fs_sb_info *sbi)
{
	struct f2fs_nc_ctx *ctx;
	int i;

	ctx = kzalloc_obj(*ctx, GFP_NOFS);
	if (!ctx)
		goto fail_open;
	ctx->sbi = sbi;
	ctx->store = f2fs_nc_store_create(sbi);
	if (!ctx->store)
		goto fail_open;
	for (i = 0; i < ARRAY_SIZE(ctx->queues); i++)
		INIT_LIST_HEAD(&ctx->queues[i]);
	refcount_set(&ctx->refs, 1);
	sbi->node_compress = ctx;
	return;

fail_open:
	if (ctx)
		f2fs_nc_ctx_release(ctx);
	f2fs_warn_ratelimited(sbi,
			      "node cache compression resources are unavailable");
}

void f2fs_nc_destroy(struct f2fs_sb_info *sbi)
{
	struct f2fs_nc_ctx *ctx = sbi->node_compress;

	if (!ctx)
		return;
	sbi->node_compress = NULL;
	f2fs_nc_ctx_put(ctx);
}

struct list_head *f2fs_nc_queue_head(struct f2fs_cached_block_list *cache,
				     unsigned int queue)
{
	struct f2fs_nc_ctx *ctx;

	if (queue == F2FS_NC_RAW)
		return &cache->lru_list;
	if (!IS_NODE_CACHE(cache) || queue >= F2FS_NC_NR_QUEUES)
		return NULL;
	ctx = cache->sbi->node_compress;
	return ctx ? &ctx->queues[queue - 1] : NULL;
}

unsigned int f2fs_nc_entry_queue(const struct f2fs_cached_block *entry)
{
	const struct f2fs_node_cached_block *node;
	int bucket;

	if (!f2fs_cache_test_compressed(entry))
		return F2FS_NC_RAW;
	node = container_of(entry, struct f2fs_node_cached_block, base);
	bucket = f2fs_nc_store_bucket_from_size(node->compressed_alloc_size);
	if (WARN_ON_ONCE(bucket < 0))
		return F2FS_NC_RAW;
	return bucket + 1;
}

void f2fs_nc_entry_attached(struct f2fs_cached_block *entry)
{
	struct f2fs_nc_ctx *ctx;

	if (!entry->cache || !IS_NODE_CACHE(entry->cache))
		return;
	ctx = entry->cache->sbi->node_compress;
	if (ctx)
		f2fs_nc_account_add(ctx, F2FS_NC_RAW, 0, 0);
}

void f2fs_nc_entry_detached(struct f2fs_cached_block *entry)
{
	struct f2fs_node_cached_block *node;
	struct f2fs_nc_ctx *ctx;
	unsigned int queue;
	u32 len = 0, alloc_size = 0;

	if (!entry->cache || !IS_NODE_CACHE(entry->cache))
		return;
	ctx = entry->cache->sbi->node_compress;
	if (!ctx)
		return;
	queue = f2fs_nc_entry_queue(entry);
	if (queue != F2FS_NC_RAW) {
		node = f2fs_nc_node_entry(entry);
		len = node->compressed_len;
		alloc_size = node->compressed_alloc_size;
	}
	atomic64_inc(&ctx->detached[queue]);
	f2fs_nc_account_del(ctx, queue, len, alloc_size);
}

void f2fs_nc_free_data(struct f2fs_cached_block *entry)
{
	struct f2fs_node_cached_block *node;
	struct f2fs_nc_ctx *ctx;
	void *object;
	u32 alloc_size;

	if (!f2fs_cache_test_compressed(entry)) {
		kfree(entry->data);
		return;
	}
	node = f2fs_nc_node_entry(entry);
	ctx = node->owner;
	object = entry->data;
	alloc_size = node->compressed_alloc_size;
	if (WARN_ON_ONCE(!ctx || !object ||
			 f2fs_nc_store_bucket_from_size(alloc_size) < 0))
		return;
	f2fs_cache_clear_compressed(entry);
	entry->data = NULL;
	node->owner = NULL;
	node->compressed_len = 0;
	node->compressed_alloc_size = 0;
	node->compressed_crc = 0;
	f2fs_nc_store_free(ctx->store, object, alloc_size);
	f2fs_nc_ctx_put(ctx);
}

void f2fs_nc_memory_usage(struct f2fs_sb_info *sbi,
			  struct f2fs_nc_memory *memory)
{
	struct f2fs_nc_ctx *ctx = sbi->node_compress;
	u64 entries = 0, data = 0;
	unsigned int i;

	if (!ctx) {
		memory->entry_bytes = (u64)NODE_CACHE(sbi)->num_entries *
				sizeof(struct f2fs_cached_block);
		memory->data_bytes = (u64)NODE_CACHE(sbi)->num_entries *
				sbi->blocksize;
		return;
	}
	spin_lock(&NODE_CACHE(sbi)->list_lock);
	for (i = 0; i < F2FS_NC_NR_QUEUES; i++)
		entries += atomic_long_read(&ctx->attached[i]);
	data = (u64)atomic_long_read(&ctx->attached[F2FS_NC_RAW]) *
			sbi->blocksize;
	for (i = 0; i < F2FS_NC_NR_QUEUES - 1; i++)
		data += atomic64_read(&ctx->attached_slot_bytes[i]);
	spin_unlock(&NODE_CACHE(sbi)->list_lock);
	memory->entry_bytes = entries * sizeof(struct f2fs_node_cached_block);
	memory->data_bytes = data;
}
