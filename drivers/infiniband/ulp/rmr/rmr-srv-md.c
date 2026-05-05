// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reliable multicast over RTRS (RMR) — server metadata subsystem
 *
 * Copyright (c) 2026 IONOS SE
 */

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " L" __stringify(__LINE__) ": " fmt

#include <linux/module.h>
#include <linux/blkdev.h>

#include "rmr-srv.h"
#include "rmr-req.h"
#include "rmr-clt.h"

/**
 * process_md_io() - Process medata IO message
 *
 * @pool:	the pool where requests go through
 * @rtrs_op:	rtrs IO context
 * @offset:	offset in bytes relative to rmr metadata.
 * @len:	length of the buffer in bytes
 * @flags:	indicates metadata IO options
 * @buf:	pointer to metadata buffer
 *
 * Return:
 *	0 on success
 *
 * Description:
 *	All metadata IOs go through this function to submit requests to block device. The offset it
 *	passes on is relative to bytes shifting on rmr medata which is composed of a header
 *	structure for pool metadata, bitmap and last_io array.
 */
int process_md_io(struct rmr_pool *pool, struct rtrs_srv_op *rtrs_op, u32 offset, u32 len,
			 unsigned long flags, void *buf)
{
	struct rmr_srv_pool *srv_pool;
	struct rmr_srv_req *req;
	int err = 0;

	srv_pool = (struct rmr_srv_pool *)pool->priv;

	if (!percpu_ref_tryget_live(&pool->ids_inflight_ref)) {
		err = -EIO;
		goto no_put;
	}

	req = rmr_srv_md_req_create(srv_pool, rtrs_op, buf, offset, len, flags, rmr_srv_endreq);
	if (IS_ERR(req)) {
		pr_err("Failed to create rmr_req %pe\n", req);
		err = PTR_ERR(req);
		goto put_pool;
	}

	rmr_md_req_submit(req);
	return 0;

put_pool:
	percpu_ref_put(&pool->ids_inflight_ref);
no_put:
	return err;
}

int rmr_srv_read_md(struct rmr_pool *pool, struct rtrs_srv_op *rtrs_op, u32 offset, u32 len,
		    struct rmr_pool_md *pool_md_page)
{
	/* pool_md is pre-allocated */
	return process_md_io(pool, rtrs_op, offset, len, RMR_OP_MD_READ, pool_md_page);
}

static int rmr_srv_load_last_io(struct rmr_srv_pool *srv_pool)
{
	void *buf;
	u64 offset, len;
	struct rmr_pool *pool = srv_pool->pool;
	struct rmr_pool_md *pool_md = &pool->pool_md;
	int err = 0;

	if (!pool_md->queue_depth) {
		pr_err("%s: pool %s has zero queue_depth\n",
		       __func__, pool->poolname);
		return -EINVAL;
	}
	offset = RMR_LAST_IO_OFFSET;
	len = rmr_last_io_len(pool_md->queue_depth);

	if (!srv_pool->last_io_idx) {
		srv_pool->last_io_idx = kcalloc(pool_md->queue_depth,
						sizeof(*srv_pool->last_io_idx), GFP_KERNEL);
		if (!srv_pool->last_io_idx)
			return -ENOMEM;
	}

	buf = kzalloc(len, GFP_KERNEL);
	if (!buf) {
		err = -ENOMEM;
		return err;
	}

	err = rmr_srv_read_md(pool, NULL, offset, len, buf);
	if (err) {
		pr_err("%s: failed to read last_io buffer of len %lld at offset %lld\n",
		       __func__, len, offset);
		goto free_buf;
	}
	memcpy(srv_pool->last_io_idx, (rmr_id_t *)buf, len);

free_buf:
	kfree(buf);
	return err;
}

/**
 * rmr_srv_md_maps_sync - Sync dirty maps to persistent storage
 *
 * Description:
 *	Writes maps in two passes to the map-related regions of the on-disk layout:
 *
 *	Pass 1 — hdr_region (single PAGE_SIZE write at RMR_MD_SIZE + last_io_len):
 *	  Fills one rmr_map_cbuf_hdr slot per map_idx in [0:maps_cnt].
 *	  The buffer is kzalloc'd, so slots beyond maps_cnt are zero.
 *	  The entire PAGE_SIZE region is issued as a single I/O.
 *
 *	Pass 2 — maps_region (slp pages at computed offsets after hdr_region):
 *	  Each map's data offset = map_region_offset + map_idx * per_map_size.
 *	  pool->maps[0:maps_cnt] is always dense (no NULL gaps).
 */
void rmr_srv_md_maps_sync(struct rmr_pool *pool)
{
	struct rmr_map_cbuf_hdr *map_cbuf_hdr;
	struct rmr_dirty_id_map *map = NULL;
	u32 hdr_region_offset = rmr_bitmap_offset(pool->pool_md.queue_depth);
	u32 map_region_offset = hdr_region_offset + RMR_MAP_BUF_HDR_SIZE;
	u64 per_map_size = 0;
	int err, lock_idx;
	void *buf;
	u8 map_idx;

	buf = kzalloc(RMR_MAP_BUF_HDR_SIZE, GFP_KERNEL);
	if (!buf)
		return;

	lock_idx = srcu_read_lock(&pool->map_srcu);

	/* Fill the header region: one slot per active map */
	for (map_idx = 0; map_idx < pool->maps_cnt; map_idx++) {
		map = rcu_dereference(pool->maps[map_idx]);
		if (WARN_ON(!map))
			goto unlock;

		map_cbuf_hdr = buf + map_idx * sizeof(struct rmr_map_cbuf_hdr);
		map_cbuf_hdr->version = RMR_MAP_FORMAT_VER;
		map_cbuf_hdr->member_id = map->member_id;
		map_cbuf_hdr->no_of_chunks = map->no_of_chunks;
		map_cbuf_hdr->no_of_flp = map->no_of_flp;
		map_cbuf_hdr->no_of_slp_in_last_flp = map->no_of_slp_in_last_flp;
		map_cbuf_hdr->no_of_chunk_in_last_slp = map->no_of_chunk_in_last_slp;
		map_cbuf_hdr->total_slp = map->total_slp;
		per_map_size = map->total_slp * PAGE_SIZE;
	}

	/* Write the entire header region as a single PAGE_SIZE I/O */
	err = process_md_io(pool, NULL, hdr_region_offset,
			PAGE_SIZE, RMR_OP_MD_WRITE, buf);
	if (err) {
		pr_warn("%s: failed to write header region at 0x%x: %d\n",
			__func__, hdr_region_offset, err);
		goto unlock;
	}

	if (WARN_ON(!per_map_size))
		goto unlock;

	/* Write each map's slp pages */
	for (map_idx = 0; map_idx < pool->maps_cnt; map_idx++) {
		u32 map_data_offset;
		el_flp *flp_ptr;
		u64 no_of_slps;
		void *slp;
		int i, j;

		map = rcu_dereference(pool->maps[map_idx]);
		if (WARN_ON(!map))
			break;

		map_data_offset = map_region_offset + map_idx * per_map_size;

		for (i = 0; i < map->no_of_flp; i++) {
			flp_ptr = (el_flp *)map->dirty_bitmap[i];

			if (i == (map->no_of_flp - 1))
				no_of_slps = map->no_of_slp_in_last_flp;
			else
				no_of_slps = NO_OF_SLP_PER_FLP;

			for (j = 0; j < no_of_slps; j++, flp_ptr++) {
				slp = (void *)(*flp_ptr);

				err = process_md_io(pool, NULL, map_data_offset,
						PAGE_SIZE, RMR_OP_MD_WRITE, slp);
				if (err)
					pr_warn("%s: failed to write map slp at 0x%x: %d\n",
						__func__, map_data_offset, err);
				map_data_offset += PAGE_SIZE;
			}
		}
	}

unlock:
	srcu_read_unlock(&pool->map_srcu, lock_idx);
	kfree(buf);
}

/**
 * rmr_srv_refresh_md_maps - Restore maps from map buffers on disk
 *
 * Description:
 *	Reads back the maps written by rmr_srv_md_maps_sync(). Reads the hdr_region
 *	in a single I/O to obtain the per-map headers, then loads each present
 *	map's slp pages from maps_region:
 *	  data offset = map_region_offset + map_idx * per_map_size
 *	Header slots 0..N-1 are active; remaining are zero (member_id == 0).
 */
static int rmr_srv_refresh_md_maps(struct rmr_srv_pool *srv_pool)
{
	struct rmr_pool *pool = srv_pool->pool;
	struct rmr_map_cbuf_hdr *map_cbuf_hdr;
	struct rmr_dirty_id_map *map = NULL;
	u32 hdr_region_offset = rmr_bitmap_offset(pool->pool_md.queue_depth);
	u32 map_region_offset = hdr_region_offset + RMR_MAP_BUF_HDR_SIZE;
	int err = 0, lock_idx;
	void *buf;
	u8 map_idx, valid_nr = 0;
	bool unpack;

	buf = kzalloc(RMR_MAP_BUF_HDR_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* Read the entire header region in a single PAGE_SIZE I/O */
	err = rmr_srv_read_md(pool, NULL, hdr_region_offset, RMR_MAP_BUF_HDR_SIZE, buf);
	if (err) {
		pr_err("%s: failed to read header region at offset %u\n",
				__func__, hdr_region_offset);
		kfree(buf);
		return err;
	}

	lock_idx = srcu_read_lock(&pool->map_srcu);
	for (map_idx = 0; map_idx < RMR_POOL_MAX_SESS; map_idx++) {
		u64 per_map_size;
		u32 map_data_offset;
		el_flp *flp_ptr;
		u64 no_of_slps;
		void *slp;
		int i, j;

		map_cbuf_hdr = buf + map_idx * sizeof(struct rmr_map_cbuf_hdr);
		pr_debug("%s: %llu %u %llu %llu %llu %llu %llu\n", __func__,
			map_cbuf_hdr->version,
			map_cbuf_hdr->member_id,
			map_cbuf_hdr->no_of_chunks,
			map_cbuf_hdr->no_of_flp,
			map_cbuf_hdr->no_of_slp_in_last_flp,
			map_cbuf_hdr->no_of_chunk_in_last_slp,
			map_cbuf_hdr->total_slp);

		/* Empty slot: no more active maps beyond this point */
		if (!map_cbuf_hdr->member_id)
			break;
		valid_nr++;

		per_map_size = map_cbuf_hdr->total_slp * PAGE_SIZE;
		map_data_offset = map_region_offset + map_idx * per_map_size;

		unpack = false;
		/*
		 * The dirty map should be updated only when the one on disk is more updated.
		 * Such cases are as follows.
		 * 1) The dirty map does not exist in the pool. The map will be simply restored to
		 * the last version we have.
		 * 2) The dirty map of the pool is just created. If it has been updated, the one on
		 * disk is outdated.
		 */
		map = rmr_pool_find_map(pool, map_cbuf_hdr->member_id);
		if (!map) {
			map = rmr_map_create(pool, map_cbuf_hdr->member_id);
			if (IS_ERR(map)) {
				err = PTR_ERR(map);
				pr_err("%s: pool %s, member_id %d failed to create map\n",
				       __func__, pool->poolname, map_cbuf_hdr->member_id);
				goto unlock;
			}
			unpack = true;
		} else if (rmr_map_empty(map)) {
			unpack = true;
		}

		if (map->no_of_chunks != map_cbuf_hdr->no_of_chunks ||
				map->no_of_flp != map_cbuf_hdr->no_of_flp ||
				map->no_of_slp_in_last_flp != map_cbuf_hdr->no_of_slp_in_last_flp ||
				map->no_of_chunk_in_last_slp != map_cbuf_hdr->no_of_chunk_in_last_slp ||
				map->total_slp != map_cbuf_hdr->total_slp) {
			pr_err("%s: Sanity check failed\n", __func__);
			goto unlock;
		}

		xa_store(&pool->stg_members, map_cbuf_hdr->member_id, XA_TRUE, GFP_KERNEL);

		if (!unpack)
			continue;

		for (i = 0; i < map->no_of_flp; i++) {
			flp_ptr = (el_flp *)map->dirty_bitmap[i];

			if (i == (map->no_of_flp - 1))
				no_of_slps = map->no_of_slp_in_last_flp;
			else
				no_of_slps = NO_OF_SLP_PER_FLP;

			for (j = 0; j < no_of_slps; j++, flp_ptr++) {
				slp = (void *)(*flp_ptr);

				err = rmr_srv_read_md(pool, NULL, map_data_offset,
						PAGE_SIZE, slp);
				if (err) {
					pr_err("%s: failed to read bitmap at offset %u\n",
						__func__, map_data_offset);
					goto unlock;
				}
				map_data_offset += PAGE_SIZE;
			}
		}
	}

unlock:
	if (!valid_nr)
		pr_err("%s: no valid map found in metadata\n", __func__);

	/*
	 * TODO: We need better error handling logic here.
	 * Lets suppose after successfully reading few pages for a map, we fail to read next page.
	 * We then error out and fail the register, but leave the partially updated map in the pool.
	 * Later when another register is called, and we come here to read the maps, we will
	 * see a non-empty map, and skip reading the map from disk.
	 */
	srcu_read_unlock(&pool->map_srcu, lock_idx);
	kfree(buf);
	return err;
}

/**
 * rmr_srv_md_update() - update the metadata of the server pool
 *
 * Description:
 *	Read current in-memory pool states that changes to the srv_md of this pool.
 */
static int rmr_srv_md_update(struct rmr_srv_pool *srv_pool)
{
	struct rmr_pool *pool;
	struct rmr_srv_md *my_srv_md;
	int md_i;

	pool = srv_pool->pool;
	md_i = rmr_pool_find_md(&pool->pool_md, srv_pool->member_id, true);
	if (md_i < 0) {
		pr_warn("No space for new member %d.\n", srv_pool->member_id);
		return -EINVAL;
	}
	my_srv_md = &pool->pool_md.srv_md[md_i];
	my_srv_md->member_id = srv_pool->member_id;
	my_srv_md->store_state = atomic_read(&srv_pool->store_state);
	my_srv_md->map_ver = srv_pool->pool->map_ver;
	my_srv_md->srv_pool_state = atomic_read(&srv_pool->state);
	pr_debug("Set srv_md[%d] it with the member_id %d.\n", md_i, srv_pool->member_id);
	return 0;
}

/**
 * rmr_srv_flush_pool_md() - Write pool_md region to disk immediately
 *
 * @srv_pool:	Server pool whose pool_md is to be flushed
 *
 * Description:
 *	Persist pool_md without waiting for the delayed work.
 */
void rmr_srv_flush_pool_md(struct rmr_srv_pool *srv_pool)
{
	struct rmr_pool *pool = srv_pool->pool;
	void *buf;
	int err;

	if (!atomic_read(&srv_pool->store_state) || !pool->mapped_size)
		return;

	err = rmr_srv_md_update(srv_pool);
	if (err) {
		pr_warn("%s: failed to update pool_md before flush: 0x%x\n", __func__, err);
		return;
	}

	buf = kzalloc(RMR_MD_SIZE, GFP_KERNEL);
	if (!buf)
		return;

	memcpy(buf, &pool->pool_md, sizeof(struct rmr_pool_md));
	err = process_md_io(pool, NULL, 0, RMR_MD_SIZE, RMR_OP_MD_WRITE, buf);
	if (err)
		pr_warn("%s: failed to flush pool_md: 0x%x at offset 0 len %lu\n",
			__func__, err, RMR_MD_SIZE);
	kfree(buf);
}

/**
 * rmr_srv_flush_last_io() - Write last_io region to disk
 *
 * @srv_pool:	Server pool whose last_io is to be flushed
 */
static void rmr_srv_flush_last_io(struct rmr_srv_pool *srv_pool)
{
	struct rmr_pool *pool = srv_pool->pool;
	u64 last_io_len = rmr_last_io_len(pool->pool_md.queue_depth);
	void *buf;
	int err;

	if (!last_io_len || !srv_pool->last_io)
		return;

	buf = kzalloc(last_io_len, GFP_KERNEL);
	if (!buf)
		return;

	memcpy(srv_pool->last_io_idx, srv_pool->last_io, last_io_len);
	memcpy(buf, srv_pool->last_io_idx, last_io_len);

	err = process_md_io(pool, NULL, RMR_MD_SIZE, last_io_len,
			    RMR_OP_MD_WRITE, buf);
	if (err)
		pr_warn("%s: failed to flush last_io: 0x%x at offset %lu len %llu\n",
			__func__, err, RMR_MD_SIZE, last_io_len);
	kfree(buf);
}

/**
 * rmr_srv_md_load_buf() - Load the server metadata from buffer to the server pool.
 *
 * Description:
 *	This function loads the server-side metadata from buffer to the pool. The buffer must be
 *	in the format of rmr pool metadata structure, which may contain updated srv_md of
 *	multiple servers.
 */
static int rmr_srv_md_load_buf(struct rmr_pool *pool, void *buf)
{
	struct rmr_srv_pool *srv_pool = (struct rmr_srv_pool *)pool->priv;
	struct rmr_srv_md *srv_md_buf;
	u8 member_id = 0;
	int err = 0, index, i;
	bool ret = false;

	buf += (RMR_CLT_MD_SIZE - sizeof(struct rmr_srv_md));
	for (i = 0; i < RMR_POOL_MAX_SESS; i++) {
		buf += sizeof(struct rmr_srv_md);
		srv_md_buf = (struct rmr_srv_md *)buf;
		member_id = srv_md_buf->member_id;
		/* skip updating the srv_md of this server pool */
		if (!member_id || member_id == srv_pool->member_id)
			continue;

		index = rmr_pool_find_md(&pool->pool_md, member_id, true);
		if (index < 0) {
			pr_debug("%s: No space in the pool_md for new member %d\n",
				 __func__, member_id);
			err = -EINVAL;
			continue;
		}

		pr_debug("Load srv_md[%d] with member_id %d\n", index, member_id);
		memcpy(&pool->pool_md.srv_md[index], srv_md_buf, sizeof(struct rmr_srv_md));
		ret = true;
	}

	if (!ret) {
		pr_debug("No server metadata found in the buffer\n");
		err = -EINVAL;
	}

	return err;
}

/**
 * rmr_srv_md_process_buf() - Load the metadata from buffer to the server pool.
 *
 * Description:
 *	This node loads the metadata from buffer to the server pool.
 */
int rmr_srv_md_process_buf(struct rmr_pool *pool, void *buf, bool sync)
{
	struct rmr_srv_pool *srv_pool;
	struct rmr_pool_md *buf_pool_md, *dest_md = &pool->pool_md;
	int err = 0;

	srv_pool = (struct rmr_srv_pool *)pool->priv;
	buf_pool_md = (struct rmr_pool_md *)buf;
	if (!sync) {
		/* Copy only the client-side header. */
		memcpy(dest_md, buf_pool_md, RMR_CLT_MD_SIZE);
	} else {
		err = rmr_srv_md_load_buf(pool, buf);
		if (err)
			pr_err("Failed to load md buf to pool %s\n", pool->poolname);
	}

	return err;
}

int rmr_srv_send_md_update(struct rmr_pool *pool)
{
	struct rmr_srv_pool *srv_pool = (struct rmr_srv_pool *)pool->priv;
	struct rmr_pool *sync_pool = srv_pool->clt;
	struct rmr_msg_pool_cmd msg = {};
	int err = 0, buflen;
	void *buf;

	/* Only normal-state server pools should send metadata updates. */
	if (atomic_read(&srv_pool->state) != RMR_SRV_POOL_STATE_NORMAL)
		return -EINVAL;

	/* For a stg node A, is A->B alive? */
	if (!sync_pool) {
		pr_debug("pool %s has no sync pool assigned. Cannot send md update commands.\n",
			 pool->poolname);
		return -ENXIO;
	}

	buf = kzalloc(RMR_MD_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	buflen = RMR_MD_SIZE;

	rmr_clt_init_cmd(sync_pool, &msg);
	msg.cmd_type = RMR_CMD_MD_SEND;
	/* This node sends messages to start md_update. */
	msg.md_send_cmd.leader_id = srv_pool->member_id;
	msg.md_send_cmd.src_mapped_size = pool->mapped_size;

	err = rmr_clt_send_cmd_with_data_all(sync_pool, &msg, buf, buflen);
	if (err < 0) {
		pr_debug("pool %s sends all sess RMR_CMD_MD_SEND failed\n", pool->poolname);
		goto free_buf;
	}

	/*
	 * keep the original slice of buffer if the corresponding send req failed.
	 *
	 * TODO:
	 * We need to use the err received from rmr_clt_send_cmd_with_data_all in this function,
	 * and match the sessions we are skipping.
	 *
	 * In general, the sessions_skipped == (RMR_POOL_MAX_SESS - (number_of_legs - 1 - err).
	 * If the above number does not match, then we abandon the buffers, and try again.
	 */
	err = rmr_srv_md_load_buf(pool, buf);
	if (err) {
		pr_debug("Failed to load md buf to pool %s\n", pool->poolname);
		goto free_buf;
	}

free_buf:
	kfree(buf);
	return err;
}

/**
 * rmr_srv_refresh_md() - Refresh the metadata of the rmr pool.
 *
 * @srv_pool: Server pool whose metadata to be find
 *
 * Description:
 *	Read the metadata of the rmr pool from the backing store.
 *
 * Return:
 *	True when reading the metadata succeeds in two cases. The first case is a successful read
 *	but no metadata found. The second case is it found metadata which contains the srv_md.
 *	False otherwise.
 */
int rmr_srv_refresh_md(struct rmr_srv_pool *srv_pool)
{
	struct rmr_pool_md *pool_md_page;
	struct rmr_pool *pool = srv_pool->pool;
	int index, ret;
	u64 md_ver;

	pool_md_page = kzalloc(RMR_MD_SIZE, GFP_KERNEL);
	if (!pool_md_page)
		return -ENOMEM;

	if (rmr_srv_read_md(pool, NULL, 0, RMR_MD_SIZE, pool_md_page)) {
		pr_err("%s: failed reading md of rmr\n", __func__);
		goto free_md;
	}

	pr_info("%s: Read md of pool %s from store with magic 0x%llx\n",
		__func__, pool_md_page->poolname, pool_md_page->magic);

	if (pool_md_page->magic != RMR_POOL_MD_MAGIC) {
		pr_info("%s: No valid md found on the store for pool %s\n",
			__func__, pool->poolname);
		ret = -EINVAL;
		goto free_md;
	}

	/*
	 * TODO: Should we sanity check other params also?
	 */
	if (pool_md_page->chunk_size != pool->chunk_size) {
		pr_err("%s: chunk size mismatched. pool chunk size %u, md chunk size %u\n",
		       __func__, pool->chunk_size, pool_md_page->chunk_size);
		goto free_md;
	}

	/* Import the metadata to the states of the pool. */
	index = rmr_pool_find_md(pool_md_page, srv_pool->member_id, false);
	if (index < 0) {
		pr_info("%s: No md found for member_id %d\n", __func__, srv_pool->member_id);
		ret = index;
		goto free_md;
	}

	if (pool_md_page->srv_md[index].mapped_size != pool->mapped_size) {
		pr_err("%s: Mapped size mismatched. The srv pool %llu, md %llu\n",
		       __func__, pool->mapped_size, pool_md_page->mapped_size);
		ret = -EINVAL;
		goto free_md;
	}

	md_ver = pool_md_page->srv_md[index].map_ver;
	if (md_ver < pool->map_ver)
		pr_err("The current map ver is %lld but the map ver on md is %lld.\n",
		       pool->map_ver, md_ver);
	else
		pool->map_ver = md_ver;

	pool->pool_md = *pool_md_page;

	ret = rmr_srv_load_last_io(srv_pool);
	if (ret) {
		pr_err("%s: failed to load last_io array to memory with err 0x%x\n",
		       __func__, ret);
		goto zero_md;
	}

	pr_info("%s: no_of_chunks %lld\n", __func__, pool->no_of_chunks);
	ret = rmr_srv_refresh_md_maps(srv_pool);
	if (ret) {
		pr_err("%s: failed to load dirty bitmap to memory with err %pe\n",
		       __func__, ERR_PTR(ret));
		goto free_last_io;
	}
	goto free_md;

free_last_io:
	kfree(srv_pool->last_io_idx);
	srv_pool->last_io_idx = NULL;
zero_md:
	memset(&pool->pool_md, 0, sizeof(pool->pool_md));
free_md:
	kfree(pool_md_page);
	return ret;
}

/**
 * rmr_srv_mark_maps_dirty() - Set MD_DIRTY_MAPS and schedule delayed sync
 *
 * @srv_pool:	Server pool with changed maps
 */
void rmr_srv_mark_maps_dirty(struct rmr_srv_pool *srv_pool)
{
	set_bit(MD_DIRTY_MAPS, &srv_pool->md_dirty);
	mod_delayed_work(srv_pool->md_sync_wq, &srv_pool->md_sync_dwork,
			 msecs_to_jiffies(RMR_SRV_MD_SYNC_INTERVAL_MS));
}

/**
 * rmr_srv_md_sync - sync dirty metadata regions of pool
 *
 * Description:
 *	Dirty-driven consumer: only flushes regions whose dirty bit is set.
 *	Producers set bits and schedule this work via mod_delayed_work().
 *	Does NOT re-queue itself — the next dirty event will schedule it.
 */
void rmr_srv_md_sync(struct work_struct *work)
{
	struct rmr_srv_pool *srv_pool;
	struct rmr_pool *pool;
	bool ret, did_work = false;

	srv_pool = container_of(to_delayed_work(work), struct rmr_srv_pool, md_sync_dwork);
	if (!srv_pool->pool)
		return;

	/*
	 * It could happen that access the pool while the pool is not there. Use reference counting
	 * for server pool to avoid the issue.
	 */
	ret = rmr_get_srv_pool(srv_pool);
	if (!ret) {
		pr_err("%s: pool is not there\n", __func__);
		return;
	}

	pool = srv_pool->pool;

	/*
	 * Update srv_md snapshot and notify peers whenever any region is dirty.
	 */
	if (!rmr_srv_md_update(srv_pool) && rmr_srv_send_md_update(pool))
		pr_debug("failed to send md update\n");

	/*
	 * The io store is ready after the store is registered and the pool metadata is
	 * updated, if any.
	 */
	if (!atomic_read(&srv_pool->store_state) || !pool->mapped_size)
		goto put_pool;

	/*
	 * On-disk layout of rmr pool metadata:
	 *
	 *   0           RMR_MD_SIZE   +last_io_len    +PAGE_SIZE
	 *   +-----------+-------------+---------------+--------------------+
	 *   | pool_md   | last_io     | hdr_region    | maps_region ...    |
	 *   +-----------+-------------+---------------+--------------------+
	 *   <-RMR_MD_SIZE><-last_io_len><--PAGE_SIZE--><-per_map slp pages->
	 *
	 * pool->maps[0:maps_cnt] is always dense (no NULL gaps).
	 *
	 * This I/O covers pool_md + last_io. hdr_region and maps_region are
	 * written separately by rmr_srv_md_maps_sync().
	 */
	if (test_and_clear_bit(MD_DIRTY_POOL, &srv_pool->md_dirty)) {
		rmr_srv_flush_pool_md(srv_pool);
		did_work = true;
	}

	if (test_and_clear_bit(MD_DIRTY_LAST_IO, &srv_pool->md_dirty)) {
		rmr_srv_flush_last_io(srv_pool);
		did_work = true;
	}

	if (test_and_clear_bit(MD_DIRTY_MAPS, &srv_pool->md_dirty)) {
		rmr_srv_md_maps_sync(pool);
		did_work = true;
	}

	if (did_work)
		pr_debug("%s: flushed dirty regions for server pool %u of %s\n",
			 __func__, srv_pool->member_id, pool->poolname);

put_pool:
	rmr_put_srv_pool(srv_pool);
	/* Do NOT re-queue. Producers schedule us via mod_delayed_work. */
}
