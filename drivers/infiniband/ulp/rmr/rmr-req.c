// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#include <linux/slab.h>

#include "rmr-req.h"
#include "rmr-srv.h"
#include "rmr-clt.h"

extern struct kmem_cache *rmr_req_cachep;
extern struct kmem_cache *rmr_map_entry_cachep;
extern struct rmr_store_ops *pstore_ops;

static void rmr_req_complete(struct rmr_srv_req *req);
static void rmr_req_store_done(struct rmr_srv_req *req);
static void rmr_req_sync_failed(struct rmr_srv_req *req);
static void rmr_req_send_map_clear(struct rmr_srv_req *req);
static void rmr_req_sync_complete(struct rmr_srv_req *req);
static void rmr_req_store(struct rmr_srv_req *req);

/**
 * rmr_srv_req_resp - Response from the lower level module
 *
 * @req:	Request to be processed
 * @err:	Error value
 *
 * Description:
 *	This function is the return point from the below module
 *	where IO is submitted.
 *
 * Context:
 *	In this function the request should always be in state RMR_REQ_STATE_STORE
 */
void rmr_srv_req_resp(struct rmr_srv_req *req, int err)
{
	/*
	 * Use the error sent from lower layer
	 */
	req->err = err;

	/*
	 * For Normal (non-sync) requests we handle both non-error and error cases from one
	 * place. Since its simple.
	 */
	if (rmr_op(req->flags) != RMR_OP_SYNCREQ) {
		rmr_req_complete(req);
		return;
	}

	/*
	 * Sync requests are complicated, since it needs extra post-processing
	 * once IO is done for us.
	 *
	 * 1) In case of no failure, we need to send map clear to other nodes,
	 *    since they think we are still dirty for this chunk.
	 *
	 * 2) We need to check for waiting IO in entry->wait_list, and kick them.
	 */
	if (!req->err)
		rmr_req_store_done(req);
	else
		rmr_req_sync_failed(req);
}
EXPORT_SYMBOL(rmr_srv_req_resp);

/**
 * rmr_srv_req_create - Create an rmr server request
 *
 * @msg:	IO message containing information
 * @srv_pool:	Server pool creating this request
 * @rtrs_op:	rtrs IO context
 * @data:	pointer to data buf
 * @datalen:	len of data buf
 * @endreq:	Function to be called at the end of rmr request processing
 *
 * Description:
 *	RMR server request are base structures which holds the IO while they are being processed.
 *	They go through a state machine, while a number of checks are done. IOs which are
 *	destined for a chunk that is dirty, are paused while that chunk is synced.
 *
 * Return:
 *	Pointer to the create rmr server request on success
 *	Error pointer on failure
 */
struct rmr_srv_req *rmr_srv_req_create(const struct rmr_msg_io *msg, struct rmr_srv_pool *srv_pool,
				       struct rtrs_srv_op *rtrs_op, void *data, u32 datalen,
				       void (*endreq)(struct rmr_srv_req *, int))
{
	struct rmr_srv_req *req;
	struct rmr_srv_io_store *store = srv_pool->io_store;
	int i;

	if (!store || !atomic_read(&srv_pool->store_state)) {
		pr_err("%s: store not set, or srv_pool not in correct state %s\n",
		       __func__, srv_pool->pool->poolname);
		return ERR_PTR(-ENODEV);
	}

	req = kmem_cache_zalloc(rmr_req_cachep, GFP_KERNEL);
	if (!req) {
		pr_err("cannot allocate memory for rmr_req.\n");
		return ERR_PTR(-ENOMEM);
	}

	req->id.a = le64_to_cpu(msg->id_a);
	req->id.b = le64_to_cpu(msg->id_b);

	req->offset = le32_to_cpu(msg->offset);
	req->length = le32_to_cpu(msg->length);
	req->flags = le32_to_cpu(msg->flags);
	req->prio = le16_to_cpu(msg->prio);

	req->mem_id = le32_to_cpu(msg->mem_id);
	for (i = 0; i < msg->failed_cnt; i++)
		req->failed_srv_id[i] = msg->failed_id[i];

	req->failed_cnt = msg->failed_cnt;
	req->map_ver = le64_to_cpu(msg->map_ver);
	req->sync = msg->sync;

	req->data = data;
	req->datalen = datalen;
	req->rtrs_op = rtrs_op;
	req->srv_pool = srv_pool;
	req->store = store;
	req->endreq = endreq;

	pr_debug("req %p, chunk_size %u\n", req, req->srv_pool->pool->chunk_size);

	return req;
}

struct rmr_srv_req *rmr_srv_md_req_create(struct rmr_srv_pool *srv_pool,
					  struct rtrs_srv_op *rtrs_op, void *data,
					  u32 offset, u32 len, unsigned long flags,
					  void (*endreq)(struct rmr_srv_req *, int))
{
	struct rmr_srv_req *req;
	struct rmr_srv_io_store *store = srv_pool->io_store;

	if (!store) {
		pr_err("No store_id registered for srv pool %s\n", srv_pool->pool->poolname);
		return ERR_PTR(-ENODEV);
	}

	req = kmem_cache_zalloc(rmr_req_cachep, GFP_KERNEL);
	if (!req) {
		pr_err("cannot allocate memory for rmr_req.\n");
		return ERR_PTR(-ENOMEM);
	}
	req->offset = offset;
	req->length = len;
	req->flags = flags;
	req->sync = false; /* A md req is always non-sync */

	req->data = data;
	req->rtrs_op = rtrs_op;
	req->srv_pool = srv_pool;
	req->store = store;
	req->endreq = endreq;

	pr_debug("md req %p, len %u\n", req, len);

	return req;
}

void rmr_req_submit(struct rmr_srv_req *req);
static void rmr_req_sched(struct work_struct *work)
{
	struct rmr_srv_req *req = container_of(work, struct rmr_srv_req, work);

	pr_debug("scheduled work process for req %p\n", req);
	if (req->err)
		rmr_req_complete(req);
	else
		rmr_req_submit(req);
}

void rmr_process_wait_list(struct rmr_map_entry *entry, int err)
{
	struct llist_node *first, *next;
	struct rmr_srv_req *req;

	pr_debug("processing wait list for entry %p, sync_cnt=%d\n",
		 entry, atomic_read(&entry->sync_cnt));

	WARN_ON(atomic_read(&entry->sync_cnt) > 0);

	while (!llist_empty(&entry->wait_list)) {
		first = llist_del_all(&entry->wait_list);
		while (first) {
			next = first->next;
			req = llist_entry(first, struct rmr_srv_req, node);

			pr_debug("process waiting req %p id (%llu, %llu) flags %u\n",
				 req, req->id.a, req->id.b, req->flags);
			if (err) {
				pr_err("fail waiting req %p id (%llu, %llu) flags %u err %d\n",
				       req, req->id.a, req->id.b, req->flags, err);
				req->err = -EIO;
			}

			pr_debug("schedule processing req %p with err %d\n", req, req->err);
			INIT_WORK(&req->work, rmr_req_sched);
			schedule_work(&req->work);

			first = next;
		}
	}
}

void rmr_req_submit(struct rmr_srv_req *req)
{
	struct rmr_srv_pool *srv_pool = req->srv_pool;
	struct rmr_map_entry *entry;
	struct rmr_dirty_id_map *map;

	if (rmr_op(req->flags) == RMR_OP_FLUSH && !req->length) {
		rmr_req_store(req);
		return;
	}

	pr_debug("check map for req %p flag %u request id [%llu, %llu] offset %u length %u\n",
		 req, req->flags,
		 req->id.a, req->id.b, req->offset, req->length);

	map = rmr_pool_find_map(srv_pool->pool, srv_pool->member_id);
	if (!map) {
		pr_err("no map found for pool_id %u\n", srv_pool->member_id);
		req->err = -EINVAL;
		goto err;
	}

	rcu_read_lock();
	entry = rmr_map_get_dirty_entry(map, req->id);
	if (!entry) {
		/*
		 * The chunk containing data for this req is NOT dirty for us
		 */
		pr_debug("check map for req %p flags %u request id [%llu, %llu], no entry in the map\n",
			 req, req->flags, req->id.a, req->id.b);
		rcu_read_unlock();
		rmr_req_store(req);
		return;
	} else {
		/*
		 * The chunk for this data is dirty for us.
		 *
		 * we have 2 cases.
		 *
		 * 1) Its coming from a sync rmr-clt (Its an internal read).
		 * Then, fail the IO, since we do not want to end up in a deadlock,
		 * or go through multiple hops for a single read. The sender can try some other
		 * node itself.
		 */
		if (req->sync) {
			WARN_ON(rmr_op(req->flags) != RMR_OP_READ);
			rcu_read_unlock();
			req->err = -EIO;
			goto err;
		}

		/*
		 * 2) If its coming from a non-sync rmr-clt,
		 *    simply go ahead with syncing the data first.
		 */
		llist_add(&req->node, &entry->wait_list);
		pr_debug("%s: req %p flags %u id (%llu %llu) added to wait list. sync_cnt %d\n",
			 __func__, req, req->flags, req->id.a, req->id.b,
			 atomic_read(&entry->sync_cnt));

		rcu_read_unlock();
		/*
		 * If we are the first who grabs the entry then start sync.
		 *
		 * Otherwise, the one syncing the data would pick us up from the entry->wait_list
		 * and kick us. So simply exit for now.
		 */
		if (atomic_cmpxchg(&entry->sync_cnt, -1, 0) == -1) {
			int err;

			req->priv = entry;
			err = rmr_srv_sync_chunk_id(srv_pool, entry, req->id, false);
			if (err) {
				atomic_set(&entry->sync_cnt, -1);
				rmr_process_wait_list(entry, err);
			}
		}
	}

	return;

err:
	rmr_req_complete(req);
}

static void rmr_req_store(struct rmr_srv_req *req)
{
	int err;

	pr_debug("submit to store req %p flags %u request id [%llu, %llu] offset %u length %u\n",
		 req, req->flags,
		 req->id.a, req->id.b, req->offset, req->length);

	err = req->store->ops->submit_req(req->store->priv, req->data, req->offset,
					  req->length, req->flags, req->prio, req);
	if (err) {
		pr_err("%s: error submitting req %p, err %d\n", __func__, req, err);
		req->err = err;
		if (rmr_op(req->flags) == RMR_OP_SYNCREQ)
			rmr_req_sync_failed(req);
		else
			rmr_req_complete(req);
	}
}

static void rmr_md_req_store(struct rmr_srv_req *req)
{
	int err;

	err = req->store->ops->submit_md_req(req->store->priv, req->data, req->offset, req->length,
					     req->flags, req);
	if (err) {
		req->endreq(req, err);
		pr_err("release md req %p, flags %u\n", req, req->flags);
		kmem_cache_free(rmr_req_cachep, req);
	}
}

/* md req submission path*/
void rmr_md_req_submit(struct rmr_srv_req *req)
{
	rmr_md_req_store(req);
}

static void rmr_req_sched_store(struct work_struct *work)
{
	struct rmr_srv_req *req = container_of(work, struct rmr_srv_req, work);

	pr_debug("scheduled store for req %p\n", req);
	rmr_req_store(req);
}

static void rmr_req_remote_io_done(void *priv, int err)
{
	struct rmr_srv_req *req = priv;

	pr_debug("called for req %p, err code %d\n", req, err);

	rmr_clt_put_iu(req->srv_pool->clt, req->iu);

	if (err) {
		req->err = err;
		rmr_req_sync_failed(req);
		return;
	}

	pr_debug("schedule store for req %p with err %d\n", req, req->err);
	INIT_WORK(&req->work, rmr_req_sched_store);
	schedule_work(&req->work);
}

static void rmr_req_remote_read(struct rmr_srv_req *req)
{
	struct rmr_srv_pool *srv_pool = req->srv_pool;
	struct rmr_pool *clt = srv_pool->clt;
	unsigned long flags;
	int err;

	pr_debug("redirecting req id (%llu, %llu)\n",
		 req->id.a, req->id.b);
	if (!clt) {
		pr_err("No srv pool assigned for redirect for %s\n", srv_pool->pool->poolname);
		err = -EINVAL;
		goto err;
	}

	if (rmr_op(req->flags) == RMR_OP_SYNCREQ)
		flags = RMR_OP_READ;
	else
		flags = req->flags;

	req->iu = rmr_clt_get_iu(clt, flags, WAIT);
	if (IS_ERR_OR_NULL(req->iu)) {
		pr_err("Failed to get rmr_iu for req id (%llu, %llu)\n",
		       req->id.a, req->id.b);
		err = -EINVAL;
		goto err;
	}

	sg_init_one(&req->sg, req->data, req->datalen);

	pr_debug("After sg_init_one nents=%d\n", sg_nents(&req->sg));

	/* look at the flags here! */
	err = rmr_clt_request(clt, req->iu, req->offset, req->length, flags,
			      req->prio, req, rmr_req_remote_io_done,
			      &req->sg, sg_nents(&req->sg));
	if (err) {
		pr_err("rmr_clt_request error %d\n", err);
		rmr_clt_put_iu(clt, req->iu);
		err = -EREMOTEIO;
		goto err;
	}

	pr_debug("remote read submitted\n");
	return;

err:
	req->err = err;
	rmr_req_sync_failed(req);
}

static void rmr_sync_req_sched(struct work_struct *work)
{
	struct rmr_srv_req *req = container_of(work, struct rmr_srv_req, work);

	pr_debug("scheduled work process for req %p\n", req);
	if (req->err)
		rmr_req_sync_complete(req);
	else
		rmr_req_send_map_clear(req);
}

static void rmr_req_complete(struct rmr_srv_req *req)
{
	pr_debug("send completeion for req %p flags %u request id (%llu, %llu) offset %u length %u err %d\n",
		 req, req->flags,
		 req->id.a, req->id.b, req->offset, req->length, req->err);

	/* endreq() records the Last IO buffer accordingly. */
	req->endreq(req, req->err);

	pr_debug("release req %p, flags %u\n", req, req->flags);

	kmem_cache_free(rmr_req_cachep, req);
}

static struct rmr_srv_req *rmr_req_create_sync_req(struct rmr_srv_pool *srv_pool, rmr_id_t id,
						   u32 offset, u32 len, bool from_sync,
						   struct rmr_srv_req *parent)
{
	struct rmr_srv_req *req;
	struct rmr_srv_io_store *store = srv_pool->io_store;

	if (!store) {
		pr_err("No store_id registered for srv pool %s\n", srv_pool->pool->poolname);
		return ERR_PTR(-ENODEV);
	}

	req = kmem_cache_zalloc(rmr_req_cachep, GFP_KERNEL);
	if (!req) {
		pr_err("cannot allocate memory for rmr_req.\n");
		return ERR_PTR(-ENOMEM);
	}
	req->id.a = id.a;
	req->id.b = id.b;
	req->flags = RMR_OP_SYNCREQ;
	req->length = len;
	req->offset = offset;
	req->srv_pool = srv_pool;
	req->store = store;
	req->from_sync = from_sync;

	if (parent) {
		req->data = parent->data + offset;
	} else {
		req->data = kmalloc(req->length, GFP_KERNEL);
		if (!req->data) {
			pr_err("cannot allocate memory for sync req id [%llu, %llu]\n",
			       req->id.a, req->id.b);
			kmem_cache_free(rmr_req_cachep, req);
			return ERR_PTR(-ENOMEM);
		}
	}
	req->datalen = len;
	req->parent = parent;

	pr_debug("sync req %p created, flags %u request id (%llu, %llu) offset %u length %u parent %p\n",
		 req, req->flags, req->id.a, req->id.b, req->offset, req->length, parent);

	return req;
}

//should be called only if corresponding map entry has 0 sync cnt
int rmr_srv_sync_chunk_id(struct rmr_srv_pool *srv_pool, struct rmr_map_entry *entry,
			  rmr_id_t id, bool from_sync)
{
	struct rmr_pool *pool = srv_pool->pool;
	struct rmr_dirty_id_map *map;
	struct rmr_srv_req *parent_req;
	u32 max_io_size, total_len, offset;

	if (!srv_pool->clt) {
		pr_err("For pool %s no sync pool assigned.\n", pool->poolname);
		return -EINVAL;
	}
	max_io_size = srv_pool->max_sync_io_size;

	map = rmr_pool_find_map(pool, srv_pool->member_id);
	if (!map) {
		pr_err("no map found for pool_id %u\n", srv_pool->member_id);
		//TODO: handle this , probably initialize map, or just throw err?
		return -EINVAL;
	}

	offset = CHUNK_TO_OFFSET(id.b, pool->chunk_size_shift);
	total_len = pool->chunk_size;

	pr_debug("pool %s sync id (%llu, %llu), total_len %u, max_io_size %u\n",
		 pool->poolname, id.a, id.b, total_len, max_io_size);

	/*
	 * The parent_req starts with total_len, then get decremented in loop below.
	 * The child reqs are filled one by one from end to second.
	 *
	 * Maybe refactor this to a simple loop?
	 */
	parent_req = rmr_req_create_sync_req(srv_pool, id, offset, total_len, from_sync, NULL);
	if (IS_ERR_OR_NULL(parent_req)) {
		pr_err("pool %s failed to create main sync req to sync id (%llu, %llu)\n",
		       pool->poolname, id.a, id.b);
		return -ENOMEM;
	}
	parent_req->priv = entry;

	if (from_sync) {
		if (rmr_srv_get_sync_permit(srv_pool)) {
			pr_err("rmr_srv_sync_chunk_id failed to acquire permit for parent\n");
			kfree(parent_req->data);
			kmem_cache_free(rmr_req_cachep, parent_req);

			return -EINVAL;
		}
	}

	// inc ref cnt for parent_req
	map_entry_get_sync(entry);
	while (parent_req->length > max_io_size) {
		struct rmr_srv_req *req;
		u32 child_offset = offset + (parent_req->length - max_io_size);

		// submit req
		req = rmr_req_create_sync_req(srv_pool, id, (parent_req->length - max_io_size),
					      max_io_size, from_sync, parent_req);
		if (IS_ERR_OR_NULL(req)) {
			pr_err("%s: Pool %s, id (%llu, %llu), offset %u, len %u, err %ld\n",
			       __func__, pool->poolname, id.a, id.b,
			       (parent_req->length - max_io_size), max_io_size, PTR_ERR(req));
			parent_req->err = PTR_ERR(req);

			rmr_req_sync_failed(parent_req);
			return -EINVAL;
		}

		/*
		 * The offset sent to rmr_req_create_sync_req for this req is in context of the
		 * chunk. But the real offset for this req in the disk is this.
		 */
		req->offset = child_offset;

		if (from_sync) {
			if (rmr_srv_get_sync_permit(srv_pool)) {
				pr_err("rmr_srv_sync_chunk_id failed to acquire permit for child\n");
				kmem_cache_free(rmr_req_cachep, req);

				parent_req->err = -EBUSY;
				rmr_req_sync_failed(parent_req);
				return -EINVAL;
			}
		}

		// inc ref cnt for the child req just created
		map_entry_get_sync(entry);
		req->priv = entry;
		rmr_req_remote_read(req);

		parent_req->length -= max_io_size;
		parent_req->datalen -= max_io_size;
	}

	//submit parent req
	rmr_req_remote_read(parent_req);

	return 0;
}

static void __release_parent_req(struct rcu_head *head)
{
	struct rmr_srv_req *req = container_of(head, struct rmr_srv_req, rcu);
	struct rmr_map_entry *entry = req->priv;

	pr_debug("is called for req=%p id=(%llu,%llu) err=%d, entry=%p\n",
		 req, req->id.a, req->id.b, req->err, entry);

	kfree(req->data);

	//may be now we can stop saving entry in req->priv, but always rmr_map_find it
	if (!req->err) {
		pr_debug("req %p, completed all sync req, lets clean map\n", req);
		rmr_process_wait_list(entry, 0);
	} else {
		pr_debug("req %p completed with err %d, process wait list\n",
			 req, req->err);

		/* sync of this entry failed, we reset the sync_cnt so that the other req
		 * or sync thread could try again in the future. Without resetting, no one
		 * could get the ref and start sync again.
		 */
		atomic_set(&entry->sync_cnt, -1);
		rmr_process_wait_list(entry, req->err);
	}

	pr_debug("free entry %p for req %p\n", entry, req);
	kmem_cache_free(rmr_map_entry_cachep, entry);

	if (req->from_sync)
		rmr_srv_put_sync_permit(req->srv_pool);

	kmem_cache_free(rmr_req_cachep, req);
}

static void rmr_req_sync_complete(struct rmr_srv_req *req)
{
	struct rmr_srv_pool *srv_pool = req->srv_pool;
	struct rmr_dirty_id_map *map;
	int lock_idx;

	pr_debug("sync_req %p completed for id (%llu, %llu), offset %u, len %u, err %d, from sync %d\n",
		 req, req->id.a, req->id.b, req->offset, req->length,
		 req->err, req->from_sync);

	if (req->err)
		rmr_srv_sync_req_failed(req->srv_pool);

	pr_debug("release sync req %p, flags %u\n", req, req->flags);

	/*
	 * Only parent sync req own the allocated data.
	 */
	if (!req->parent) {
		if (!req->err) {
			map = rmr_pool_find_map(srv_pool->pool,
						srv_pool->member_id);
			if (map) {
				lock_idx = srcu_read_lock(&srv_pool->pool->map_srcu);
				rmr_map_unset_dirty(map, req->id,
						    MAP_NO_FILTER);
				srcu_read_unlock(&srv_pool->pool->map_srcu, lock_idx);
			} else {
				pr_err("no map found for pool_id %u\n", srv_pool->member_id);
				req->err = -EINVAL;
			}
		}

		pr_debug("req %p, completed all sync req, lets clean map\n",
			 req);
		call_rcu(&req->rcu, __release_parent_req);
	} else {
		/*
		 * Child req has nothing to do but put permit and free
		 */
		if (req->from_sync)
			rmr_srv_put_sync_permit(req->srv_pool);

		kmem_cache_free(rmr_req_cachep, req);
	}
}

static void rmr_req_sync_failed(struct rmr_srv_req *req)
{
	rmr_srv_sync_req_failed(req->srv_pool);

	pr_err("pool %s sync req %p failed for id (%llu, %llu), offset %u, len %u, err %d\n",
	       req->srv_pool->pool->poolname, req, req->id.a, req->id.b,
	       req->offset, req->length, req->err);

	rmr_req_store_done(req);
}

// this is actually very like rmr_req_remote_io_done but without rmr_clt_put_iu
// do we want to have one function for both cases?
static void rmr_req_map_clear_done(void *priv, int err)
{
	struct rmr_srv_req *req = priv;

	rmr_clt_put_iu(req->srv_pool->clt, req->iu);

	pr_debug("called for req %p, err code %d\n", req, err);
	if (err)
		pr_err("pool %s, sync req  with id (%llu, %llu) failed to send map clear\n",
		       req->srv_pool->pool->poolname, req->id.a, req->id.b);

	rmr_req_sync_complete(req);
}

static void rmr_req_store_done(struct rmr_srv_req *req)
{
	struct rmr_map_entry *entry = req->priv;
	struct rmr_srv_req *parent_req = NULL;

	pr_debug("called for req %p id (%llu, %llu ) offset %u len %u with parent req %p\n",
		 req, req->id.a, req->id.b, req->offset, req->length, req->parent);

	if (req->parent)
		parent_req = req->parent;
	else
		parent_req = req;

	if (req->err)
		parent_req->err = req->err;

	if (map_entry_put_sync(entry)) {
		pr_debug("%s: for entry %p id (%llu, %llu) all sync req done.\n", __func__,
			 entry, req->id.a, req->id.b);

		/* We have to schedule the work of parent req from here since we are in the
		 * interrupt context of either parent req or child req
		 */
		pr_debug("%s: process parent_req %p\n", __func__, parent_req);
		INIT_WORK(&parent_req->work, rmr_sync_req_sched);
		schedule_work(&parent_req->work);
	}

	if (req != parent_req) {
		pr_debug("completing req %p with err %d\n", req, req->err);
		rmr_req_sync_complete(req);
	}
}

static void rmr_req_send_map_clear(struct rmr_srv_req *req)
{
	struct rmr_srv_pool *srv_pool = req->srv_pool;
	struct rmr_pool *pool = srv_pool->clt;
	struct rmr_iu *iu;
	int err;

	if (!pool) {
		pr_err("Cannot send map clear. No pool client assigend for srv pool %s\n",
		       req->srv_pool->pool->poolname);
		req->err = -EINVAL;
		goto err;
	}

	/*
	 * We try to clear map, but if we fail to, we simply ignore the error.
	 * Such zombie entries will be clear by rmr_srv_check_map_clear.
	 */
	iu = rmr_clt_get_iu(pool, RMR_OP_WRITE, WAIT);
	if (IS_ERR_OR_NULL(iu)) {
		pr_err("Failed to get rmr_iu for req id (%llu, %llu)\n",
		       req->id.a, req->id.b);
		goto err;
	}

	pr_debug("send map clear req id (%llu, %llu), member_id %u\n",
		 req->id.a, req->id.b, srv_pool->member_id);

	/*
	 * For MAP_CLEAR, we only need rmr_id_t for chunk number,
	 * and our member_id to say to clear the above chunk number for ths storage node.
	 *
	 * We also update the minimum members needed for map update.
	 */
	iu->msg.hdr.group_id = cpu_to_le32(pool->group_id);
	iu->msg.hdr.type = cpu_to_le16(RMR_MSG_MAP_CLEAR);
	iu->msg.hdr.__padding = 0;

	iu->msg.id_a = cpu_to_le64(req->id.a);
	iu->msg.id_b = cpu_to_le64(req->id.b);
	iu->msg.member_id = srv_pool->member_id;

	iu->msg.flags = cpu_to_le32(RMR_OP_WRITE);

	iu->conf = rmr_req_map_clear_done;
	iu->priv = req;

	req->iu = iu;

	err = rmr_clt_send_map_update(pool, req->iu);
	if (err) {
		pr_err("%s error %d\n", __func__, err);
		rmr_clt_put_iu(pool, req->iu);
		goto err;
	}

	pr_debug("send map clear submitted\n");
	return;

err:
	rmr_req_sync_complete(req);
}
