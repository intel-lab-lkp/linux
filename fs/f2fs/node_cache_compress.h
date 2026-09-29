/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __F2FS_NODE_CACHE_COMPRESS_H__
#define __F2FS_NODE_CACHE_COMPRESS_H__

#include "cache.h"
#include "node_cache_policy.h"

struct f2fs_nc_ctx;

/* Attributed cache memory, excluding worker workspace and slab metadata. */
struct f2fs_nc_memory {
	u64 entry_bytes;	/* Cache-entry storage. */
	u64 data_bytes;	/* Raw buffers plus compressed slab slots. */
};

/* Snapshot used by debugfs; queue arrays use enum f2fs_nc_queue. */
struct f2fs_nc_stats {
	/* Current queue state; payload and slots start at F2FS_NC_256. */
	u64 attached[F2FS_NC_NR_QUEUES];
	u64 payload[F2FS_NC_NR_QUEUES - 1];
	u64 slots[F2FS_NC_NR_QUEUES - 1];
	/* Cumulative statistics since mount. */
	u64 shrink_scanned[F2FS_NC_NR_QUEUES];
	u64 shrink_freed[F2FS_NC_NR_QUEUES];
	u64 detached[F2FS_NC_NR_QUEUES];
	u64 compressed_to_raw;
	u64 restored_hits;
	u64 restore_failures;
	u64 cycles;
	u64 visited;
	u64 candidates;
	u64 attempts;
	u64 converted;
	u64 referenced_moved;
	u64 candidate_refs;		/* Worker-held references right now. */
	u64 transient_extra_bytes;	/* Temporary raw/compressed overlap. */
	u64 allocation_failures;
	/* Current configuration snapshot. */
	u32 threshold_pct;
	u32 interval_ms;
	bool initialized;		/* A compression context is available. */
};

#ifdef CONFIG_F2FS_FS_NODE_CACHE_COMPRESSION
/* Extended NODE_CACHE entry; base must remain the first member. */
struct f2fs_node_cached_block {
	struct f2fs_cached_block base;
	struct f2fs_nc_ctx *owner;	/* Context owning compressed data. */
	u32 compressed_len;		/* Valid compressed payload bytes. */
	u32 compressed_alloc_size;	/* Private-slab slot size. */
	u32 compressed_crc;		/* CRC of the original node block. */
};

static_assert(offsetof(struct f2fs_node_cached_block, base) == 0);

size_t f2fs_nc_entry_alloc_size(struct f2fs_cached_block_list *cache);
void f2fs_nc_init(struct f2fs_sb_info *sbi);
void f2fs_nc_destroy(struct f2fs_sb_info *sbi);
u64 f2fs_nc_next_deadline(struct f2fs_sb_info *sbi);
void f2fs_nc_schedule_start(struct f2fs_sb_info *sbi, u64 now_ms);
void f2fs_nc_run(struct f2fs_sb_info *sbi);
u64 f2fs_nc_config_value(struct f2fs_sb_info *sbi,
			 enum f2fs_nc_param id);
int f2fs_nc_config_update(struct f2fs_sb_info *sbi,
			  enum f2fs_nc_param id, u64 value);
void f2fs_nc_free_data(struct f2fs_cached_block *entry);
int f2fs_nc_restore(struct f2fs_cached_block *entry);
void f2fs_nc_validation_failed(struct f2fs_cached_block *entry);
void f2fs_nc_validation_succeeded(struct f2fs_cached_block *entry);
void f2fs_nc_content_changed(struct f2fs_cached_block *entry);
struct list_head *f2fs_nc_queue_head(struct f2fs_cached_block_list *cache,
				     unsigned int queue);
unsigned int f2fs_nc_entry_queue(const struct f2fs_cached_block *entry);
void f2fs_nc_entry_attached(struct f2fs_cached_block *entry);
void f2fs_nc_entry_detached(struct f2fs_cached_block *entry);
unsigned long f2fs_nc_count_nodes(struct f2fs_sb_info *sbi);
unsigned long f2fs_nc_shrink_nodes(struct f2fs_sb_info *sbi,
				   unsigned long nr_to_scan);
void f2fs_nc_memory_usage(struct f2fs_sb_info *sbi,
			  struct f2fs_nc_memory *memory);
void f2fs_nc_get_stats(struct f2fs_sb_info *sbi,
		       struct f2fs_nc_stats *stats);
#else
static inline size_t f2fs_nc_entry_alloc_size(struct f2fs_cached_block_list *cache)
{
	return sizeof(struct f2fs_cached_block);
}

static inline void f2fs_nc_init(struct f2fs_sb_info *sbi) { }

static inline void f2fs_nc_destroy(struct f2fs_sb_info *sbi) { }

static inline void f2fs_nc_free_data(struct f2fs_cached_block *entry)
{
	kfree(entry->data);
}

static inline int f2fs_nc_restore(struct f2fs_cached_block *entry)
{
	return 0;
}

static inline void f2fs_nc_validation_failed(struct f2fs_cached_block *entry) { }
static inline void f2fs_nc_validation_succeeded(struct f2fs_cached_block *entry) { }
static inline void f2fs_nc_content_changed(struct f2fs_cached_block *entry) { }

static inline struct list_head *f2fs_nc_queue_head(struct f2fs_cached_block_list *cache,
						   unsigned int queue)
{
	return queue == F2FS_NC_RAW ? &cache->lru_list : NULL;
}

static inline unsigned int
f2fs_nc_entry_queue(const struct f2fs_cached_block *entry)
{
	return F2FS_NC_RAW;
}

static inline void f2fs_nc_entry_attached(struct f2fs_cached_block *entry) { }
static inline void f2fs_nc_entry_detached(struct f2fs_cached_block *entry) { }

static inline unsigned long f2fs_nc_count_nodes(struct f2fs_sb_info *sbi)
{
	return NODE_CACHE(sbi)->num_entries;
}

static inline unsigned long f2fs_nc_shrink_nodes(struct f2fs_sb_info *sbi,
						 unsigned long nr_to_scan)
{
	return f2fs_shrink_cache_list(NODE_CACHE(sbi),
			&NODE_CACHE(sbi)->lru_list, ULONG_MAX, nr_to_scan, NULL);
}

static inline void f2fs_nc_memory_usage(struct f2fs_sb_info *sbi,
					struct f2fs_nc_memory *memory)
{
	memory->entry_bytes = (u64)NODE_CACHE(sbi)->num_entries *
			sizeof(struct f2fs_cached_block);
	memory->data_bytes = (u64)NODE_CACHE(sbi)->num_entries * sbi->blocksize;
}

#endif

#endif /* __F2FS_NODE_CACHE_COMPRESS_H__ */
