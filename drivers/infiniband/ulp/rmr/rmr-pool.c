// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include "rmr-pool.h"

LIST_HEAD(pool_list);
DEFINE_MUTEX(pool_mutex);	/* mutex to protect pool_list */
struct kmem_cache *rmr_map_entry_cachep;

const char *rmr_get_cmd_name(enum rmr_msg_cmd_type cmd)
{
	switch (cmd) {
	case RMR_CMD_MAP_READY: return "RMR_CMD_MAP_READY";
	case RMR_CMD_MAP_SEND: return "RMR_CMD_MAP_SEND";
	case RMR_CMD_SEND_MAP_BUF: return "RMR_CMD_SEND_MAP_BUF";
	case RMR_CMD_MAP_BUF_DONE: return "RMR_CMD_MAP_BUF_DONE";
	case RMR_CMD_MAP_DONE: return "RMR_CMD_MAP_DONE";
	case RMR_CMD_MAP_DISABLE: return "RMR_CMD_MAP_DISABLE";
	case RMR_CMD_READ_MAP_BUF: return "RMR_CMD_READ_MAP_BUF";
	case RMR_CMD_MAP_CHECK: return "RMR_CMD_MAP_CHECK";
	case RMR_CMD_LAST_IO_TO_MAP: return "RMR_CMD_LAST_IO_TO_MAP";
	case RMR_CMD_STORE_CHECK: return "RMR_CMD_STORE_CHECK";
	case RMR_CMD_MAP_TEST: return "RMR_CMD_MAP_TEST";
	case RMR_CMD_SEND_MD_BUF: return "RMR_CMD_SEND_MD_BUF";
	case RMR_CMD_MD_SEND: return "RMR_CMD_MD_SEND";

	case RMR_CMD_MAP_GET_VER: return "RMR_CMD_MAP_GET_VER";
	case RMR_CMD_MAP_SET_VER: return "RMR_CMD_MAP_SET_VER";
	case RMR_CMD_DISCARD_CLEAR_FLAG: return "RMR_CMD_DISCARD_CLEAR_FLAG";
	case RMR_CMD_SEND_DISCARD: return "RMR_CMD_SEND_DISCARD";

	case RMR_MAP_CMD_MAX: return "RMR_MAP_CMD_MAX";

	case RMR_CMD_POOL_INFO: return "RMR_CMD_POOL_INFO";
	case RMR_CMD_JOIN_POOL: return "RMR_CMD_JOIN_POOL";

	case RMR_CMD_REJOIN_POOL: return "RMR_CMD_REJOIN_POOL";

	case RMR_CMD_LEAVE_POOL: return "RMR_CMD_LEAVE_POOL";
	case RMR_CMD_ENABLE_POOL: return "RMR_CMD_ENABLE_POOL";

	case RMR_CMD_USER: return "RMR_CMD_USER";

	case RMR_POOL_CMD_MAX: return "RMR_POOL_CMD_MAX";

	default: return "Unknown command";
	}
}

void free_pool(struct rmr_pool *pool)
{
	WARN_ON(!list_empty(&pool->sess_list));

	cleanup_srcu_struct(&pool->sess_list_srcu);
	cleanup_srcu_struct(&pool->map_srcu);

	if (!list_empty(&pool->entry)) {
		mutex_lock(&pool_mutex);
		list_del(&pool->entry);
		mutex_unlock(&pool_mutex);
	}

	percpu_ref_exit(&pool->ids_inflight_ref);
	kfree(pool);
}

/**
 * rmr_find_pool_by_group_id - Find a pool with group_id in global pool list
 *
 * @group_id: Group_id of the pool being searched
 *
 * Locks:
 *    Caller should hold global pool_mutex
 */
struct rmr_pool *rmr_find_pool_by_group_id(u32 group_id)
{
	struct rmr_pool *pool;

	list_for_each_entry(pool, &pool_list, entry)
		if (pool->group_id == group_id)
			return pool;

	return NULL;
}

/**
 * rmr_find_pool - Find a pool named poolname in the global pool list
 *
 * @poolname: Name of the pool to be searched
 *
 * Locks:
 *    Caller must hold global pool_mutex
 */
struct rmr_pool *rmr_find_pool(const char *poolname)
{
	struct rmr_pool *pool;

	lockdep_assert_held(&pool_mutex);

	list_for_each_entry(pool, &pool_list, entry) {
		if (!strcmp(poolname, pool->poolname))
			return pool;
	}

	return NULL;
}

static void rmr_pool_inflight_ref_release(struct percpu_ref *ref)
{
	struct rmr_pool *pool = container_of(ref, struct rmr_pool, ids_inflight_ref);

	complete_all(&pool->complete_done);
}

void rmr_pool_confirm_inflight_ref(struct percpu_ref *ref)
{
	struct rmr_pool *pool = container_of(ref, struct rmr_pool, ids_inflight_ref);

	complete_all(&pool->confirm_done);
}

static struct rmr_pool *alloc_pool(const char *poolname, u32 group_id)
{
	struct rmr_pool *pool;
	int ret;

	pr_debug("%s: allocate pool %s with group_id %u\n",
		 __func__, poolname, group_id);

	if (strlen(poolname) > NAME_MAX) {
		pr_err("%s: Failed to create '%s': name too long\n", __func__, poolname);
		return ERR_PTR(-EINVAL);
	}

	pool = kzalloc(sizeof(struct rmr_pool), GFP_KERNEL);
	if (unlikely(!pool))
		return ERR_PTR(-ENOMEM);

	ret = init_srcu_struct(&pool->sess_list_srcu);
	if (ret) {
		pr_err("%s: Sess list srcu init failed, err: %d\n", __func__, ret);
		pool = ERR_PTR(ret);
		goto free_pool;
	}

	ret = init_srcu_struct(&pool->map_srcu);
	if (ret) {
		pr_err("%s: Map srcu init failed, err: %d\n", __func__, ret);
		pool = ERR_PTR(ret);
		goto cleanup_sess_srcu;
	}

	ret = percpu_ref_init(&pool->ids_inflight_ref,
			      rmr_pool_inflight_ref_release,
			      PERCPU_REF_ALLOW_REINIT, GFP_KERNEL);
	if (ret) {
		pr_err("%s: Percpu reference init failed for pool %s\n", __func__, poolname);
		pool = ERR_PTR(ret);
		goto cleanup_map_srcu;
	}

	pool->group_id = group_id;
	pool->map_ver = 1;
	pool->mapped_size = 0;
	xa_init_flags(&pool->stg_members, XA_FLAGS_ALLOC);
	init_completion(&pool->complete_done);
	init_completion(&pool->confirm_done);
	mutex_init(&pool->sess_lock);
	mutex_init(&pool->maps_lock);
	INIT_LIST_HEAD(&pool->entry);
	INIT_LIST_HEAD(&pool->sess_list);

	init_completion(&pool->discard_done);
	atomic_set(&pool->discard_waiting, 0);
	atomic_set(&pool->normal_count, 0);

	strscpy(pool->poolname, poolname, sizeof(pool->poolname));

	return pool;

cleanup_map_srcu:
	cleanup_srcu_struct(&pool->map_srcu);
cleanup_sess_srcu:
	cleanup_srcu_struct(&pool->sess_list_srcu);
free_pool:
	kfree(pool);
	return pool;
}

struct rmr_pool *rmr_create_pool(const char *poolname, void *priv)
{
	u32 group_id;
	struct rmr_pool *pool;

	mutex_lock(&pool_mutex);

	pool = rmr_find_pool(poolname);
	if (unlikely(pool)) {
		pr_err("Pool '%s' already exists\n", poolname);
		pool = ERR_PTR(-EEXIST);
		goto out;
	}

	/* Calculate the poolname hash */
	group_id = rmr_pool_hash(poolname);

	/* Double ensure there is no hash-clash */
	pool = rmr_find_pool_by_group_id(group_id);
	if (unlikely(pool)) {
		pr_err("Pool '%s' already exists\n", poolname);
		pool = ERR_PTR(-EEXIST);
		goto out;
	}

	pool = alloc_pool(poolname, group_id);
	if (IS_ERR(pool)) {
		pr_err("Pool allocation failed for pool %s\n", poolname);
		goto out;
	}

	list_add(&pool->entry, &pool_list);
	pool->priv = priv;
	pool->pool_md.magic = RMR_POOL_MD_MAGIC;

out:
	mutex_unlock(&pool_mutex);
	return pool;
}

/**
 * rmr_pool_maps_to_buf - Copy dirty_bitmap buffer of pool to buf
 *
 * @pool:	The pool whose map is to be copied
 * @map_idx:	The map index in the pool's map array
 * @offset:	The offset to read from in the maps dirty_bitmap buffer
 * @buf:	Pointer to buf where to copy the dirty_bitmap buffer
 * @buflen:	Length of the buf available to copy to
 * @filter:	TODO
 *
 * Description:
 *	This function is one half of the (map <-> buf) pair. It is used to save map into a buf.
 *	The other half is rmr_pool_save_map, which is used to save a buf into the map.
 *	This function is used while both sending a map and reading a map.
 *	The process for both of them is largely same.
 *
 *	The relevant params like member_id, offset for the dirty_bitmap buffer
 *	are stored in the rmr_map_buf_hdr, which is kept at the starting of buf.
 *
 *	The caller has to take care of sending the correct map index and offset to copy from.
 *	For this, the function provides some help in the form of updating the map_idx and
 *	offset values (for map send), and storing it those in map_buf_hdr (for map read).
 *
 * Return value:
 *	0 If there is no more data to send
 *	Total size copied to buf
 */
int rmr_pool_maps_to_buf(struct rmr_pool *pool, u8 *map_idx, u64 *slp_idx,
			 void *buf, size_t buflen, rmr_map_filter filter)
{
	struct rmr_map_buf_hdr *map_buf_hdr = (struct rmr_map_buf_hdr *)buf;
	struct rmr_dirty_id_map *map = NULL;
	int lock_idx;
	u64 no_of_slp;

	/* Adjust buf and buflen */
	buf += sizeof(struct rmr_map_buf_hdr);
	buflen -= sizeof(struct rmr_map_buf_hdr);

	lock_idx = srcu_read_lock(&pool->map_srcu);
	for ( ; ; *map_idx += 1) {

		if (*map_idx >= pool->maps_cnt) {
			srcu_read_unlock(&pool->map_srcu, lock_idx);
			return 0;
		}

		map = rcu_dereference(pool->maps[*map_idx]);
		if (map)
			break;
	}

	map_buf_hdr->version = RMR_MAP_FORMAT_VER;

	/* This is for the destination, to inform where to store */
	map_buf_hdr->member_id = map->member_id;
	map_buf_hdr->dst_slp_idx = (*slp_idx);

	/*
	 * SLPs are pages. Duh!
	 */
	no_of_slp = buflen >> PAGE_SHIFT;
	no_of_slp = min(no_of_slp, (map->total_slp - *slp_idx));
	rmr_map_slps_to_buf(map, *slp_idx, no_of_slp, buf);
	map_buf_hdr->buf_size = no_of_slp * PAGE_SIZE;

	if ((*slp_idx + no_of_slp) >= map->total_slp) {
		/*
		 * All done for this map.
		 * Now move on to the next one, and reset the index.
		 */
		*map_idx += 1;
		*slp_idx = 0;
	} else {
		/*
		 * Copy the number of SLPs we can, and increment the index.
		 */
		*slp_idx += no_of_slp;
	}

	pr_info("%s: buf_size %u, buflen w/o hdr %lu\n",
		__func__, map_buf_hdr->buf_size, buflen);

	/* This is for MAP_READ, to inform where to ask from next */
	map_buf_hdr->map_idx = *map_idx;
	map_buf_hdr->slp_idx = *slp_idx;

	srcu_read_unlock(&pool->map_srcu, lock_idx);

	return (map_buf_hdr->buf_size + sizeof(struct rmr_map_buf_hdr));
}

/**
 * rmr_pool_save_map - Copy given buf to dirty_bitmap buffer of pool
 *
 * @pool:	The pool whose map is the dest for the copy
 * @buf:	Pointer to buf from where to copy
 * @buflen:	Length of the buf available to copy
 * @test_only:	Only test if the buf given matches with dirty_bitmap buf of pool
 * @map_clean:	TODO
 *
 * Description:
 *	This function is the other half of the (map <-> buf) pair.
 *	It saves buf into the map of pool. The relevant params are read from the
 *	rmr_map_buf_hdr which lies in the start of the given buf.
 *
 * Return value:
 *	0 on success
 *	-errno on error
 */
int rmr_pool_save_map(struct rmr_pool *pool, void *buf, size_t buflen,
		      bool test_only)
{
	struct rmr_map_buf_hdr *map_buf_hdr = (struct rmr_map_buf_hdr *)buf;
	struct rmr_dirty_id_map *map = NULL;
	int err = 0, lock_idx;
	u32 buf_size;
	u64 slp_idx;

	if (map_buf_hdr->version != RMR_MAP_FORMAT_VER) {
		pr_err("Wrong map format. Expected %d but received %llu\n",
		       RMR_MAP_FORMAT_VER, map_buf_hdr->version);
		return -EINVAL;
	}

	/* Adjust buf and buflen */
	buf += sizeof(struct rmr_map_buf_hdr);
	buflen -= sizeof(struct rmr_map_buf_hdr);

	lock_idx = srcu_read_lock(&pool->map_srcu);
	map = rmr_pool_find_map(pool, map_buf_hdr->member_id);
	if (!map) {
		pr_err("%s: No map found for member_id %llu\n",
		       __func__, map_buf_hdr->member_id);
		err = -ENOENT;
		goto out;
	}

	slp_idx = map_buf_hdr->dst_slp_idx;
	buf_size = map_buf_hdr->buf_size;

	pr_info("%s: For pool %s, received map for %llu, slp_idx %llu, buf_size %u, buflen %lu\n",
		__func__, pool->poolname, map_buf_hdr->member_id, slp_idx, buf_size, buflen);

	/* Sanity */
	WARN_ON(buf_size > buflen);
	WARN_ON(buf_size % PAGE_SIZE);

	pr_info("%s: buf_size %u, buflen w/o hdr %lu\n", __func__, map_buf_hdr->buf_size, buflen);

	/*
	 * The buf_size would be a factor of PAGE_SIZE,
	 * and thats how we know no_of_slp(s) to save.
	 */
	if (!rmr_map_buf_to_slps(map, buf, buf_size, slp_idx, test_only)) {
		pr_err("%s: rmr_map_buf_to_slps failed\n", __func__);
		goto out;
	}

out:
	srcu_read_unlock(&pool->map_srcu, lock_idx);

	return err;
}
