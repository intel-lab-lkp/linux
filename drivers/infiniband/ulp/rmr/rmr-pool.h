/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#ifndef RMR_POOL_H
#define RMR_POOL_H

#include <linux/limits.h>	/* for NAME_MAX */
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/jhash.h>	/* for jhash() */
#include <linux/kernel.h>	/* for round_up */
#include "rmr.h"
#include "rmr-map.h"

#define RMR_POOL_MD_MAGIC 0xDEADBEEF
#define XA_TRUE  ((void *)1UL)
#define XA_FALSE ((void *)2UL)

extern struct kmem_cache *rmr_map_entry_cachep;
/*
 * enum srv_sync_thread_state
 */
enum srv_sync_thread_state {
	SYNC_THREAD_REQ_STOP,	/* 0 */
	SYNC_THREAD_STOPPED,
	SYNC_THREAD_RUNNING,
	SYNC_THREAD_WAIT,
};

enum srv_map_update_state {
	MAP_UPDATE_STATE_DISABLED,
	MAP_UPDATE_STATE_READY,
	MAP_UPDATE_STATE_DONE,
};

/* The srv pool specific structure */
struct rmr_srv_md {
	u64			map_ver;
	u64			mapped_size;		/* server store size in sectors */
	u8			member_id;
	u8			srv_pool_state;		/* server pool state */
	u8			store_state;		/* state of io_store */
	u8			map_update_state;
	bool			discard_entries;
};

/* Shared by each pool */
struct rmr_pool_md {
	char			poolname[NAME_MAX];
	u64			magic;
	u32			group_id;
	u32			chunk_size;		/* rmr client */
	u64			mapped_size;		/* client view of store size */
	u32			queue_depth;
	u64			map_ver;
	struct rmr_srv_md	srv_md[RMR_POOL_MAX_SESS];
} __packed;

struct rmr_pool {
	char			poolname[NAME_MAX];
	u32			group_id;	/* jhash() on poolname */
	struct kobject		kobj;
	struct kobject		sessions_kobj;
	struct list_head	entry;		/* for global pool_list */

	struct list_head	sess_list;	/* list of sessions */
	struct mutex		sess_lock;	/* protect list of sessions */
	struct srcu_struct	sess_list_srcu;

	void			*priv;
	u64			mapped_size;
	u32 			chunk_size;
	u8			chunk_size_shift;
	u64			no_of_chunks;

	struct percpu_ref       ids_inflight_ref;
	struct completion       complete_done;
	struct completion       confirm_done;

	struct completion	discard_done; /* for sync client pool */
	/* Set when waiting for response of discard request */
	atomic_t		discard_waiting;

	u8                      maps_cnt;
	struct mutex		maps_lock;
	struct rmr_dirty_id_map __rcu
				*maps[RMR_POOL_MAX_SESS];
	/* All member ids of the storage nodes */
	struct xarray		stg_members;
	u64			map_ver;
	atomic_t		normal_count; /* number of pool sessions currently in NORMAL state */
	struct srcu_struct	map_srcu;

	struct rmr_pool_md	pool_md;

	bool is_clt;
	bool sync;
};

/**
 * rmr_pool_find_md - find the index of the srv_md with the provided key in the pool_md
 *
 * @pool_md: the pool_md to search
 * @key: the member_id of the server pool to search for
 * @empty_slot: the empty slot is required by caller or not
 *
 * Description:
 *	Find the index of the srv_md with the matched key. If there is no such a key and the empty
 *	slot is not required, return -1.
 *
 * Return:
 *	>= 0, the index of the key in the pool_md. Return the index of an empty slot when the key
 *	is not found and the empty_slot flag is true
 *	-1 if the key is not found and empty_slot is false, or the pool_md doesn't exist
 */
static inline int rmr_pool_find_md(struct rmr_pool_md *pool_md, u8 key, bool empty_slot)
{
	int i;
	int empty_i = -1;

	if (!pool_md)
		return -1;

	for (i = 0; i < RMR_POOL_MAX_SESS; i++) {
		if (!pool_md->srv_md[i].member_id)
			empty_i = i;

		if (pool_md->srv_md[i].member_id == key)
			return i;
	}

	if (empty_slot)
		return empty_i;
	return -1;
}

/**
 * rmr_pool_md_check_discard - check the discard_entries flag of the srv_md
 *
 * @pool: the pool to check pool_md
 * @member_id: the member_id of the srv_md to check
 *
 * Description:
 *	Check if the pool has received the discards from the server pool with the provided
 *	member_id.
 *
 * Return:
 *	1 (true) if the pool has received the discards,
 *	0 (false) if the pool has not received the discards,
 *	<0 if the pool has no info of the server pool
 */
static inline int rmr_pool_md_check_discard(struct rmr_pool *pool, u8 member_id)
{
	int md_i = rmr_pool_find_md(&pool->pool_md, member_id, false);

	if (md_i < 0) {
		pr_err("Failed to find md for member_id %u\n", member_id);
		return -EINVAL;
	}

	/* If the flag is set, this pool has received the discards. */
	return pool->pool_md.srv_md[md_i].discard_entries;
}

#define RMR_MAP_FORMAT_VER 1
/*
 * Get the first most significant bit of map_ver. If it is one, then the store of that storage node
 * is being replaced.
 */
#define RMR_STORE_IS_REPLACE(map_ver) (map_ver >> 63 & 1ULL)
#define RMR_STORE_GET_VER(map_ver) (map_ver & ~(1ULL << 63))
#define RMR_STORE_SET_REPLACE(map_ver) (map_ver |= 1ULL << 63)
#define RMR_STORE_UNSET_REPLACE(map_ver) (map_ver &= ~(1ULL << 63))
#define RTRS_IO_LIMIT	   102400
//#define RTRS_IO_LIMIT 40 //for tests only

/*
 * TODO:
 * We currently do not have mapped_size while creating dirty maps,
 * which means we cannot calculate no_of_chunks, hence cannot allocate bitmap
 * So, as a workaround, we allocate max size bitmap,
 * and to reduce that allocation, we cap max mapped_size.
 *
 * 1GB max mapped size for now.
 * (Size mentioned in number of sectors, just like nr_sects)
 */
#define RMR_MAX_MAPPED_SIZE    2097152

/* The header structure of rmr pool metadata will not over this limit. */
#define RMR_MD_SIZE		PAGE_SIZE
#define RMR_MD_SIZE_SECTORS	(PAGE_SIZE / SECTOR_SIZE)
#define RMR_MAP_BUF_HDR_SIZE    PAGE_SIZE
#define RMR_SRV_MD_SIZE		(sizeof(struct rmr_srv_md) * RMR_POOL_MAX_SESS)
#define RMR_CLT_MD_SIZE		(sizeof(struct rmr_pool_md) - RMR_SRV_MD_SIZE)
#define RMR_SECTOR_SIZE		512
#define RMR_INT_ROUND_UP(x, y)	(((x) + (y) - 1) / (y))
#define RMR_ROUND_UP(x)		round_up(x, RMR_SECTOR_SIZE)

#define RMR_SRV_MAX_QDEPTH	512

/* last_io region starts right after the pool_md header page */
#define RMR_LAST_IO_OFFSET	RMR_MD_SIZE

static inline u64 rmr_last_io_len(u32 queue_depth)
{
	return RMR_ROUND_UP((u64)queue_depth * sizeof(rmr_id_t));
}

static inline u64 rmr_bitmap_offset(u32 queue_depth)
{
	return RMR_LAST_IO_OFFSET + rmr_last_io_len(queue_depth);
}

static inline u64 rmr_per_map_bitmap_size(u64 no_of_chunks)
{
	return DIV_ROUND_UP(no_of_chunks, CHUNKS_PER_SLP) * PAGE_SIZE;
}

static inline u64 rmr_bitmap_len(u64 no_of_chunks)
{
	return RMR_POOL_MAX_SESS * rmr_per_map_bitmap_size(no_of_chunks);
}

struct rmr_map_buf_hdr {
	u64 version;
	u64 member_id;

	/*
	 * dst_slp_idx: SLP index in the local dirty map buffer,
	 * from where to write the recved dirty map buffer
	 */
	u64 dst_slp_idx;
	u32 buf_size;

	/*
	 * slp_idx: Only used for MAP_READ,
	 * to let client know where to ask from in the next iteration
	 */
	u64 map_idx;
	u64 slp_idx;
} __packed;

extern struct list_head pool_list;
extern struct mutex pool_mutex;

const char *rmr_get_cmd_name(enum rmr_msg_cmd_type cmd);

struct rmr_pool *rmr_create_pool(const char *poolname, void *priv);
void free_pool(struct rmr_pool *pool);

struct rmr_pool *rmr_find_pool_by_group_id(u32 group_id);
struct rmr_pool *rmr_find_pool(const char *poolname);
int rmr_pool_maps_to_buf(struct rmr_pool *pool, u8 *map_idx, u64 *slp_idx,
			 void *buf, size_t buflen, rmr_map_filter filter);
int rmr_pool_save_map(struct rmr_pool *pool, void *buf, size_t buflen,
		      bool test_only);

static inline void rmr_pool_update_no_of_chunk(struct rmr_pool *pool)
{
	u64 calc_no_of_chunks = 0, old_no_of_chunks = pool->no_of_chunks;

	/*
	 * In include/linux/types.h
	 *
	 * "Linux always considers sectors to be 512 (SECTOR_SHIFT==9) bytes long independently
	 * of the devices real block size."
	 *
	 * mapped_size is saved in sectors.
	 */
	if (pool->mapped_size) {
		calc_no_of_chunks = (pool->mapped_size >> (pool->chunk_size_shift - 9));

		if (pool->chunk_size &&
		    (pool->mapped_size << 9) % pool->chunk_size)
			calc_no_of_chunks += 1;
	}

	if (calc_no_of_chunks != pool->no_of_chunks) {
		pool->no_of_chunks = calc_no_of_chunks;
		pr_info("%s: For %s, no_of_chunks old (%llu), updated %llu\n",
			__func__, pool->poolname, old_no_of_chunks, pool->no_of_chunks);
	}
}

/*
 * rmr_pool_maps_append - Append a map to the dense maps array
 * @pool: pool
 * @map: map to add
 *
 * Context: Caller must hold maps_lock.
 */
static inline void rmr_pool_maps_append(struct rmr_pool *pool,
					struct rmr_dirty_id_map *map)
{
	rcu_assign_pointer(pool->maps[pool->maps_cnt], map);
	pool->maps_cnt++;
}

/*
 * rmr_pool_maps_swap_remove - Remove map at index @i using swap-with-last
 * @pool: pool
 * @i: index of the map in the map array to remove
 * @map: the map being removed
 *
 * Description:
 *      Maintains the dense invariant: pool->maps[0:maps_cnt] has no NULL gaps.
 *
 * Context: Caller must hold maps_lock.
 */
static inline void rmr_pool_maps_swap_remove(struct rmr_pool *pool, u8 i,
					     struct rmr_dirty_id_map *map)
{
	u8 last = pool->maps_cnt - 1;

	if (i != last)
		rcu_assign_pointer(pool->maps[i], rcu_dereference_protected(pool->maps[last],
					lockdep_is_held(&pool->maps_lock)));

	rcu_assign_pointer(pool->maps[last], NULL);
	pool->maps_cnt--;
}

static inline struct rmr_dirty_id_map *rmr_pool_find_map(struct rmr_pool *pool, u8 member_id)
{
	int i;
	struct rmr_dirty_id_map *map;
	struct rmr_dirty_id_map *res = NULL;

	rcu_read_lock();
	for (i = 0; i < pool->maps_cnt; i++) {
		map = rcu_dereference(pool->maps[i]);

		if (WARN_ON(!map) || map->member_id != member_id)
			continue;

		res = map;
		break;
	}
	rcu_read_unlock();

	return res;
}

static inline int rmr_pool_remove_map(struct rmr_pool *pool, u8 member_id)
{
	int i;
	struct rmr_dirty_id_map *mp;
	struct rmr_dirty_id_map *map = NULL;

	pr_info("%s: pool %s is removing map for member_id %d\n",
		__func__, pool->poolname, member_id);

	mutex_lock(&pool->maps_lock);
	for (i = 0; i < pool->maps_cnt; i++) {
		mp = rcu_dereference_protected(pool->maps[i],
				lockdep_is_held(&pool->maps_lock));
		if (WARN_ON(!mp))
			continue;
		if (mp->member_id == member_id) {
			map = mp;
			break;
		}
	}

	if (!map) {
		mutex_unlock(&pool->maps_lock);
		pr_err("%s: pool %s cannot find map for member_id %d\n",
		       __func__, pool->poolname, member_id);
		return -EINVAL;
	}

	/* Dirty map entries are also removed since the map no longer exists. */
	rmr_map_unset_dirty_all(map);

	rmr_pool_maps_swap_remove(pool, i, map);
	synchronize_srcu(&pool->map_srcu);

	mutex_unlock(&pool->maps_lock);

	/* Free up the memory */
	rmr_map_destroy(map);

	return 0;
}


bool rmr_pool_change_state(struct rmr_pool *pool, enum rmr_pool_state new_state);

void rmr_pool_confirm_inflight_ref(struct percpu_ref *ref);

static inline u32 rmr_pool_hash(const char *poolname)
{
	return jhash(poolname, strlen(poolname), 0);
}

#endif /* RMR_POOL_H */
