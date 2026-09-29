// SPDX-License-Identifier: GPL-2.0
#include <linux/atomic.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/lz4.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "f2fs.h"
#include "node_cache_compress.h"

#define F2FS_NC_ALLOC_BACKOFF_MS		1000U
#define F2FS_NC_PROCESS_BATCH		32U

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

static int f2fs_nc_store_bucket(u32 len)
{
	int i;

	if (!len)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(f2fs_nc_bucket_sizes); i++)
		if (len <= f2fs_nc_bucket_sizes[i])
			return i;
	return -E2BIG;
}

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

static void *f2fs_nc_store_alloc(struct f2fs_nc_store *store, u32 len,
				 gfp_t gfp, u32 *alloc_size)
{
	void *object;
	int bucket = f2fs_nc_store_bucket(len);

	if (!store || !alloc_size || bucket < 0)
		return NULL;
	object = kmem_cache_alloc(store->caches[bucket], gfp);
	if (!object)
		return NULL;
	atomic_long_inc(&store->objects[bucket]);
	*alloc_size = f2fs_nc_bucket_sizes[bucket];
	return object;
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
	/* Per-mount workspace used only by the cache thread. */
	void *workmem;
	void *scratch;
	struct f2fs_cached_block **candidates;
	spinlock_t config_lock;		/* protect config and next deadline */
	struct f2fs_nc_config config;
	/* Compressed queues only; raw entries use NODE_CACHE()->lru_list. */
	struct list_head queues[F2FS_NC_NR_QUEUES - 1];
	/* Current population and compressed bytes by queue. */
	atomic_long_t attached[F2FS_NC_NR_QUEUES];
	atomic64_t attached_payload[F2FS_NC_NR_QUEUES - 1];
	atomic64_t attached_slot_bytes[F2FS_NC_NR_QUEUES - 1];
	/* Cumulative reclaim, detach, restore and worker statistics. */
	atomic64_t shrink_scanned[F2FS_NC_NR_QUEUES];
	atomic64_t shrink_freed[F2FS_NC_NR_QUEUES];
	atomic64_t detached[F2FS_NC_NR_QUEUES];
	atomic64_t compressed_to_raw;
	atomic64_t restored_hits;
	atomic64_t restore_failures;
	atomic64_t cycles;
	atomic64_t visited;
	atomic64_t candidate_count;
	atomic64_t attempts;
	atomic64_t converted;
	atomic64_t candidate_refs;
	atomic64_t transient_extra_bytes;
	atomic64_t allocation_failures;
	/* Absolute CLOCK_MONOTONIC deadline; U64_MAX disables scheduling. */
	u64 next_compress_ms;
	/* Signed quota history carried between shrinker calls. */
	s64 shrink_credit[F2FS_NC_NR_QUEUES];
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
	WARN_ON_ONCE(atomic64_read(&ctx->candidate_refs));
	WARN_ON_ONCE(atomic64_read(&ctx->transient_extra_bytes));
	kfree(ctx->candidates);
	kfree(ctx->scratch);
	kfree(ctx->workmem);
	f2fs_nc_store_destroy(ctx->store);
	kfree(ctx);
}

static void f2fs_nc_ctx_put(struct f2fs_nc_ctx *ctx)
{
	if (refcount_dec_and_test(&ctx->refs))
		f2fs_nc_ctx_release(ctx);
}

static void f2fs_nc_ctx_get(struct f2fs_nc_ctx *ctx)
{
	refcount_inc(&ctx->refs);
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
	ctx->workmem = kmalloc(LZ4_MEM_COMPRESS, GFP_NOFS);
	if (!ctx->workmem)
		goto fail_open;
	ctx->scratch = kmalloc(F2FS_NC_MAX_OBJECT_SIZE, GFP_NOFS);
	if (!ctx->scratch)
		goto fail_open;
	ctx->candidates = kcalloc(F2FS_NC_MAX_CANDIDATES,
				  sizeof(*ctx->candidates), GFP_NOFS);
	if (!ctx->candidates)
		goto fail_open;
	spin_lock_init(&ctx->config_lock);
	f2fs_nc_config_defaults(&ctx->config);
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

static void f2fs_nc_config_snapshot(struct f2fs_nc_ctx *ctx,
				    struct f2fs_nc_config *cfg)
{
	unsigned long flags;

	spin_lock_irqsave(&ctx->config_lock, flags);
	*cfg = ctx->config;
	spin_unlock_irqrestore(&ctx->config_lock, flags);
}

static u64 f2fs_nc_compress_deadline(const struct f2fs_nc_config *cfg,
				     u64 now_ms)
{
	if (!cfg->compression_threshold_pct)
		return U64_MAX;
	return now_ms + cfg->compression_interval_ms;
}

u64 f2fs_nc_config_value(struct f2fs_sb_info *sbi, enum f2fs_nc_param id)
{
	struct f2fs_nc_ctx *ctx = sbi->node_compress;
	struct f2fs_nc_config cfg;

	if (!ctx) {
		f2fs_nc_config_defaults(&cfg);
		return f2fs_nc_config_get(&cfg, id);
	}
	f2fs_nc_config_snapshot(ctx, &cfg);
	return f2fs_nc_config_get(&cfg, id);
}

int f2fs_nc_config_update(struct f2fs_sb_info *sbi,
			  enum f2fs_nc_param id, u64 value)
{
	struct f2fs_nc_ctx *ctx = sbi->node_compress;
	struct f2fs_nc_config old, new;
	unsigned long flags;
	int ret;

	if (!ctx) {
		f2fs_nc_config_defaults(&old);
		ret = f2fs_nc_config_set(&new, &old, id, value);
		if (ret)
			return ret;
		return f2fs_nc_config_get(&old, id) == value ? 0 : -EOPNOTSUPP;
	}

	spin_lock_irqsave(&ctx->config_lock, flags);
	old = ctx->config;
	ret = f2fs_nc_config_set(&new, &old, id, value);
	if (ret)
		goto out_unlock;
	ctx->config = new;
	ctx->next_compress_ms =
		f2fs_nc_compress_deadline(&new, ktime_to_ms(ktime_get()));
out_unlock:
	spin_unlock_irqrestore(&ctx->config_lock, flags);
	return ret;
}

u64 f2fs_nc_next_deadline(struct f2fs_sb_info *sbi)
{
	struct f2fs_nc_ctx *ctx = sbi->node_compress;
	unsigned long flags;
	u64 deadline;

	if (!ctx)
		return U64_MAX;
	spin_lock_irqsave(&ctx->config_lock, flags);
	deadline = ctx->next_compress_ms;
	spin_unlock_irqrestore(&ctx->config_lock, flags);
	return deadline;
}

void f2fs_nc_schedule_start(struct f2fs_sb_info *sbi, u64 now_ms)
{
	struct f2fs_nc_ctx *ctx = sbi->node_compress;
	unsigned long flags;

	if (!ctx)
		return;
	spin_lock_irqsave(&ctx->config_lock, flags);
	ctx->next_compress_ms = f2fs_nc_compress_deadline(&ctx->config, now_ms);
	spin_unlock_irqrestore(&ctx->config_lock, flags);
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

static int
f2fs_nc_publish_raw(struct f2fs_cached_block *entry,
		    struct f2fs_node_cached_block *node,
		    struct f2fs_nc_ctx *ctx, void *raw, bool restored)
{
	struct f2fs_cached_block_list *cache = entry->cache;
	void *object = entry->data;
	u32 len = node->compressed_len;
	u32 alloc_size = node->compressed_alloc_size;
	unsigned int queue = f2fs_nc_entry_queue(entry);

	spin_lock(&cache->list_lock);
	entry->data = raw;
	f2fs_cache_clear_compressed(entry);
	node->owner = NULL;
	node->compressed_len = 0;
	node->compressed_alloc_size = 0;
	node->compressed_crc = 0;
	if (restored) {
		f2fs_cache_set_restored(entry);
		f2fs_cache_set_uptodate(entry);
	} else {
		f2fs_cache_clear_restored(entry);
		f2fs_cache_clear_uptodate(entry);
	}
	list_move_tail(&entry->list, &cache->lru_list);
	f2fs_nc_account_del(ctx, queue, len, alloc_size);
	f2fs_nc_account_add(ctx, F2FS_NC_RAW, 0, 0);
	atomic64_inc(&ctx->compressed_to_raw);
	spin_unlock(&cache->list_lock);

	f2fs_nc_store_free(ctx->store, object, alloc_size);
	f2fs_nc_ctx_put(ctx);
	return 0;
}

static int
f2fs_nc_restore_fallback(struct f2fs_cached_block *entry,
			 struct f2fs_node_cached_block *node,
			 struct f2fs_nc_ctx *ctx)
{
	void *raw;

	raw = f2fs_kmalloc(ctx->sbi, ctx->sbi->blocksize, GFP_NOFS);
	if (!raw)
		return -ENOMEM;
	atomic64_inc(&ctx->restore_failures);
	return f2fs_nc_publish_raw(entry, node, ctx, raw, false);
}

int f2fs_nc_restore(struct f2fs_cached_block *entry)
{
	struct f2fs_node_cached_block *node;
	struct f2fs_nc_ctx *ctx;
	struct f2fs_sb_info *sbi;
	void *raw;
	int ret;

	if (!entry || !entry->cache || !IS_NODE_CACHE(entry->cache) ||
	    !f2fs_cache_test_compressed(entry))
		return 0;
	if (WARN_ON_ONCE(!f2fs_cache_test_locked(entry)))
		return -EFSCORRUPTED;
	if (WARN_ON_ONCE(f2fs_cache_test_dirty(entry) ||
			 f2fs_cache_test_writeback(entry)))
		return -EFSCORRUPTED;

	node = f2fs_nc_node_entry(entry);
	ctx = node->owner;
	if (!ctx || ctx != entry->cache->sbi->node_compress || !entry->data)
		return -EFSCORRUPTED;
	sbi = ctx->sbi;
	if (f2fs_nc_store_bucket_from_size(node->compressed_alloc_size) < 0)
		return -EFSCORRUPTED;
	if (!node->compressed_len ||
	    node->compressed_len > node->compressed_alloc_size)
		return f2fs_nc_restore_fallback(entry, node, ctx);

	raw = f2fs_kmalloc(sbi, sbi->blocksize, GFP_NOFS);
	if (!raw)
		return -ENOMEM;
	ret = LZ4_decompress_safe(entry->data, raw, node->compressed_len,
				  sbi->blocksize);
	if (ret != sbi->blocksize ||
	    f2fs_crc32(raw, sbi->blocksize) != node->compressed_crc) {
		atomic64_inc(&ctx->restore_failures);
		return f2fs_nc_publish_raw(entry, node, ctx, raw, false);
	}
	return f2fs_nc_publish_raw(entry, node, ctx, raw, true);
}

void f2fs_nc_validation_failed(struct f2fs_cached_block *entry)
{
	struct f2fs_nc_ctx *ctx;

	if (f2fs_cache_test_restored(entry) && entry->cache) {
		ctx = entry->cache->sbi->node_compress;
		if (ctx)
			atomic64_inc(&ctx->restore_failures);
	}
	f2fs_cache_clear_restored(entry);
	f2fs_cache_clear_uptodate(entry);
}

void f2fs_nc_validation_succeeded(struct f2fs_cached_block *entry)
{
	struct f2fs_nc_ctx *ctx;

	if (f2fs_cache_test_restored(entry) && entry->cache) {
		ctx = entry->cache->sbi->node_compress;
		if (ctx)
			atomic64_inc(&ctx->restored_hits);
	}
	f2fs_cache_clear_restored(entry);
}

void f2fs_nc_content_changed(struct f2fs_cached_block *entry)
{
	if (!entry || !entry->cache || !IS_NODE_CACHE(entry->cache))
		return;
	WARN_ON_ONCE(f2fs_cache_test_compressed(entry));
	f2fs_cache_clear_restored(entry);
	f2fs_cache_clear_incompressible(entry);
}

static bool f2fs_nc_candidate(struct f2fs_cached_block *entry,
			      struct f2fs_cached_block_list *cache, int refs)
{
	return entry->cache == cache && entry->data &&
		!f2fs_cache_test_compressed(entry) &&
		f2fs_cache_test_uptodate(entry) &&
		!f2fs_cache_test_dirty(entry) &&
		!f2fs_cache_test_writeback(entry) &&
		!f2fs_cache_test_locked(entry) &&
		!f2fs_cache_test_referenced(entry) &&
		!f2fs_cache_test_incompressible(entry) &&
		atomic_read(&entry->refcount) == refs;
}

enum f2fs_nc_compress_result {
	F2FS_NC_COMPRESS_SKIPPED,
	F2FS_NC_COMPRESS_CONVERTED,
	F2FS_NC_COMPRESS_NO_MEMORY,
};

static enum f2fs_nc_compress_result
f2fs_nc_compress(struct f2fs_cached_block *entry,
		 const struct f2fs_nc_config *cfg)
{
	struct f2fs_cached_block_list *cache = entry->cache;
	struct f2fs_node_cached_block *node;
	struct f2fs_nc_ctx *ctx;
	struct f2fs_sb_info *sbi;
	unsigned long flags;
	unsigned int max_len;
	void *object;
	void *raw;
	u32 alloc_size;
	u32 crc;
	int bucket;
	int len;

	if (!cache || !IS_NODE_CACHE(cache) || !cfg ||
	    f2fs_cache_test_compressed(entry))
		return F2FS_NC_COMPRESS_SKIPPED;
	if (WARN_ON_ONCE(!f2fs_cache_test_locked(entry)))
		return F2FS_NC_COMPRESS_SKIPPED;
	if (!f2fs_cache_test_uptodate(entry) || f2fs_cache_test_dirty(entry) ||
	    f2fs_cache_test_writeback(entry) ||
	    f2fs_cache_test_referenced(entry) ||
	    f2fs_cache_test_incompressible(entry) || !entry->data ||
	    atomic_read(&entry->refcount) != 2)
		return F2FS_NC_COMPRESS_SKIPPED;

	sbi = cache->sbi;
	ctx = sbi->node_compress;
	if (!ctx || !cfg->compression_threshold_pct)
		return F2FS_NC_COMPRESS_SKIPPED;
	max_len = min_t(u64,
			(u64)sbi->blocksize * cfg->compression_threshold_pct /
			F2FS_NC_PERCENT_MAX,
			F2FS_NC_MAX_OBJECT_SIZE);
	if (!max_len)
		return F2FS_NC_COMPRESS_SKIPPED;

	atomic64_inc(&ctx->attempts);
	len = LZ4_compress_default(entry->data, ctx->scratch, sbi->blocksize,
				   max_len, ctx->workmem);
	if (len <= 0) {
		if (max_len == F2FS_NC_MAX_OBJECT_SIZE)
			f2fs_cache_set_incompressible(entry);
		return F2FS_NC_COMPRESS_SKIPPED;
	}
	object = f2fs_nc_store_alloc(ctx->store, len, GFP_NOFS, &alloc_size);
	if (!object) {
		atomic64_inc(&ctx->allocation_failures);
		return F2FS_NC_COMPRESS_NO_MEMORY;
	}
	bucket = f2fs_nc_store_bucket_from_size(alloc_size);
	if (WARN_ON_ONCE(bucket < 0)) {
		f2fs_nc_store_free(ctx->store, object, alloc_size);
		return F2FS_NC_COMPRESS_SKIPPED;
	}
	/*
	 * Before publication only the new slot is extra.  After publication,
	 * the old raw block remains the extra allocation until it is freed.
	 */
	atomic64_add(alloc_size, &ctx->transient_extra_bytes);
	memcpy(object, ctx->scratch, len);
	raw = entry->data;
	crc = f2fs_crc32(raw, sbi->blocksize);

	spin_lock(&cache->list_lock);
	spin_lock_irqsave(&cache->tree_lock, flags);
	if (entry->cache != cache || entry->data != raw ||
	    atomic_read(&entry->refcount) != 2 || f2fs_cp_error(sbi) ||
	    f2fs_cache_test_referenced(entry) ||
	    !f2fs_cache_test_uptodate(entry) || f2fs_cache_test_dirty(entry) ||
	    f2fs_cache_test_writeback(entry) ||
	    f2fs_cache_test_incompressible(entry)) {
		spin_unlock_irqrestore(&cache->tree_lock, flags);
		spin_unlock(&cache->list_lock);
		f2fs_nc_store_free(ctx->store, object, alloc_size);
		atomic64_sub(alloc_size, &ctx->transient_extra_bytes);
		return F2FS_NC_COMPRESS_SKIPPED;
	}
	node = f2fs_nc_node_entry(entry);
	f2fs_nc_ctx_get(ctx);
	node->owner = ctx;
	node->compressed_len = len;
	node->compressed_alloc_size = alloc_size;
	node->compressed_crc = crc;
	entry->data = object;
	f2fs_cache_set_compressed(entry);
	list_move_tail(&entry->list, &ctx->queues[bucket]);
	f2fs_nc_account_del(ctx, F2FS_NC_RAW, 0, 0);
	f2fs_nc_account_add(ctx, bucket + 1, len, alloc_size);
	atomic64_add((s64)sbi->blocksize - alloc_size,
		     &ctx->transient_extra_bytes);
	spin_unlock_irqrestore(&cache->tree_lock, flags);
	spin_unlock(&cache->list_lock);

	kfree(raw);
	atomic64_sub(sbi->blocksize, &ctx->transient_extra_bytes);
	atomic64_inc(&ctx->converted);
	return F2FS_NC_COMPRESS_CONVERTED;
}

void f2fs_nc_run(struct f2fs_sb_info *sbi)
{
	struct f2fs_nc_ctx *ctx = sbi->node_compress;
	struct f2fs_cached_block_list *cache = NODE_CACHE(sbi);
	struct f2fs_nc_config cfg;
	struct f2fs_cached_block *entry;
	unsigned long flags;
	u64 now_ms;
	unsigned long raw_population;
	unsigned int goal, visited = 0, count = 0, i;
	bool allocation_failed = false;

	if (!ctx)
		return;
	f2fs_nc_config_snapshot(ctx, &cfg);
	if (!cfg.compression_threshold_pct || f2fs_cp_error(sbi) ||
	    f2fs_readonly(sbi->sb))
		goto out;
	/* Count enabled worker passes, including passes with no candidates. */
	atomic64_inc(&ctx->cycles);
	raw_population = atomic_long_read(&ctx->attached[F2FS_NC_RAW]);
	goal = min_t(unsigned long, raw_population, F2FS_NC_SCAN_MAX);
	if (!goal)
		goto out;

	spin_lock(&cache->list_lock);
	list_for_each_entry(entry, &cache->lru_list, list) {
		if (visited == goal || count == F2FS_NC_MAX_CANDIDATES)
			break;
		visited++;
		if (!f2fs_nc_candidate(entry, cache, 1))
			continue;
		f2fs_cache_get(entry);
		ctx->candidates[count++] = entry;
		atomic64_inc(&ctx->candidate_refs);
	}
	spin_unlock(&cache->list_lock);
	atomic64_add(visited, &ctx->visited);
	atomic64_add(count, &ctx->candidate_count);

	for (i = 0; i < count; i++) {
		enum f2fs_nc_compress_result compress_result =
			F2FS_NC_COMPRESS_SKIPPED;

		entry = ctx->candidates[i];
		if (f2fs_trylock_cache(entry)) {
			compress_result = f2fs_nc_compress(entry, &cfg);
			f2fs_unlock_cache(entry);
		}
		f2fs_put_cache(entry, false);
		atomic64_dec(&ctx->candidate_refs);
		ctx->candidates[i] = NULL;
		switch (compress_result) {
		case F2FS_NC_COMPRESS_CONVERTED:
			break;
		case F2FS_NC_COMPRESS_NO_MEMORY:
			allocation_failed = true;
			i++;
			goto put_remaining;
		case F2FS_NC_COMPRESS_SKIPPED:
			break;
		}
		if ((i + 1) % F2FS_NC_PROCESS_BATCH == 0)
			cond_resched();
	}
put_remaining:
	for (; i < count; i++) {
		f2fs_put_cache(ctx->candidates[i], false);
		atomic64_dec(&ctx->candidate_refs);
		ctx->candidates[i] = NULL;
	}
out:
	now_ms = ktime_to_ms(ktime_get());
	spin_lock_irqsave(&ctx->config_lock, flags);
	if (allocation_failed && ctx->config.compression_threshold_pct)
		WRITE_ONCE(ctx->next_compress_ms, now_ms +
			   max_t(u32, ctx->config.compression_interval_ms,
				 F2FS_NC_ALLOC_BACKOFF_MS));
	else
		WRITE_ONCE(ctx->next_compress_ms,
			   f2fs_nc_compress_deadline(&ctx->config, now_ms));
	spin_unlock_irqrestore(&ctx->config_lock, flags);
}

static void f2fs_nc_population_snapshot(struct f2fs_nc_ctx *ctx,
					unsigned long nr[F2FS_NC_NR_QUEUES])
{
	struct f2fs_cached_block_list *cache = NODE_CACHE(ctx->sbi);
	unsigned int i;

	spin_lock(&cache->list_lock);
	for (i = 0; i < F2FS_NC_NR_QUEUES; i++)
		nr[i] = atomic_long_read(&ctx->attached[i]);
	spin_unlock(&cache->list_lock);
}

unsigned long f2fs_nc_count_nodes(struct f2fs_sb_info *sbi)
{
	struct f2fs_nc_ctx *ctx = sbi->node_compress;
	unsigned long nr[F2FS_NC_NR_QUEUES];

	if (!ctx)
		return NODE_CACHE(sbi)->num_entries;
	f2fs_nc_population_snapshot(ctx, nr);
	return f2fs_nc_effective_count(nr, sbi->blocksize);
}

unsigned long f2fs_nc_shrink_nodes(struct f2fs_sb_info *sbi,
				   unsigned long nr_to_scan)
{
	struct f2fs_cached_block_list *cache = NODE_CACHE(sbi);
	struct f2fs_nc_ctx *ctx = sbi->node_compress;
	unsigned long nr[F2FS_NC_NR_QUEUES];
	unsigned long quota[F2FS_NC_NR_QUEUES];
	unsigned long freed = 0;
	unsigned int i;

	if (!ctx)
		return f2fs_shrink_cache_list(cache, &cache->lru_list,
				ULONG_MAX, nr_to_scan, NULL);
	f2fs_nc_population_snapshot(ctx, nr);
	f2fs_nc_scan_quotas(nr, sbi->blocksize, nr_to_scan,
			    ctx->shrink_credit, quota);
	for (i = 0; i < F2FS_NC_NR_QUEUES; i++) {
		struct list_head *head;
		unsigned long scanned = 0;
		unsigned long queue_freed;

		if (!quota[i])
			continue;
		head = f2fs_nc_queue_head(cache, i);
		if (!head)
			continue;
		queue_freed = f2fs_shrink_cache_list(cache, head, nr[i], quota[i],
						     &scanned);
		freed += queue_freed;
		atomic64_add(scanned, &ctx->shrink_scanned[i]);
		atomic64_add(queue_freed, &ctx->shrink_freed[i]);
	}
	return freed;
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

void f2fs_nc_get_stats(struct f2fs_sb_info *sbi,
		       struct f2fs_nc_stats *stats)
{
	struct f2fs_nc_ctx *ctx = sbi->node_compress;
	struct f2fs_nc_config cfg;
	unsigned int i;

	memset(stats, 0, sizeof(*stats));
	if (!ctx)
		return;
	f2fs_nc_config_snapshot(ctx, &cfg);
	stats->initialized = true;
	stats->threshold_pct = cfg.compression_threshold_pct;
	stats->interval_ms = cfg.compression_interval_ms;
	for (i = 0; i < F2FS_NC_NR_QUEUES; i++) {
		stats->attached[i] = atomic_long_read(&ctx->attached[i]);
		stats->shrink_scanned[i] = atomic64_read(&ctx->shrink_scanned[i]);
		stats->shrink_freed[i] = atomic64_read(&ctx->shrink_freed[i]);
		stats->detached[i] = atomic64_read(&ctx->detached[i]);
		if (i != F2FS_NC_RAW) {
			stats->payload[i - 1] =
				atomic64_read(&ctx->attached_payload[i - 1]);
			stats->slots[i - 1] =
				atomic64_read(&ctx->attached_slot_bytes[i - 1]);
		}
	}
	stats->compressed_to_raw = atomic64_read(&ctx->compressed_to_raw);
	stats->restored_hits = atomic64_read(&ctx->restored_hits);
	stats->restore_failures = atomic64_read(&ctx->restore_failures);
	stats->cycles = atomic64_read(&ctx->cycles);
	stats->visited = atomic64_read(&ctx->visited);
	stats->candidates = atomic64_read(&ctx->candidate_count);
	stats->attempts = atomic64_read(&ctx->attempts);
	stats->converted = atomic64_read(&ctx->converted);
	stats->candidate_refs = atomic64_read(&ctx->candidate_refs);
	stats->transient_extra_bytes =
		atomic64_read(&ctx->transient_extra_bytes);
	stats->allocation_failures = atomic64_read(&ctx->allocation_failures);
}
