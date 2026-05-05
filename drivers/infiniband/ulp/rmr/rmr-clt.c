// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reliable multicast over RTRS (RMR)
 *
 * Copyright (c) 2026 IONOS SE
 */

#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME " L" __stringify(__LINE__) ": " fmt

#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>

#include "rmr-clt.h"
#include "rmr-clt-trace.h"

MODULE_AUTHOR("The RMR and BRMR developers");
MODULE_DESCRIPTION("RMR Client");
MODULE_VERSION(RMR_VER_STRING);
MODULE_LICENSE("GPL");

#define RMR_CLT_SEND_MSG_TIMEOUT_MS 30000

//static int send_msg_leave_pool(struct rmr_clt_pool_sess *pool_sess, bool wait);
static void retry_failed_read(struct work_struct *work);
static DEFINE_MUTEX(g_sess_lock);
static LIST_HEAD(g_sess_list);

static bool rmr_get_clt_pool(struct rmr_clt_pool *clt_pool)
{
	pr_debug("pool %s, before inc refcount %d\n",
		 clt_pool->pool->poolname, refcount_read(&clt_pool->refcount));
	return refcount_inc_not_zero(&clt_pool->refcount);
}

static struct rmr_clt_pool *rmr_find_and_get_clt_pool(const char *poolname)
{
	struct rmr_pool *pool;
	struct rmr_clt_pool *clt_pool;

	mutex_lock(&pool_mutex);
	pool = rmr_find_pool(poolname);
	if (!pool) {
		clt_pool = ERR_PTR(-ENOENT);
		goto out;
	}

	clt_pool = (struct rmr_clt_pool *)pool->priv;
	if (!rmr_get_clt_pool(clt_pool))
		clt_pool = ERR_PTR(-EINVAL);

out:
	mutex_unlock(&pool_mutex);
	return clt_pool;
}

void rmr_put_clt_pool(struct rmr_clt_pool *clt_pool)
{
	struct rmr_pool *pool = clt_pool->pool;

	might_sleep();

	pr_debug("clt pool %s, before dec refcnt %d\n",
		 (pool ? pool->poolname : "(empty)"), refcount_read(&clt_pool->refcount));
	if (refcount_dec_and_test(&clt_pool->refcount)) {

		destroy_workqueue(clt_pool->recover_wq);
		mutex_destroy(&clt_pool->io_freeze_lock);
		mutex_destroy(&clt_pool->clt_pool_lock);

		if (pool) {
			pr_info("clt: destroy pool %s\n", pool->poolname);
			free_pool(pool);
		}

		kfree(clt_pool);
	}
}

static inline int rmr_clt_sess_get(struct rmr_clt_sess *sess)
{
	return kref_get_unless_zero(&sess->kref);
}

static void rmr_clt_sess_release(struct kref *kref)
{
	struct rmr_clt_sess *clt_sess;

	clt_sess = container_of(kref, struct rmr_clt_sess, kref);

	mutex_lock(&g_sess_lock);

	rmr_clt_destroy_clt_sess_sysfs_files(clt_sess);

	pr_info("close rtrs for session %s\n", clt_sess->sessname);
	rtrs_clt_close(clt_sess->rtrs);
	list_del(&clt_sess->g_list);
	kfree(clt_sess);

	mutex_unlock(&g_sess_lock);
}

void rmr_clt_sess_put(struct rmr_clt_sess *sess)
{
	kref_put(&sess->kref, rmr_clt_sess_release);
}

static const char *rmr_get_clt_pool_state_name(enum rmr_clt_pool_state state)
{
	switch (state) {
	case RMR_CLT_POOL_STATE_JOINED: return "RMR_CLT_POOL_STATE_JOINED";
	case RMR_CLT_POOL_STATE_IN_USE: return "RMR_CLT_POOL_STATE_IN_USE";

	default: return "Unknown state";
	}
}

static void rmr_clt_dump_state(struct rmr_clt_pool *rmr_clt_pool)
{
	char current_state[1024] = {0};
	int i, n = 0, len = sizeof(current_state);

	for (i = 0; i < RMR_CLT_POOL_STATE_MAX; i++) {
		enum rmr_clt_pool_state state = (enum rmr_clt_pool_state)i;

		if (test_bit(state, &rmr_clt_pool->state))
			n += scnprintf(current_state + n, len - n, "%s, ",
				       rmr_get_clt_pool_state_name(state));
	}

	pr_info("%s: RMR client pool current state: %s\n", __func__, current_state);
}

/**
 * rmr_clt_change_pool_state() - Change clt pool state
 *
 * @clt_pool:	Client pool whose state is to be changed
 * @new_state:	New state to set
 * @set:	Informs whether to set/unset the given new+state
 */
void rmr_clt_change_pool_state(struct rmr_clt_pool *rmr_clt_pool,
			       enum rmr_clt_pool_state new_state, bool set)
{
	if (set) {
		set_bit(new_state, &rmr_clt_pool->state);
		pr_info("%s: state %s set\n",
			__func__, rmr_get_clt_pool_state_name(new_state));
	} else {
		clear_bit(new_state, &rmr_clt_pool->state);
		pr_info("%s: state %s cleared\n",
			__func__, rmr_get_clt_pool_state_name(new_state));
	}

	rmr_clt_dump_state(rmr_clt_pool);
}

/**
 * send_map_get_version() - Send a map get version command
 *
 * @pool_sess:		pool session where to send the message
 *
 * Description:
 *	Ask the storage node to send back its map_version.
 *
 * Return:
 *	0 on success
 *	Negative error in case of failure
 */

/**
 * rmr_clt_md_update() - Update the client (non-sync) pool metadata
 */
static void rmr_clt_md_update(struct rmr_pool *pool)
{
	struct rmr_pool_md *clt_md = &pool->pool_md;

	if (pool->sync)
		return;

	clt_md->map_ver = pool->map_ver;
}

#if 0
static int send_map_set_version(struct rmr_clt_pool_sess *pool_sess, u64 ver)
{
	struct rmr_msg_pool_cmd msg = {};
	struct rmr_pool *pool = pool_sess->pool;
	int err;

	rmr_clt_init_cmd(pool, &msg);
	msg.cmd_type = RMR_CMD_MAP_SET_VER;
	msg.set_map_ver_cmd.map_ver = ver;

	err = rmr_clt_pool_send_cmd(pool_sess, &msg, WAIT);
	if (err) {
		pr_err("%s: For sess %s, %s failed with err %d\n",
		       __func__, pool_sess->sessname, rmr_get_cmd_name(msg.cmd_type), err);
	}
	return err;
}

/**
 * rmr_clt_coordinate_discard() - Coordinate the discard_entries flag
 *
 * @pool:		the client pool
 * @member_id:		member id of the source node
 *
 * Description:
 *	This function sends discard request to all normal pool sessions of the pool.
 *	It is to solve the case where network is partitioned between the server nodes
 *	and only the client connects those partitions. Any request that failed on a session
 *	would fail this call.
 *
 *	TODO: To address the network partitions (including the client), wait for consistency
 *	protocols.
 *
 * Return:
 *	0 on success
 *	Negative error in case of failure
 *
 * Pre-requisite: rcu read lock should be held by caller
 */
static int rmr_clt_coordinate_discard(struct rmr_pool *pool, u8 cmd_type, u8 member_id)
{
	struct rmr_clt_pool_sess *pool_sess;
	int err = 0;

	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		/*
		 * If the pool session state is not normal, the dirty maps of the that pool is
		 * likely corrupted. Don't bother to send the discards.
		 */
		if (atomic_read(&pool_sess->state) != RMR_CLT_POOL_SESS_NORMAL)
			continue;

		pr_info("%s: send discards to (pool_sess %s: %d) with member_id %u\n",
			__func__, pool_sess->sessname, pool_sess->member_id, member_id);

		/* Send discard request to the pool session. */
		err = send_discard(pool_sess, cmd_type, member_id);
		if (err) {
			pr_err("%s: Failed discard request on sess %s for member_id %u\n",
			       __func__, pool_sess->sessname, member_id);
			return err;
		}
	}

	return err;
}

static int rmr_clt_handle_discard(struct rmr_pool *pool)
{
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_dirty_id_map *map;
	int idx, ret, err = 0;
	u64 map_ver;

	idx = srcu_read_lock(&pool->sess_list_srcu);

	/* Find out if there is pending discard requests on the server side */
	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		ret = send_map_get_version(pool_sess, &map_ver);
		if (ret)
			continue;

		/*
		 * When disk replacement appears at the storage node, pserver will set the all
		 * map entries of that server to dirty.
		 */
		if (RMR_STORE_IS_REPLACE(map_ver)) {
			map = rmr_pool_find_map(pool, pool_sess->member_id);
			if (!map) {
				pr_err("The clt pool %s cannot find map for member_id %u\n",
				       pool->poolname, pool_sess->member_id);
				err = -EINVAL;
				goto out;
			}

			rmr_map_set_dirty_all(map, MAP_NO_FILTER);

			/* Check any normal pool session failed to receive discards */
			err = rmr_clt_coordinate_discard(pool, RMR_CMD_SEND_DISCARD,
					pool_sess->member_id);
			if (err) {
				pr_err("%s: Failed to coordinate discard state for member_id %u\n",
				       __func__, pool_sess->member_id);
				goto out;
			}

			/* update the map version */
			err = send_map_set_version(pool_sess, RMR_STORE_UNSET_REPLACE(map_ver));
			if (err) {
				pr_err("%s: Failed to reset map version for %s\n",
				       __func__, pool_sess->sessname);
				goto out;
			}

			/* Everyone knows about the discarded entries now. */
			err = rmr_clt_coordinate_discard(pool, RMR_CMD_DISCARD_CLEAR_FLAG,
					pool_sess->member_id);
			if (err) {
				pr_err("%s: Failed to clear discard flag for S%u\n",
				       __func__, pool_sess->member_id);
				goto out;
			}
		}
	}

out:
	srcu_read_unlock(&pool->sess_list_srcu, idx);
	return err;
}
#endif

static int rmr_clt_start_send_md(struct rmr_pool *pool);

/**
 * recover_work() - A work thread, which performs a number of tasks at regular intervals
 *
 * @work:	The work struct holding the data
 *
 * Description:
 *	Every client pool has its own work thread. It performs the following 3 tasks.
 *	1) Pool sessions in NORMAL state, and having dirty map entries associated with it,
 *	are checked, and if the entries are cleared from the particular storage node, then
 *	they are deleted from the pserver also.
 *	2) If the pool session state is FAILED, but the network state (clt session) is connected,
 *	then a store check message is send to the pool session. The storage node wil confirm
 *	with the backend, if IOs can be send or not.
 *	3) Send the client pool metadata to the servers.
 */
void recover_work(struct work_struct *work)
{
	struct rmr_pool *pool;
	struct rmr_clt_pool *clt_pool;
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_pool_md *clt_md;
	int index, lock_idx = 0;

	clt_pool = container_of(to_delayed_work(work), struct rmr_clt_pool, recover_dwork);
	pool = clt_pool->pool;

	pr_debug("check map for pool %s started...\n", pool->poolname);

	lock_idx = srcu_read_lock(&pool->sess_list_srcu);
	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		struct rmr_clt_sess *clt_sess = pool_sess->clt_sess;

		pr_debug("pool %s sess %s sess->member_id %d  sess->state %d\n",
			 pool->poolname, pool_sess->sessname,
			 pool_sess->member_id, atomic_read(&pool_sess->state));

		clt_md = &pool->pool_md;
		index = rmr_pool_find_md(clt_md, pool_sess->member_id, false);
		if (index < 0) {
			pr_debug("%s failed to find pool_sess %u\n",
				 __func__, pool_sess->member_id);
			continue;
		}
		if (pool_sess->maintenance_mode)
			goto pool_sess_state_check;

		if (atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_NORMAL) {
			struct rmr_dirty_id_map *map;

			map = rmr_pool_find_map(pool, pool_sess->member_id);
			if (!map) {
				pr_debug("pool %s no map found for member_id %u\n",
				       pool->poolname, pool_sess->member_id);
				continue;
			}
			if (!rmr_map_empty(map)) {
				pr_debug("pool %s sess %s map is not empty, check stg map...\n",
					 pool->poolname, pool_sess->sessname);
				send_map_check(pool_sess);
			}
		}
pool_sess_state_check:
		if (atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_FAILED &&
		    clt_sess->state == RMR_CLT_SESS_CONNECTED) {
			pr_debug("pool %s sess %s try pool sess recover\n",
				 pool->poolname, pool_sess->sessname);
			send_store_check(pool_sess);
		}
	}
	srcu_read_unlock(&pool->sess_list_srcu, lock_idx);

	rmr_clt_md_update(pool);
	/* If the send fails, wait for the next update. */
	rmr_clt_start_send_md(pool);

	pr_debug("check map for pool %s done. schedule next one.\n", pool->poolname);

	queue_delayed_work(clt_pool->recover_wq, &clt_pool->recover_dwork,
			   msecs_to_jiffies(RMR_RECOVER_INTERVAL_MS));
}

static int init_clt_pool(struct rmr_clt_pool *clt_pool)
{
	int err;

	clt_pool->pcpu_sess = alloc_percpu(typeof(*clt_pool->pcpu_sess));
	if (unlikely(!clt_pool->pcpu_sess)) {
		err = -ENOMEM;
		goto out_err;
	}

	return 0;

out_err:
	return err;
}

static void destroy_clt_pool(struct rmr_pool *pool)
{
	int i;
	struct rmr_clt_pool *clt_pool;
	struct rmr_dirty_id_map *map;
	struct rmr_dirty_id_map *maplist = NULL;

	clt_pool = (struct rmr_clt_pool *)pool->priv;
	if (clt_pool) {
		free_percpu(clt_pool->pcpu_sess);
		clt_pool->pcpu_sess = NULL;
	}

	mutex_lock(&pool->maps_lock);
	for (i = 0; i < pool->maps_cnt; i++) {
		map = rcu_dereference_protected(pool->maps[i],
						lockdep_is_held(&pool->maps_lock));
		if (WARN_ON(!map))
			continue;
		rcu_assign_pointer(pool->maps[i], NULL);
		map->next = maplist;
		maplist = map;
	}
	pool->maps_cnt = 0;

	if (maplist)
		synchronize_srcu(&pool->map_srcu);

	mutex_unlock(&pool->maps_lock);

	rmr_maplist_destroy(maplist);
}

static void rmr_put_sess_iu(struct rmr_clt_pool_sess *pool_sess,
			    struct rmr_clt_sess_iu *sess_iu);

static struct rmr_iu *
rmr_alloc_iu(void)
{
	struct rmr_iu *iu;

	iu = kzalloc(sizeof(*iu), GFP_KERNEL);
	if (!iu)
		return NULL;
	INIT_LIST_HEAD(&iu->sess_list);
	iu->num_sessions = 0;
	refcount_set(&iu->ref, 1);
	return iu;
}

void rmr_get_iu(struct rmr_iu *iu)
{
	refcount_inc(&iu->ref);
}

void rmr_put_iu(struct rmr_iu *iu)
{
	struct rmr_clt_sess_iu *sess_iu, *tmp;

	if (refcount_dec_and_test(&iu->ref)) {
		list_for_each_entry_safe(sess_iu, tmp,
					 &iu->sess_list, entry) {
			if (!list_empty(&sess_iu->entry))
				list_del_init(&sess_iu->entry);
			rmr_put_sess_iu(sess_iu->pool_sess, sess_iu);
		}
		kfree(iu);
	}
}

void rmr_clt_free_pool_sess(struct rmr_clt_pool_sess *pool_sess)
{
	struct rmr_clt_pool *clt_pool;
	struct rmr_clt_sess *clt_sess = pool_sess->clt_sess;

	clt_pool = (struct rmr_clt_pool *)pool_sess->pool->priv;

	if (!list_empty(&pool_sess->clt_sess_entry)) {
		mutex_lock(&clt_sess->lock);
		list_del(&pool_sess->clt_sess_entry);
		mutex_unlock(&clt_sess->lock);
	}

	pr_info("before free pool_sess %s, clt_sess refcount=%d\n",
		pool_sess->sessname, kref_read(&clt_sess->kref));

	kfree(pool_sess);
}

void rmr_clt_put_pool(struct rmr_pool *pool)
{
	struct rmr_clt_pool *clt_pool = (struct rmr_clt_pool *)pool->priv;

	rmr_put_clt_pool(clt_pool);
}
EXPORT_SYMBOL(rmr_clt_put_pool);

/**
 * rmr_clt_open() - Open a client for use
 *
 * @priv:		private data for the user
 * @link_ev:		holds the link event callback
 * @poolname:		name of the pool to open
 *
 * Description:
 *	Open an RMR pool for the user to use. The rmr pool must have at least one session.
 *	A single pool can be opened and used by only a single user.
 *
 * Return:
 *	Returns pointer to the rmr pool opened.
 */
struct rmr_pool *rmr_clt_open(void *priv, rmr_clt_ev_fn *link_ev, const char *poolname)
{
	struct rmr_clt_pool *clt_pool;
	int err;

	clt_pool = rmr_find_and_get_clt_pool(poolname);
	if (IS_ERR(clt_pool)) {
		pr_err("RMR client pool '%s' is not found\n", poolname);
		err = PTR_ERR(clt_pool);
		goto err_out;
	}

	if (!mutex_trylock(&clt_pool->clt_pool_lock)) {
		pr_err("RMR client pool '%s' is busy, recovery in progress\n", poolname);
		err = -EBUSY;
		goto put_err;
	}
	if (test_bit(RMR_CLT_POOL_STATE_IN_USE, &clt_pool->state)) {
		pr_err("RMR client pool '%s' is already in use\n", poolname);
		err = -ENOENT;
		goto put_err;
	}

	if (!test_bit(RMR_CLT_POOL_STATE_JOINED, &clt_pool->state)) {
		pr_err("RMR client pool '%s' has no sessions open\n", poolname);
		err = -ENOENT;
		goto put_err;
	}

	clt_pool->link_ev = link_ev;
	clt_pool->priv = priv;

	err = init_clt_pool(clt_pool);
	if (unlikely(err)) {
		pr_err("RMR client pool '%s' failed to initialize: %d\n", poolname, err);
		goto put_err;
	}

	rmr_clt_change_pool_state(clt_pool, RMR_CLT_POOL_STATE_IN_USE, true);

	mutex_unlock(&clt_pool->clt_pool_lock);
	return clt_pool->pool;

put_err:
	mutex_unlock(&clt_pool->clt_pool_lock);
	rmr_put_clt_pool(clt_pool);
err_out:
	return ERR_PTR(err);
}
EXPORT_SYMBOL(rmr_clt_open);

void rmr_clt_close(struct rmr_pool *pool)
{
	struct rmr_clt_pool *clt_pool = (struct rmr_clt_pool *)pool->priv;

	mutex_lock(&clt_pool->clt_pool_lock);
	rmr_clt_change_pool_state(clt_pool, RMR_CLT_POOL_STATE_IN_USE, false);

	pr_info("%s: RMR client close called for pool %s\n", __func__, pool->poolname);

	/*
	 * Freeze I/O.
	 * Degrade ref count to the usual model with a single shared
	 * atomic_t counter
	 */
	rmr_clt_pool_io_freeze(clt_pool);
	pr_info("pool %s wait for inflight io to complete\n", clt_pool->pool->poolname);

	/* Wait for all completion */
	rmr_clt_pool_io_wait_complete(clt_pool);

	pr_info("pool %s inflight io completed\n", clt_pool->pool->poolname);

	clt_pool->link_ev = NULL;
	clt_pool->priv = NULL;

	/* Unfreeze and Resurrect */
	rmr_clt_pool_io_unfreeze(clt_pool);

	mutex_unlock(&clt_pool->clt_pool_lock);

	rmr_put_clt_pool(clt_pool);
}
EXPORT_SYMBOL(rmr_clt_close);

void *rmr_clt_get_priv(struct rmr_pool *pool)
{
	struct rmr_clt_pool *clt_pool;

	clt_pool = (struct rmr_clt_pool *)pool->priv;
	if (clt_pool)
		return clt_pool->priv;

	return NULL;
}
EXPORT_SYMBOL(rmr_clt_get_priv);

static struct rmr_clt_sess *alloc_clt_sess(const char *sessname)
{
	struct rmr_clt_sess *sess;

	sess = kzalloc_node(sizeof(*sess), GFP_KERNEL, NUMA_NO_NODE);
	if (unlikely(!sess)) {
		pr_err("Failed to create session %s,"
		       " allocating session struct failed\n",
		       sessname);
		return ERR_PTR(-ENOMEM);
	}
	strscpy(sess->sessname, sessname, sizeof(sess->sessname));
	mutex_init(&sess->lock);
	INIT_LIST_HEAD(&sess->pool_sess_list);
	kref_init(&sess->kref);
	sess->state = RMR_CLT_SESS_DISCONNECTED;

	return sess;
}

static struct rmr_clt_pool_sess *alloc_pool_sess(struct rmr_pool *pool,
						 struct rmr_clt_sess *clt_sess)
{
	struct rmr_clt_pool_sess *pool_sess;

	pool_sess = kzalloc_node(sizeof(*pool_sess), GFP_KERNEL, NUMA_NO_NODE);
	if (unlikely(!pool_sess)) {
		pr_err("Failed to allocate session for pool %s\n", pool->poolname);
		return ERR_PTR(-ENOMEM);
	}

	strscpy(pool_sess->sessname, clt_sess->sessname, NAME_MAX);
	INIT_LIST_HEAD(&pool_sess->entry);
	INIT_LIST_HEAD(&pool_sess->clt_sess_entry);
	pool_sess->pool = pool;
	pool_sess->clt_sess = clt_sess;
	pool_sess->maintenance_mode = false;
	atomic_set(&pool_sess->state, RMR_CLT_POOL_SESS_CREATED);

	return pool_sess;
}

/*
 * Checks if the session already exists (search by session name)
 * Returns TRUE if session found, FALSE otherwise.
 */
static bool __find_sess_by_name(struct rmr_pool *pool, const char *sessname)
{
	struct rmr_clt_pool_sess *pool_sess;
	int idx;

	idx = srcu_read_lock(&pool->sess_list_srcu);
	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		if (!strcmp(sessname, pool_sess->sessname)) {
			srcu_read_unlock(&pool->sess_list_srcu, idx);
			return true;
		}
	}
	srcu_read_unlock(&pool->sess_list_srcu, idx);

	return false;
}

/**
 * __find_sess_by_member_id() - Find and return pool_sess with a given member_id
 *
 * @pool:	RMR pool to search pool_sess in
 * @member_id:	member ID to search
 *
 * Return:
 *	Pointer to rmr_clt_pool_sess on success
 *	NULL if no pool session exists with the given member_id
 *
 * Context:
 *	The caller should hold srcu_read_lock
 */
static struct rmr_clt_pool_sess *__find_sess_by_member_id(struct rmr_pool *pool, u8 member_id)
{
	struct rmr_clt_pool_sess *pool_sess = NULL, *tmp_pool_sess;

	list_for_each_entry_srcu(tmp_pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		if (member_id == tmp_pool_sess->member_id) {
			pool_sess = tmp_pool_sess;
			break;
		}
	}

	return pool_sess;
}

/**
 * pool_sess_change_state() - Change pool session state
 *
 * @pool_sess:		Pool session whose state is to be changed
 * @newstate:		New state which is to be set
 *
 * Description:
 *	Pool session states decide a number of crucial things.
 *	Where the IOs can be sent, which node has an outdated map, etc.
 *	As such, transitioning of states are important and is tightly controlled through
 *	this function. All state transitions should happen through this function.
 *
 * Return:
 *	True in case the state was changed
 *	False in case the state was not changed
 */
bool pool_sess_change_state(struct rmr_clt_pool_sess *pool_sess,
			    enum rmr_clt_pool_sess_state newstate)
{
	bool changed = false;
	int oldstate = atomic_read(&pool_sess->state);

	if (WARN_ON(oldstate == RMR_CLT_POOL_SESS_REMOVING))
		goto out;

	switch (newstate) {
	case RMR_CLT_POOL_SESS_NORMAL:
		if (pool_sess->maintenance_mode)
			break;
		/*
		 * Non-sync sessions must pass through RECONNECTING before
		 * reaching NORMAL so that a map update can take place first.
		 * Sync sessions skip RECONNECTING entirely and go FAILED→NORMAL
		 * directly.
		 */
		if (!rmr_clt_sess_is_sync(pool_sess)) {
			if (WARN_ON(oldstate == RMR_CLT_POOL_SESS_FAILED))
				break;
			if (oldstate == RMR_CLT_POOL_SESS_CREATED ||
			    oldstate == RMR_CLT_POOL_SESS_RECONNECTING)
				changed = atomic_try_cmpxchg(&pool_sess->state,
							     &oldstate,
							     newstate);
		} else {
			if (oldstate == RMR_CLT_POOL_SESS_CREATED ||
			    oldstate == RMR_CLT_POOL_SESS_FAILED ||
			    oldstate == RMR_CLT_POOL_SESS_RECONNECTING)
				changed = atomic_try_cmpxchg(&pool_sess->state,
							     &oldstate,
							     newstate);
		}
		break;
	case RMR_CLT_POOL_SESS_RECONNECTING:
		/*
		 * Sync sessions never need a map update and must not enter
		 * RECONNECTING.
		 */
		if (WARN_ON(rmr_clt_sess_is_sync(pool_sess) &&
			    !pool_sess->maintenance_mode))
			break;
		if (oldstate == RMR_CLT_POOL_SESS_FAILED ||
		    oldstate == RMR_CLT_POOL_SESS_CREATED ||
		    (oldstate == RMR_CLT_POOL_SESS_NORMAL && pool_sess->maintenance_mode))
			changed = atomic_try_cmpxchg(&pool_sess->state,
						     &oldstate,
						     newstate);
		break;
	case RMR_CLT_POOL_SESS_FAILED:
		changed = atomic_try_cmpxchg(&pool_sess->state,
					     &oldstate,
					     newstate);
		/*
		 * TODO
		 * We should really be updating map version with the state,
		 * Or before it.
		 */
		if (changed && oldstate != RMR_CLT_POOL_SESS_FAILED)
			pool_sess->pool->map_ver++;
		break;
	case RMR_CLT_POOL_SESS_REMOVING:
		changed = atomic_try_cmpxchg(&pool_sess->state,
					     &oldstate,
					     newstate);
		break;
	default:
		pr_err("%s: Unknown state %d\n", __func__, newstate);
		break;
	}

	if (changed && !rmr_clt_sess_is_sync(pool_sess)) {
		if (newstate == RMR_CLT_POOL_SESS_NORMAL) {
			/*
			 * Entering NORMAL: this session is no longer the last
			 * authoritative holder of the dirty map.
			 */
			pool_sess->was_last_authoritative = false;
			atomic_inc(&pool_sess->pool->normal_count);
		} else if (oldstate == RMR_CLT_POOL_SESS_NORMAL) {
			/*
			 * Leaving NORMAL via FAILED or maintenance-mode
			 * RECONNECTING: decrement the count of NORMAL sessions.
			 * If this was the last one, mark it as authoritative so
			 * that recovery can enable it directly (without a map
			 * update) when it comes back — its dirty map was the last
			 * complete one the pool had.
			 *
			 * REMOVING is not marked authoritative: a deliberate
			 * removal (delete or disassemble) is not an uncontrolled
			 * failure. On reassembly the leg goes through the full
			 * map update path and does not need the direct-enable
			 * shortcut.
			 */
			if (newstate == RMR_CLT_POOL_SESS_FAILED ||
			    (newstate == RMR_CLT_POOL_SESS_RECONNECTING &&
			     pool_sess->maintenance_mode)) {
				if (atomic_dec_and_test(&pool_sess->pool->normal_count))
					pool_sess->was_last_authoritative = true;
			} else {
				/* REMOVING */
				atomic_dec(&pool_sess->pool->normal_count);
			}
		}
	}

out:

	trace_pool_sess_change_state(pool_sess, newstate, oldstate, changed);

	return changed;
}

void rmr_clt_pool_io_freeze(struct rmr_clt_pool *clt_pool)
{
	struct rmr_pool *pool = clt_pool->pool;

	mutex_lock(&clt_pool->io_freeze_lock);
	if (atomic_inc_return(&clt_pool->io_freeze) == 1)
		percpu_ref_kill(&pool->ids_inflight_ref);
	mutex_unlock(&clt_pool->io_freeze_lock);
}

void rmr_clt_pool_io_unfreeze(struct rmr_clt_pool *clt_pool)
{
	struct rmr_pool *pool = clt_pool->pool;

	mutex_lock(&clt_pool->io_freeze_lock);
	if (atomic_dec_return(&clt_pool->io_freeze) == 0) {
		reinit_completion(&pool->complete_done);
		percpu_ref_reinit(&pool->ids_inflight_ref);

		wake_up_all(&clt_pool->map_update_wq);
	}
	mutex_unlock(&clt_pool->io_freeze_lock);
}

void rmr_clt_pool_io_wait_complete(struct rmr_clt_pool *clt_pool)
{
	struct rmr_pool *pool = clt_pool->pool;

	wait_for_completion(&pool->complete_done);
}

//am: what kind of locking is rquired for that ?
static void set_pool_sess_states_to_failed(struct rmr_clt_sess *clt_sess)
{
	struct rmr_clt_pool_sess *pool_sess;

	mutex_lock(&clt_sess->lock);

	list_for_each_entry(pool_sess, &clt_sess->pool_sess_list, clt_sess_entry) {
		if (pool_sess_change_state(pool_sess, RMR_CLT_POOL_SESS_FAILED))
			pr_info("set sess %s to failed due to link_ev\n", pool_sess->sessname);
	}
	mutex_unlock(&clt_sess->lock);
}

static void rmr_clt_link_ev(void *priv, enum rtrs_clt_link_ev ev)
{
	struct rmr_clt_sess *clt_sess = priv;

	switch (ev) {
	case RTRS_CLT_LINK_EV_DISCONNECTED:
		pr_info("Rtrs link ev disconnected: session %s\n",
			clt_sess->sessname);
		clt_sess->state = RMR_CLT_SESS_DISCONNECTED;
		set_pool_sess_states_to_failed(clt_sess);
		break;
	case RTRS_CLT_LINK_EV_RECONNECTED:
		pr_info("Rtrs link ev reconnected: session %s\n",
			clt_sess->sessname);
		clt_sess->state = RMR_CLT_SESS_CONNECTED;
		resend_join_pool(clt_sess);
		break;
	default:
		pr_err("Unknown rtrs link event received (%d), "
		       "session: %s\n",
		       ev, clt_sess->sessname);
	}
}

/*
 * Gets an iu for I/O operations.
 *
 * Context:
 *	The call to this function should be protected with an srcu_read_lock.
 */
static struct rmr_clt_sess_iu *rmr_get_sess_iu(struct rmr_clt_pool_sess *pool_sess,
					       enum rtrs_clt_con_type con_type,
					       enum wait_type wait)
{
	struct rmr_pool *pool = pool_sess->pool;
	struct rmr_clt_sess *clt_sess = pool_sess->clt_sess;
	struct rmr_clt_sess_iu *sess_iu;
	struct rtrs_permit *permit;

	WARN_ON(!srcu_read_lock_held(&pool->sess_list_srcu));

	if (clt_sess->state == RMR_CLT_SESS_DISCONNECTED) {
		pr_info("The rmr client session %s state is disconnected\n", clt_sess->sessname);
		return NULL;
	}

	sess_iu = kzalloc(sizeof(*sess_iu), GFP_KERNEL);
	if (!sess_iu)
		return NULL;

	permit = rtrs_clt_get_permit(clt_sess->rtrs, con_type, wait);
	if (unlikely(!permit)) {
		kfree(sess_iu);
		return NULL;
	}

	INIT_LIST_HEAD(&sess_iu->entry);
	sess_iu->permit = permit;
	sess_iu->pool_sess = pool_sess;

	return sess_iu;
}

/*
 * Gets the iu for user messages.
 * It will be reference counted initialized with refcount
 */
static inline struct rmr_clt_sess_iu *rmr_msg_get_iu(struct rmr_clt_pool_sess *pool_sess,
						     enum rtrs_clt_con_type con_type,
						     enum wait_type wait, int refcount)
{
	struct rmr_pool *pool = pool_sess->pool;
	struct rmr_clt_sess_iu *sess_iu;
	int idx;

	idx = srcu_read_lock(&pool->sess_list_srcu);

	sess_iu = rmr_get_sess_iu(pool_sess, con_type, wait);
	srcu_read_unlock(&pool->sess_list_srcu, idx);

	if (unlikely(!sess_iu))
		return NULL;

	init_waitqueue_head(&sess_iu->comp.wait);
	sess_iu->comp.errno = INT_MAX;
	atomic_set(&sess_iu->refcount, refcount);

	return sess_iu;
}

/*
 * reference counted put, refcount has to be initialized.
 */
void rmr_msg_put_iu(struct rmr_clt_pool_sess *pool_sess,
		    struct rmr_clt_sess_iu *sess_iu)
{
	if (atomic_dec_and_test(&sess_iu->refcount)) {
		rtrs_clt_put_permit(pool_sess->clt_sess->rtrs, sess_iu->permit);
		kfree(sess_iu);
	}
}

/*
 * put the sess_iu without reference counting.
 * I/O does not need reference counting.
 */
static void rmr_put_sess_iu(struct rmr_clt_pool_sess *pool_sess,
			    struct rmr_clt_sess_iu *sess_iu)
{
	rtrs_clt_put_permit(pool_sess->clt_sess->rtrs, sess_iu->permit);
	kfree(sess_iu);
}

void wake_up_iu_comp(struct rmr_clt_sess_iu *sess_iu)
{
	sess_iu->comp.errno = sess_iu->errno;
	wake_up(&sess_iu->comp.wait);
}

void msg_conf(void *priv, int errno)
{
	struct rmr_clt_sess_iu *sess_iu = (struct rmr_clt_sess_iu *)priv;

	sess_iu->errno = errno;
	/* just schedule the work because kfree must not be done here */
	schedule_work(&sess_iu->work);
}

static int send_usr_msg(struct rtrs_clt_sess *rtrs, int dir,
			struct rmr_clt_sess_iu *sess_iu,
			struct kvec *vec, size_t nr, size_t len,
			struct scatterlist *sg, unsigned int sg_len,
			void (*conf)(struct work_struct *work),
			int *errno, enum rmr_wait_type wait)
{
	int err;
	struct rtrs_clt_req_ops req_ops;

	INIT_WORK(&sess_iu->work, conf);
	req_ops = (struct rtrs_clt_req_ops){
		.priv = sess_iu,
		.conf_fn = msg_conf,
	};

	trace_send_usr_msg(dir, sess_iu);

	err = rtrs_clt_request(dir, &req_ops, rtrs, sess_iu->permit,
			       vec, nr, len, sg, sg_len);
	if (!err && wait) {
		wait_event_timeout(sess_iu->comp.wait,
				   sess_iu->comp.errno != INT_MAX,
				   msecs_to_jiffies(RMR_CLT_SEND_MSG_TIMEOUT_MS));
		*errno = sess_iu->comp.errno;
		if (*errno == INT_MAX)
			*errno = -ETIMEDOUT;
	} else {
		*errno = 0;
	}
	return err;
}

static int send_msg_rejoin_pool(struct rmr_clt_pool_sess *pool_sess, bool wait)
{
	struct rmr_msg_pool_cmd msg = {};
	struct rmr_pool *pool = pool_sess->pool;
	struct rmr_clt_sess *clt_sess = pool_sess->clt_sess;
	int ret;

	rmr_clt_init_cmd(pool, &msg);
	msg.cmd_type = RMR_CMD_REJOIN_POOL;

	msg.join_pool_cmd.rejoin = true;
	msg.join_pool_cmd.chunk_size = pool->chunk_size;
	msg.join_pool_cmd.queue_depth = clt_sess->queue_depth;

	ret = rmr_clt_pool_send_cmd(pool_sess, &msg, wait);
	if (ret)
		pr_err("%s failed\n", rmr_get_cmd_name(msg.cmd_type));

	return ret;
}

static int send_msg_join_pool(struct rmr_clt_pool_sess *pool_sess, bool create,
			      bool dirty, bool wait)
{
	struct rmr_msg_pool_cmd msg = {};
	struct rmr_pool_member_info *mem_info;
	struct rmr_pool *pool = pool_sess->pool;
	struct rmr_clt_pool_sess *t_pool_sess;
	struct rmr_clt_sess *clt_sess = pool_sess->clt_sess;
	struct rmr_dirty_id_map *map;
	int ret, i = 0, idx;

	rmr_clt_init_cmd(pool_sess->pool, &msg);
	msg.cmd_type = RMR_CMD_JOIN_POOL;

	msg.join_pool_cmd.queue_depth = clt_sess->queue_depth;
	msg.join_pool_cmd.chunk_size = pool->chunk_size;
	msg.join_pool_cmd.rejoin = false;

	if (!msg.sync) {
		msg.join_pool_cmd.create = create;
		msg.join_pool_cmd.dirty = dirty;
		mem_info = &(msg.join_pool_cmd.mem_info);

		idx = srcu_read_lock(&pool->sess_list_srcu);
		list_for_each_entry_srcu(t_pool_sess, &pool->sess_list, entry,
					 (srcu_read_lock_held(&pool->sess_list_srcu))) {
			if (t_pool_sess->member_id == pool_sess->member_id)
				continue;

			map = rmr_pool_find_map(pool, t_pool_sess->member_id);
			if (!map) {
				pr_err("%s: Map with member_id %u does not exist\n",
				       __func__, t_pool_sess->member_id);
				srcu_read_unlock(&pool->sess_list_srcu, idx);
				return -ENOENT;
			}

			mem_info->p_mem_info[i].member_id = t_pool_sess->member_id;
			/* Only relevant for create */
			if (create)
				mem_info->p_mem_info[i].c_dirty = !rmr_map_empty(map);
			i++;
			if (WARN_ON(i >= RMR_POOL_MAX_SESS))
				break;
		}
		srcu_read_unlock(&pool->sess_list_srcu, idx);
		mem_info->no_of_stor = i;
	}

	ret = rmr_clt_pool_send_cmd(pool_sess, &msg, wait);
	if (ret)
		pr_err("%s failed\n", rmr_get_cmd_name(msg.cmd_type));

	return ret;
}

int send_msg_leave_pool(struct rmr_clt_pool_sess *pool_sess, bool delete, bool wait)
{
	struct rmr_msg_pool_cmd msg = {};
	int ret;

	rmr_clt_init_cmd(pool_sess->pool, &msg);
	msg.cmd_type = RMR_CMD_LEAVE_POOL;

	msg.leave_pool_cmd.member_id = pool_sess->member_id;
	msg.leave_pool_cmd.delete = delete;

	ret = rmr_clt_pool_send_cmd(pool_sess, &msg, wait);
	if (ret)
		pr_err("%s failed\n", rmr_get_cmd_name(msg.cmd_type));

	return ret;
}

bool rmr_clt_sess_is_sync(struct rmr_clt_pool_sess *pool_sess)
{
	struct rmr_pool *pool = pool_sess->pool;
	bool ret = false;

	if (!pool) {
		WARN(1, "for sess %s pool is not assigned\n",
		     pool_sess->clt_sess->sessname);
		return false;
	}

	if (pool->sync) {
		pr_debug("sess %s pool %s is sync (internal) clt sess\n",
			 pool_sess->clt_sess->sessname, pool->poolname);
		ret = true;
	} else {
		pr_debug("sess %s pool %s is not sync clt sess\n",
			 pool_sess->clt_sess->sessname, pool->poolname);
		ret = false;
	}
	return ret;
}

/**
 * rmr_clt_send_pool_info() - Notify all other pool members of a membership change
 *
 * @pool_sess:	The pool session of the member whose state is changing.
 * @op:		Operation: %RMR_POOL_INFO_OP_ADD or %RMR_POOL_INFO_OP_REMOVE.
 * @mode:	For ADD: %RMR_POOL_INFO_MODE_CREATE or %RMR_POOL_INFO_MODE_ASSEMBLE.
 *		For REMOVE: %RMR_POOL_INFO_MODE_DELETE or %RMR_POOL_INFO_MODE_DISASSEMBLE.
 * @dirty:	When op is ADD and mode is CREATE, indicates that @pool_sess
 *		has outstanding dirty data that the receiving node must track.
 *
 * Sends a POOL_INFO command to every other non-FAILED, non-REMOVING
 * member in the pool so they can update their view of pool membership.
 *
 * Return:
 *	0 on success, negative error code on failure.
 *
 * Context:
 *	This function blocks while sending the command.
 */
static int rmr_clt_send_pool_info(struct rmr_clt_pool_sess *pool_sess,
				  enum rmr_pool_info_op op, enum rmr_pool_info_mode mode,
				  bool dirty)
{
	struct rmr_pool *pool = pool_sess->pool;
	struct rmr_clt_pool_sess *t_pool_sess;
	struct rmr_msg_pool_cmd msg = {};
	int idx, ret = 0;

	rmr_clt_init_cmd(pool, &msg);
	msg.cmd_type = RMR_CMD_POOL_INFO;

	msg.pool_info_cmd.member_id = pool_sess->member_id;
	msg.pool_info_cmd.operation = op;
	msg.pool_info_cmd.mode = mode;

	if (op == RMR_POOL_INFO_OP_ADD && mode == RMR_POOL_INFO_MODE_CREATE && dirty)
		msg.pool_info_cmd.dirty = dirty;

	idx = srcu_read_lock(&pool->sess_list_srcu);
	list_for_each_entry_srcu(t_pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		enum rmr_clt_pool_sess_state state;

		/*
		 * No need to send the info message to the member who just joined.
		 */
		if (t_pool_sess->member_id == pool_sess->member_id)
			continue;

		state = atomic_read(&t_pool_sess->state);
		/*
		 * TODO: For FAILED session we have to store the missed
		 * msgs and send them later when the session recovers.
		 */
		if (state == RMR_CLT_POOL_SESS_FAILED ||
		    state == RMR_CLT_POOL_SESS_REMOVING)
			continue;

		ret = rmr_clt_pool_send_cmd(t_pool_sess, &msg, WAIT);
		if (ret) {
			pr_err("%s failed with err %d\n", rmr_get_cmd_name(msg.cmd_type), ret);
			break;
		}
	}
	srcu_read_unlock(&pool->sess_list_srcu, idx);

	return ret;
}

void resend_join_pool(struct rmr_clt_sess *clt_sess)
{
	struct rmr_clt_pool_sess *pool_sess;

	mutex_lock(&clt_sess->lock);

	list_for_each_entry(pool_sess, &clt_sess->pool_sess_list, clt_sess_entry) {
		int err;

		err = send_msg_rejoin_pool(pool_sess, WAIT);
		if (err) {
			pr_err("send_msg_rejoin_pool failed for sess %s error %d\n",
				pool_sess->sessname, err);
		}
	}
	mutex_unlock(&clt_sess->lock);

	return;
}

int send_msg_enable_pool(struct rmr_clt_pool_sess *pool_sess, bool enable)
{
	struct rmr_msg_pool_cmd msg = {};
	int ret;

	rmr_clt_init_cmd(pool_sess->pool, &msg);
	msg.cmd_type = RMR_CMD_ENABLE_POOL;

	msg.enable_pool_cmd.enable = enable;

	ret = rmr_clt_pool_send_cmd(pool_sess, &msg, WAIT);
	if (ret) {
		pr_err("%s failed\n", rmr_get_cmd_name(msg.cmd_type));
		goto err;
	}

err:
	return ret;
}

static const char *rmr_clt_pool_sess_state_names[] = {
	[0] = "invalid state",
	[RMR_CLT_POOL_SESS_CREATED] = "created",
	[RMR_CLT_POOL_SESS_NORMAL] = "normal",
	[RMR_CLT_POOL_SESS_FAILED] = "failed",
	[RMR_CLT_POOL_SESS_RECONNECTING] = "reconnecting",
	[RMR_CLT_POOL_SESS_REMOVING] = "removing"
};

const char *rmr_clt_sess_state_str(enum rmr_clt_pool_sess_state state)
{
	return rmr_clt_pool_sess_state_names[state];
}

int rmr_clt_reconnect_sess(struct rmr_clt_sess *clt_sess,
			   const struct rtrs_addr *paths,
			   size_t path_cnt)
{
	struct rtrs_attrs attrs;
	struct rtrs_clt_ops rtrs_ops;
	int err = 0;

	rtrs_ops = (struct rtrs_clt_ops){
		.priv = clt_sess,
		.link_ev = rmr_clt_link_ev,
	};

	clt_sess->rtrs = rtrs_clt_open(&rtrs_ops, clt_sess->sessname,
				   paths, path_cnt, RTRS_PORT,
				   0, /* Do not use pdu of rtrs */
				   RECONNECT_DELAY,
				   MAX_RECONNECTS, 0);
	if (IS_ERR(clt_sess->rtrs)) {
		err = PTR_ERR(clt_sess->rtrs);
		pr_err("rtrs_clt_open error %d\n", err);
		goto err;
	}

	err = rtrs_clt_query(clt_sess->rtrs, &attrs);
	if (unlikely(err)) {
		pr_err("rtrs_clt_query error %d\n", err);
		goto close_sess;
	}
	clt_sess->max_io_size = attrs.max_io_size;
	clt_sess->queue_depth = attrs.queue_depth;
	clt_sess->max_segments = attrs.max_segments;

	clt_sess->state = RMR_CLT_SESS_CONNECTED;

	resend_join_pool(clt_sess);

	return err;

close_sess:
	rtrs_clt_close(clt_sess->rtrs);
err:
	return err;
}

//TODO: we do not use rsp in this function, do we need it as an argument?
static int rmr_clt_handle_rejoin_rsp(struct rmr_clt_pool_sess *pool_sess, struct rmr_msg_pool_cmd_rsp *rsp)
{
	int err = 0;

	if (rmr_clt_sess_is_sync(pool_sess)) {
		/*
		 * The client on sync side does not need map update
		 * hence goes to "normal" state directly.
		 * NB: FAILED => NORMAL
		 */
		pool_sess_change_state(pool_sess, RMR_CLT_POOL_SESS_NORMAL);
	} else {
		/*
		 * The client on non-sync side needs map update,
		 *
		 * A map update is to be triggered, which updates the map,
		 * and then sets state to "normal"
		 */
		pool_sess_change_state(pool_sess, RMR_CLT_POOL_SESS_RECONNECTING);

		/*
		 * Send the info about the pool to all the storages.
		 * Contains IDs of storages connected to this pool.
		 */
		err = rmr_clt_send_pool_info(pool_sess, RMR_POOL_INFO_OP_ADD,
					     RMR_POOL_INFO_MODE_ASSEMBLE, false);
		if (err) {
			pr_err("Rejoin: rmr_clt_send_pool_info failed for session %s",
			       pool_sess->sessname);
			return -EINVAL;
		}

		err = rmr_clt_pool_try_enable(pool_sess->pool);
		if (err)
			pr_err("%s: pool %s try_enable failed for sess %s: %d\n",
			       __func__, pool_sess->pool->poolname,
			       pool_sess->sessname, err);
	}

	return err;
}

static void rmr_clt_handle_join_rsp(struct rmr_clt_pool_sess *pool_sess,
				    struct rmr_msg_pool_cmd_rsp *rsp)
{
	struct rmr_pool *pool = pool_sess->pool;
	struct rmr_pool_md *clt_md;
	u64 mapped_size;

	clt_md = &pool->pool_md;

	pool_sess->ver = min_t(u8, rsp->ver, RMR_PROTO_VER_MAJOR);
	pool_sess->member_id = rsp->member_id;
	xa_store(&pool->stg_members, pool_sess->member_id, pool_sess, GFP_KERNEL);

	pool->chunk_size = rsp->join_pool_cmd_rsp.chunk_size;
	pool->chunk_size_shift = ilog2(pool->chunk_size);
	clt_md->chunk_size = pool->chunk_size;

	mapped_size  = rsp->join_pool_cmd_rsp.mapped_size;
	if (mapped_size) {
		pool->mapped_size = mapped_size;
		pool->pool_md.mapped_size = mapped_size;
		rmr_pool_update_no_of_chunk(pool);
		pr_info("clt join_pool: mapped size %llu\n", pool->mapped_size);
	}
}

static int cmd_process_rsp(struct rmr_clt_pool_sess *pool_sess, struct rmr_msg_pool_cmd_rsp *rsp)
{
	int err = 0;

	pr_debug("rsp, cmd_type %d, member_id %d, err %d\n",
		 rsp->cmd_type, rsp->member_id, rsp->err);

	if (rsp->err)
		return rsp->err;

	switch (rsp->cmd_type) {
	case RMR_CMD_MAP_CHECK:
		return rmr_clt_handle_map_check_rsp(pool_sess, rsp);
	case RMR_CMD_STORE_CHECK:
		return rmr_clt_handle_store_check_rsp(pool_sess, rsp);
	case RMR_CMD_MAP_READY:
	case RMR_CMD_MAP_SEND:
	case RMR_CMD_MAP_BUF_DONE:
	case RMR_CMD_MAP_DONE:
	case RMR_CMD_MAP_DISABLE:
	case RMR_CMD_LEAVE_POOL:
	case RMR_CMD_LAST_IO_TO_MAP:
	case RMR_CMD_MD_SEND:
	case RMR_CMD_MAP_SET_VER:
	case RMR_CMD_SEND_DISCARD:
	case RMR_CMD_DISCARD_CLEAR_FLAG:
	case RMR_CMD_POOL_INFO:
		pr_debug("%s: No rsp handling for %s\n", __func__, rmr_get_cmd_name(rsp->cmd_type));
		break;
	case RMR_CMD_REJOIN_POOL:
		return rmr_clt_handle_rejoin_rsp(pool_sess, rsp);
	case RMR_CMD_JOIN_POOL:
		rmr_clt_handle_join_rsp(pool_sess, rsp);
		break;
	case RMR_CMD_ENABLE_POOL:
		pool_sess->ver = min_t(u8, rsp->ver, RMR_PROTO_VER_MAJOR);
		break;
	default:
		pr_warn("%s: switch default type: %d\n", __func__, rsp->cmd_type);

		err = -EINVAL;
	}

	return err;
}

static void msg_pool_cmd_conf(struct work_struct *work)
{
	struct rmr_clt_sess_iu *sess_iu = container_of(work, struct rmr_clt_sess_iu, work);
	struct rmr_msg_pool_cmd_rsp *rsp = sess_iu->buf;
	struct rmr_clt_pool_sess *pool_sess = sess_iu->pool_sess;

	pr_debug("pool cmd for %s session %s member_id %d conf with errno %d\n",
		 pool_sess->pool->poolname, pool_sess->sessname,
		 pool_sess->member_id, sess_iu->errno);

	if (!sess_iu->errno) {
		/*
		 * We need to check if there was an error while processing the cmd
		 * on the server side. If there was, then we fail the command.
		 */
		sess_iu->errno = cmd_process_rsp(pool_sess, rsp);
	}

	kfree(rsp);
	wake_up_iu_comp(sess_iu);
	rmr_msg_put_iu(pool_sess, sess_iu);
}

void rmr_clt_init_cmd(struct rmr_pool *pool, struct rmr_msg_pool_cmd *msg)
{
	memset(msg, 0, sizeof(*msg));

	msg->hdr.group_id = cpu_to_le32(pool->group_id);
	msg->hdr.type = cpu_to_le16(RMR_MSG_CMD);
	msg->hdr.__padding = 0;
	msg->ver = RMR_PROTO_VER_MAJOR;
	msg->sync = pool->sync;

	strncpy(msg->pool_name, pool->poolname, sizeof(msg->pool_name));
}
EXPORT_SYMBOL(rmr_clt_init_cmd);

int rmr_clt_pool_send_cmd(struct rmr_clt_pool_sess *pool_sess,
			  struct rmr_msg_pool_cmd *msg, bool wait)
{
	struct rmr_clt_sess *clt_sess = pool_sess->clt_sess;
	struct rmr_msg_pool_cmd_rsp *rsp;
	struct rmr_clt_sess_iu *sess_iu;
	struct kvec vec = {
		.iov_base = msg,
		.iov_len = sizeof(*msg)
	};
	int err, errno;

	rsp = kzalloc(sizeof(*rsp), GFP_KERNEL);
	if (unlikely(!rsp))
		return -ENOMEM;

	sess_iu = rmr_msg_get_iu(pool_sess, RTRS_ADMIN_CON, RTRS_PERMIT_WAIT, 2);
	if (unlikely(!sess_iu)) {
		kfree(rsp);
		return -ENOMEM;
	}

	sess_iu->buf = rsp;
	sg_init_one(&sess_iu->sg, rsp, sizeof(*rsp));

	err = send_usr_msg(clt_sess->rtrs, READ, sess_iu,
			   &vec, 1, sizeof(*rsp), &sess_iu->sg, 1,
			   msg_pool_cmd_conf, &errno, wait);
	if (unlikely(err)) {
		rmr_msg_put_iu(pool_sess, sess_iu);
		kfree(rsp);
	} else {
		err = errno;
	}

	rmr_msg_put_iu(pool_sess, sess_iu);

	return err;
}

/*
 * Pre-requisite: rcu read lock should be held by caller
 */
static struct rmr_clt_pool_sess *
rmr_clt_get_first_normal_session(struct rmr_pool *pool)
{
	struct rmr_clt_pool_sess *pool_sess;

	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		if (atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_NORMAL)
			return pool_sess;
	}

	return NULL;
}

/**
 * rmr_clt_pool_send_all - Send a command to all sessions in the pool
 *
 * @pool:	The client pool which sends the command message
 * @msg:	The command message of pool
 *
 * Description:
 *	When sending messages to all pool sessions, it will continue to send
 *	regardless of the failure of the previous communication.
 *
 * Return:
 *	0 if at least one successful request
 *	less than 0 if all requests failed
 */
int rmr_clt_pool_send_all(struct rmr_pool *pool, struct rmr_msg_pool_cmd *msg)
{
	struct rmr_clt_pool_sess *pool_sess;
	int idx, err = 0;
	u8 member_id = 0;
	int ret = 0;

	if (msg->cmd_type == RMR_CMD_SEND_DISCARD)
		member_id = msg->send_discard_cmd.member_id;

	idx = srcu_read_lock(&pool->sess_list_srcu);
	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		/* The node has had discards. */
		if (pool_sess->member_id == member_id)
			continue;

		if (atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_FAILED)
			continue;

		pr_info("pool %s send cmd %d to sess %s\n",
			pool->poolname, msg->cmd_type, pool_sess->sessname);

		/* The err code reflects the response from this pool_sess. */
		err = rmr_clt_pool_send_cmd(pool_sess, msg, WAIT);
		if (err) {
			pr_err("pool %s sending cmd to sess %s failed, err=%d\n",
			       pool->poolname, pool_sess->sessname, err);
			continue;
		}

		pr_info("pool %s done sending cmd %d to sess %s\n",
			pool->poolname, msg->cmd_type, pool_sess->sessname);
		ret++;
	}
	srcu_read_unlock(&pool->sess_list_srcu, idx);

	if (ret)
		return 0;

	return -ENETUNREACH;
}
EXPORT_SYMBOL(rmr_clt_pool_send_all);

/**
 * rmr_clt_send_cmd_with_data_all - Send a command with data to all sessions in the pool
 *
 * Return:
 *	0 on success of all sends
 *	less than 0 if all sends failed
 *	positive number of failed sends
 */
int rmr_clt_send_cmd_with_data_all(struct rmr_pool *pool, struct rmr_msg_pool_cmd *msg,
				   void *buf, unsigned int buflen)
{
	struct rmr_clt_pool_sess *pool_sess;
	int idx, err = 0;
	bool ret = false;
	int errno = 0;

	idx = srcu_read_lock(&pool->sess_list_srcu);
	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				(srcu_read_lock_held(&pool->sess_list_srcu))) {
		if (atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_FAILED) {
			errno++;
			continue;
		}

		pr_debug("pool %s send cmd %d to sess %s\n",
			 pool->poolname, msg->cmd_type, pool_sess->sessname);
		err = rmr_clt_send_cmd_with_data(pool, pool_sess, msg, buf, buflen);
		if (err) {
			errno++;
			pr_debug("pool %s sending cmd to sess %s failed, err=%d\n",
				 pool->poolname, pool_sess->sessname, err);
			continue;
		}

		pr_debug("pool %s done sending cmd %d to sess %s\n",
			 pool->poolname, msg->cmd_type, pool_sess->sessname);
		ret = true;
	}
	srcu_read_unlock(&pool->sess_list_srcu, idx);

	if (ret)
		return errno;

	return -EINVAL;
}
EXPORT_SYMBOL(rmr_clt_send_cmd_with_data_all);

/**
 * rmr_clt_start_last_io_update() - Do the last IO update
 *
 * @pool:		The pool
 *
 * Description:
 *	Last IO update is needed in case a pserver went down while connected to a pool.
 *	A pserver going down while performing IOs could mean that some IOs could have been
 *	executed in some nodes but not all. This function takes the last 'queue_depth' number of
 *	IOs on each storage node and makes sure they are synced in between all the nodes.
 *	Before performing the last IO conversion, it also makes sure that all the storage nodes
 *	have the lastest map.
 *
 * Return:
 *	0 on success
 *	Error value on failure
 *
 * Context:
 *	srcu_read_lock should be held while calling this function.
 */
int rmr_clt_start_last_io_update(struct rmr_pool *pool)
{
	struct rmr_clt_pool_sess *pool_sess_chosen, *pool_sess;
	struct rmr_msg_pool_cmd msg = {};
	u64 map_ver, highest_map_ver = 0;
	int j, err, idx, ret = 0;
	int discard_ids[RMR_POOL_MAX_SESS];
	u8 id, nr_discards = 0;

	idx = srcu_read_lock(&pool->sess_list_srcu);

	for (j = 0; j < RMR_POOL_MAX_SESS; j++) {
		struct rmr_clt_pool_sess *ps;
		u8 mid = pool->pool_md.srv_md[j].member_id;

		if (!mid)
			continue;

		ps = xa_load(&pool->stg_members, mid);
		if (!ps) {
			pr_err("%s: member_id %u not yet assembled\n",
			       __func__, mid);
			err = -EINVAL;
			goto out;
		}
		if (atomic_read(&ps->state) != RMR_CLT_POOL_SESS_RECONNECTING) {
			pr_err("%s: member_id %u not in reconnecting state\n",
			       __func__, mid);
			err = -EINVAL;
			goto out;
		}
	}

	/*
	 * Before pserver died, it could be that one or more storage nodes were down.
	 * This would mean there is a possibility that those storage nodes will not have
	 * the latest map. But that can create problems.
	 * We need to make sure that every storage node has the latest map.
	 * Hence, find out which node has the latest map first,
	 */
	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		err = send_map_get_version(pool_sess, &map_ver);
		if (err) {
			pr_err("%s: Failed to read map version for sess %s\n",
			       __func__, pool_sess->sessname);
			err = -EINVAL;
			goto out;
		}

		if (RMR_STORE_IS_REPLACE(map_ver)) {
			map_ver = RMR_STORE_GET_VER(map_ver);
			discard_ids[nr_discards] = pool_sess->member_id;
			nr_discards++;
		}

		if (map_ver > highest_map_ver) {
			highest_map_ver = map_ver;
			pool_sess_chosen = pool_sess;
		}
	}

	for (j = 0; j < nr_discards; j++) {
		id = discard_ids[j];
		pr_info("%s: Send discard req %d to S%d\n",
			__func__, id, pool_sess_chosen->member_id);
		err = send_discard(pool_sess_chosen, RMR_CMD_SEND_DISCARD, id);
		if (err) {
			pr_err("%s: Failed to send discard request to %s\n",
			       __func__, pool_sess_chosen->sessname);
			goto out;
		}
	}

	/*
	 * We have the storage node with the latest map,
	 * make sure the latest map is sent to all other storage nodes.
	 */
	err = rmr_clt_spread_map(pool, pool_sess_chosen, false, false);
	if (err) {
		pr_err("%s: Failed to spread the latest map\n", __func__);
		goto out;
	}

	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		for (j = 0; j < nr_discards; j++) {
			id = discard_ids[j];
			pr_info("%s: Send discard clear req %d to S%d\n",
				__func__, id, pool_sess->member_id);
			err = send_discard(pool_sess, RMR_CMD_DISCARD_CLEAR_FLAG, id);
			if (err) {
				pr_err("%s: Failed to clear discard state on %s\n",
				       __func__, pool_sess->sessname);
			} else {
				ret++;
			}
		}
	}

	if (nr_discards && !ret) {
		pr_err("%s: Failed to clear discard state on any storage node\n", __func__);
		err = -EINVAL;
		goto out;
	}

	/*
	 * Now that we are done with the dispersing of the latest map,
	 * we can start last IO update.
	 */
	rmr_clt_init_cmd(pool, &msg);
	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		msg.cmd_type = RMR_CMD_LAST_IO_TO_MAP;
		err = rmr_clt_pool_send_cmd(pool_sess, &msg, WAIT);
		if (err) {
			pr_err("%s: %s failed\n", __func__, rmr_get_cmd_name(msg.cmd_type));
			goto out;
		}

		err = rmr_clt_spread_map(pool, pool_sess, true, false);
		if (err) {
			pr_err("%s: Failed to spread last_io converted map\n", __func__);
			goto out;
		}
	}

	err = rmr_clt_read_map(pool);
	if (err) {
		pr_err("%s: rmr_clt_read_map failed with err %d\n", __func__, err);
		goto out;
	}

out:
	srcu_read_unlock(&pool->sess_list_srcu, idx);
	return err;
}

/**
 * rmr_clt_enable_sess() - Enable the rmr clt pool sessions
 *
 * @pool_sess:	The rmr clt pool session to enable
 *
 * Description:
 *	This function takes care of enable request, for pool sessions
 *	not in maintenance mode and in mm.
 *
 * Return:
 *	0 on success
 *	Error value on failure
 */
int rmr_clt_enable_sess(struct rmr_clt_pool_sess *pool_sess)
{
	struct rmr_pool *pool = pool_sess->pool;
	int pool_sess_state, err = 0;

	pr_info("%s: For session %s of pool %s\n",
		__func__, pool_sess->sessname, pool->poolname);

	if (!pool_sess->maintenance_mode) {
		/*
		 * Simple enable, not related to maintenance.
		 * Manual enable is only allowed for sessions in "created" state
		 */
		pool_sess_state = atomic_read(&pool_sess->state);
		if (pool_sess_state != RMR_CLT_POOL_SESS_CREATED) {
			pr_err("Cannot manually enable session: state %d\n", pool_sess_state);
			err = -EINVAL;
			goto out;
		}

		err = send_msg_enable_pool(pool_sess, 1);
		if (err) {
			pr_err("Failed to send enable to pool %s. Err %d\n",
			       pool->poolname, err);
			goto out;
		}

		pool_sess_change_state(pool_sess, RMR_CLT_POOL_SESS_NORMAL);
	} else {
		/*
		 * Enable when in maintenance mode.
		 */
		err = rmr_clt_unset_pool_sess_mm(pool_sess);
	}

out:
	return err;
}

/**
 * rmr_clt_create_sess() - allocate and initialize rmr client session, rmr_clt_pool sess can use it
 * to submit io to the rtrs connection
 *
 * @sessname:	Name to be given to the new session being created.
 * @paths:	RTRS paths created for the session.
 * @path_cnt:	Number of paths.
 *
 * Return:
 *	Pointer to rmr_clt_sess on success
 *	ERR_PTR on failure
 *
 * Description:
 *	Create a new session to storage node with address "rtrs_addr".
 *	After this function is done, rmr_clt_pool_sess caan use this sess to submit io
 *
 * Context:
 *	This function blocks while creating the session
 */
static struct rmr_clt_sess *rmr_clt_create_sess(const char *sessname,
						const struct rtrs_addr *paths,
						size_t path_cnt)
{
	struct rmr_clt_sess *clt_sess;
	struct rtrs_attrs attrs;
	struct rtrs_clt_ops rtrs_ops;
	int err;

	clt_sess = alloc_clt_sess(sessname);
	if (IS_ERR(clt_sess)) {
		pr_err("Session '%s' can not be allocated in pool\n", sessname);
		return clt_sess; // TODO: isit err_cast here?
	}

	rtrs_ops = (struct rtrs_clt_ops){
		.priv = clt_sess,
		.link_ev = rmr_clt_link_ev,
	};
	/*
	 * Nothing was found, establish rtrs connection and proceed further.
	 */
	clt_sess->rtrs = rtrs_clt_open(&rtrs_ops, sessname,
				       paths, path_cnt, RTRS_PORT,
				       0, /* Do not use pdu of rtrs */
				       RECONNECT_DELAY,
				       MAX_RECONNECTS, 0);
	if (IS_ERR(clt_sess->rtrs)) {
		err = PTR_ERR(clt_sess->rtrs);
		pr_err("rtrs_clt_open error %d\n", err);
		goto free_clt_sess;
	}
	err = rtrs_clt_query(clt_sess->rtrs, &attrs);
	if (unlikely(err)) {
		pr_err("rtrs_clt_query error %d\n", err);
		goto close_sess;
	}
	clt_sess->max_io_size = attrs.max_io_size;
	clt_sess->queue_depth = attrs.queue_depth;
	clt_sess->max_segments = attrs.max_segments;
	//sess->sess_kobj = &sess->rtrs->dev.dev.kobj;

	err = rmr_clt_create_clt_sess_sysfs_files(clt_sess);
	if (err) {
		pr_err("failed to crete sysfs files for sess %s, err=%d\n",
		       clt_sess->sessname, err);
		goto close_sess;
	}
	clt_sess->state = RMR_CLT_SESS_CONNECTED;

	mutex_lock(&g_sess_lock);
	list_add(&clt_sess->g_list, &g_sess_list);
	mutex_unlock(&g_sess_lock);

	return clt_sess;

close_sess:
	rtrs_clt_close(clt_sess->rtrs);

free_clt_sess:
	kfree(clt_sess);

	return ERR_PTR(err);
}

/**
 * rmr_clt_pool_try_enable() - Trigger pool session recovery if conditions are met
 *
 * @pool:	The pool to check
 *
 * Scans pool sessions and fires the appropriate recovery action:
 *
 *  Case 1: ≥1 NORMAL session exists → spread its map (with enable=true) to all
 *          non-NORMAL sessions, then set them to NORMAL on the client side
 *  Case 2: Exactly one was_last_authoritative RECONNECTING session exists →
 *          enable it directly (data is complete, no map needed), then spread
 *          its map to remaining sessions
 *  Cases 3/4: All pool_md members present and RECONNECTING → last_io_update
 *
 * Return: 0 on success or when conditions are not yet met, negative error on failure.
 */
int rmr_clt_pool_try_enable(struct rmr_pool *pool)
{
	struct rmr_clt_pool *clt_pool = (struct rmr_clt_pool *)pool->priv;
	struct rmr_clt_pool_sess *pool_sess, *normal_sess, *auth_sess;
	bool any_member = false;
	int idx, j, err = 0;

	pr_info("%s: Started for pool %s\n", __func__, pool->poolname);

	/*
	 * clt_pool_lock is held across all RPC round-trips below (MAP_READY,
	 * MAP_SEND, MAP_DONE, last_io_update exchanges).  This serialises
	 * concurrent try_enable calls and prevents rmr_clt_open/close from
	 * racing with recovery.  The RPC send path (rmr_clt_pool_send_cmd)
	 * uses per-session permits and does not acquire clt_pool_lock, so
	 * there is no deadlock.  rmr_clt_open and rmr_clt_close use
	 * mutex_trylock and mutex_lock respectively to handle this.
	 */
	mutex_lock(&clt_pool->clt_pool_lock);

	normal_sess = NULL;
	auth_sess = NULL;

	idx = srcu_read_lock(&pool->sess_list_srcu);
	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		int state = atomic_read(&pool_sess->state);

		if (state == RMR_CLT_POOL_SESS_NORMAL) {
			if (!normal_sess)
				normal_sess = pool_sess;
		} else if (state == RMR_CLT_POOL_SESS_RECONNECTING &&
			   pool_sess->was_last_authoritative &&
			   !pool_sess->maintenance_mode &&
			   !auth_sess) {
			auth_sess = pool_sess;
		}
	}
	srcu_read_unlock(&pool->sess_list_srcu, idx);

	/*
	 * Invariant: at most one was_last_authoritative session can exist
	 * (guaranteed by atomic_dec_and_test in pool_sess_change_state), and
	 * it cannot coexist with a NORMAL session (if a NORMAL session exists,
	 * the pool never fully went to FAILED, so no session gets the flag).
	 */
	if (WARN_ON(auth_sess && normal_sess)) {
		err = -EINVAL;
		goto out;
	}

	/* Case 2: was_last_authoritative session — enable it directly, then spread */
	if (auth_sess) {
		err = send_msg_enable_pool(auth_sess, 1);
		if (err) {
			pr_err("%s: pool %s failed to enable auth sess %s: %d\n",
			       __func__, pool->poolname, auth_sess->sessname, err);
			goto out;
		}
		pool_sess_change_state(auth_sess, RMR_CLT_POOL_SESS_NORMAL);
		normal_sess = auth_sess;
	}

	/* Case 1: ≥1 NORMAL session → spread map to all non-NORMAL sessions */
	if (normal_sess) {
		idx = srcu_read_lock(&pool->sess_list_srcu);
		err = rmr_clt_spread_map(pool, normal_sess, true, true);
		if (err)
			pr_err("%s: pool %s spread map from %s failed: %d\n",
			       __func__, pool->poolname, normal_sess->sessname, err);
		else
			goto out_normal;

		srcu_read_unlock(&pool->sess_list_srcu, idx);
		goto out;
	}

	/* Cases 3/4: all pool_md members present and RECONNECTING */
	for (j = 0; j < RMR_POOL_MAX_SESS; j++) {
		struct rmr_clt_pool_sess *ps;
		u8 mid = pool->pool_md.srv_md[j].member_id;

		if (!mid)
			continue;

		any_member = true;
		ps = xa_load(&pool->stg_members, mid);
		if (!ps || atomic_read(&ps->state) != RMR_CLT_POOL_SESS_RECONNECTING ||
		     ps->maintenance_mode) {
			pr_info("%s: pool %s member_id %u not yet in reconnecting/mm, waiting\n",
				__func__, pool->poolname, mid);
			goto out;
		}
	}

	if (!any_member) {
		pr_info("%s: pool %s has no members in pool_md, nothing to do\n",
			__func__, pool->poolname);
		goto out;
	}

	pr_info("%s: pool %s all members reconnecting, starting last_io_update\n",
		__func__, pool->poolname);

	err = rmr_clt_start_last_io_update(pool);
	if (err) {
		pr_err("%s: pool %s last_io_update failed: %d\n",
		       __func__, pool->poolname, err);
		goto out;
	}

	idx = srcu_read_lock(&pool->sess_list_srcu);
out_normal:
	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		if (atomic_read(&pool_sess->state) != RMR_CLT_POOL_SESS_RECONNECTING ||
		    pool_sess->maintenance_mode)
			continue;

		pool_sess_change_state(pool_sess, RMR_CLT_POOL_SESS_NORMAL);
	}
	srcu_read_unlock(&pool->sess_list_srcu, idx);

out:
	mutex_unlock(&clt_pool->clt_pool_lock);
	return err;
}

/**
 * rmr_clt_read_pool_md() - Read the full pool_md from a storage server's disk
 *
 * @pool_sess:	The pool session to read from.
 *
 * Sends RMR_CMD_MD_SEND with read_full_md=1 to the given session and imports
 * the returned srv_md[] entries into pool->pool_md, skipping already-known
 * members.  Used during add_sess mode=assemble so the client learns all pool
 * member IDs from the server's on-disk metadata, not only the one being
 * assembled.
 *
 * Return:
 *	0 on success, negative error code on failure.
 */
static int rmr_clt_read_pool_md(struct rmr_clt_pool_sess *pool_sess, bool first)
{
	struct rmr_pool *pool = pool_sess->pool;
	struct rmr_msg_pool_cmd msg = {};
	struct rmr_pool_md *remote_md;
	int i, err;

	remote_md = kzalloc(sizeof(*remote_md), GFP_KERNEL);
	if (!remote_md)
		return -ENOMEM;

	rmr_clt_init_cmd(pool, &msg);
	msg.cmd_type = RMR_CMD_MD_SEND;
	msg.md_send_cmd.src_mapped_size = pool->mapped_size;
	msg.md_send_cmd.sender_id = pool_sess->member_id;
	msg.md_send_cmd.read_full_md = 1;

	err = rmr_clt_send_cmd_with_data(pool, pool_sess, &msg,
					 remote_md, sizeof(*remote_md));
	if (err) {
		pr_err("%s: failed to read pool_md from sess %s: %d\n",
		       __func__, pool_sess->sessname, err);
		goto out;
	}

	for (i = 0; i < RMR_POOL_MAX_SESS; i++) {
		u8 mid = remote_md->srv_md[i].member_id;
		int idx;

		if (!mid)
			continue;

		idx = rmr_pool_find_md(&pool->pool_md, mid, first);
		if (idx < 0)
			continue;

		if (!pool->pool_md.srv_md[idx].member_id) {
			/* New entry — import blindly */
			memcpy(&pool->pool_md.srv_md[idx], &remote_md->srv_md[i],
			       sizeof(struct rmr_srv_md));
		} else {
			/* Already known — verify stable fields are consistent */
			if (pool->pool_md.srv_md[idx].mapped_size !=
			    remote_md->srv_md[i].mapped_size)
				pr_warn("%s: member_id %u mapped_size mismatch: "
					"expected %llu, got %llu from sess %s\n",
					__func__, mid,
					pool->pool_md.srv_md[idx].mapped_size,
					remote_md->srv_md[i].mapped_size,
					pool_sess->sessname);
		}
	}

out:
	kfree(remote_md);
	return err;
}

/**
 * rmr_clt_process_non_sync_sess() - Set up map and notify peers for a new non-sync session
 *
 * @pool_sess:	The newly added pool session.
 * @create:	True if this is a fresh pool creation; false for an assemble of an
 *		existing pool.
 * @dirty:	True if there are already other sessions in the pool; the new member's
 *		map will be marked fully dirty to trigger a resync.
 *
 * Creates the dirty map for @pool_sess and informs all existing pool members
 * about the new storage node joining.  On failure the map is removed.
 *
 * Return:
 *	0 on success, negative error code on failure.
 */
static int rmr_clt_process_non_sync_sess(struct rmr_clt_pool_sess *pool_sess, bool create,
					 bool dirty)
{
	struct rmr_pool *pool = pool_sess->pool;
	struct rmr_dirty_id_map *map;
	enum rmr_pool_info_mode mode;
	u8 created_mids[RMR_POOL_MAX_SESS];
	int created_cnt = 0;
	int i, err = 0;

	/*
	 * The mapped size of the pool is set after a backend device is mapped to the
	 * client. If a new client pool session is extended to this pool, the map for that
	 * new server node needs to be created for the client pool as well.
	 */
	if (!pool->mapped_size) {
		pr_err("%s: pool %s mapped_size is 0\n",
		       __func__, pool->poolname);
		err = -EINVAL;
		goto out;
	}

	pr_info("Through add_sess, pool %s mapped_size %llu\n",
		pool->poolname, pool->mapped_size);

	rmr_pool_update_no_of_chunk(pool);

	if (create) {
		if (rmr_pool_find_map(pool, pool_sess->member_id)) {
			pr_err("%s: pool %s map for member_id %u already exists\n",
			       __func__, pool->poolname, pool_sess->member_id);
			err = -EEXIST;
			goto out;
		}

		map = rmr_map_create(pool, pool_sess->member_id);
		if (IS_ERR(map)) {
			err = PTR_ERR(map);
			pr_err("%s: pool %s failed to create map for member_id %u\n",
			       __func__, pool->poolname, pool_sess->member_id);
			goto out;
		}

		/*
		 * During pool creation, all storage nodes must start with identical
		 * data. The first node added is taken as the clean reference; any
		 * subsequent node joining must be fully synced from it.
		 * Mark the entire map dirty to trigger that initial resync.
		 */
		if (dirty)
			rmr_map_set_dirty_all(map, MAP_NO_FILTER);

		mode = RMR_POOL_INFO_MODE_CREATE;
	} else {
		/*
		 * For assemble, read pool_md first so we know all member IDs,
		 * then create maps for every member in the pool.
		 */
		mode = RMR_POOL_INFO_MODE_ASSEMBLE;

		err = rmr_clt_read_pool_md(pool_sess, !dirty);
		if (err) {
			pr_err("%s: failed to read pool_md from sess %s: %d\n",
			       __func__, pool_sess->sessname, err);
			goto out;
		}

		if (!dirty) {
			for (i = 0; i < RMR_POOL_MAX_SESS; i++) {
				u8 mid = pool->pool_md.srv_md[i].member_id;

				if (!mid)
					continue;

				map = rmr_map_create(pool, mid);
				if (IS_ERR(map)) {
					err = PTR_ERR(map);
					pr_err("%s: pool %s failed to create map for member_id %u\n",
					       __func__, pool->poolname, mid);
					goto del_maps;
				}
				created_mids[created_cnt++] = mid;
			}
		}
	}

	/*
	 * We need to send the info about this node joining to other storage nodes.
	 */
	err = rmr_clt_send_pool_info(pool_sess, RMR_POOL_INFO_OP_ADD, mode, dirty);
	if (err) {
		pr_err("rmr_clt_send_pool_info failed for session %s\n",
		       pool_sess->sessname);
		if (create)
			rmr_pool_remove_map(pool, pool_sess->member_id);
		else
			goto del_maps;
		goto out;
	}

	if (!create) {
		pool_sess_change_state(pool_sess, RMR_CLT_POOL_SESS_RECONNECTING);
		err = rmr_clt_pool_try_enable(pool);
		if (err)
			pr_err("%s: pool %s try_enable failed for sess %s: %d\n",
			       __func__, pool->poolname, pool_sess->sessname, err);
	}

	return err;

del_maps:
	for (i = 0; i < created_cnt; i++)
		rmr_pool_remove_map(pool, created_mids[i]);
out:
	return err;
}

/**
 * rmr_clt_add_pool_sess() - Add a client session to an RMR pool
 *
 * @pool:	The pool to join.
 * @clt_sess:	The client transport session to associate.
 * @create:	True if this is a fresh pool creation; false for an assemble of an
 *		existing pool.
 *
 * Sends a join_pool command to the server, allocates a pool session, creates
 * the dirty map for this storage node (for non-sync pools), and notifies the
 * other pool members via a pool_info message.
 *
 * Return:
 *	Pointer to the new pool session on success, ERR_PTR on failure.
 */
struct rmr_clt_pool_sess *rmr_clt_add_pool_sess(struct rmr_pool *pool,
						struct rmr_clt_sess *clt_sess, bool create)
{
	struct rmr_clt_pool *clt_pool;
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_pool_md *clt_md;
	int err, idx;
	bool dirty = false;

	mutex_lock(&pool->sess_lock);

	if (__find_sess_by_name(pool, clt_sess->sessname)) {
		pr_err("Session '%s' already exists in pool %s\n",
		       clt_sess->sessname, pool->poolname);
		err = -EEXIST;
		goto err_out;
	}

	pool_sess = alloc_pool_sess(pool, clt_sess);
	if (IS_ERR(pool_sess)) {
		pr_err("pool session '%s' can not be allocated in pool %s\n",
		       clt_sess->sessname, pool->poolname);
		err = PTR_ERR(pool_sess);
		goto err_out;
	}

	clt_pool = (struct rmr_clt_pool *)pool->priv;

	/* TODO handle case where tags are alreaydy initialized */
	clt_pool->queue_depth = clt_sess->queue_depth;
	clt_md = &clt_pool->pool->pool_md;
	clt_md->queue_depth = clt_sess->queue_depth;

	if (!pool->sync)
		dirty = !list_empty(&pool->sess_list);

	err = send_msg_join_pool(pool_sess, create, dirty, WAIT);
	if (unlikely(err)) {
		pr_err("send_msg_join_pool error %d\n", err);
		goto free_sess;
	}

	/*
	 * Now that we have the member_id of the new storage node,
	 * check if it is unique.
	 */
	idx = srcu_read_lock(&pool->sess_list_srcu);
	if (__find_sess_by_member_id(pool, pool_sess->member_id)) {
		srcu_read_unlock(&pool->sess_list_srcu, idx);
		pr_err("%s: Session with member_id %u already exists\n",
		       __func__, pool_sess->member_id);
		err = -EEXIST;
		goto err_leave_pool;
	}
	srcu_read_unlock(&pool->sess_list_srcu, idx);

	list_add_tail_rcu(&pool_sess->entry, &pool->sess_list);

	if (!pool->sync) {
		err = rmr_clt_process_non_sync_sess(pool_sess, create, dirty);
		if (err) {
			pr_err("%s: rmr_clt_process_non_sync_sess failed for sess %s with err %d\n",
			       __func__, clt_sess->sessname, err);
			goto rem_from_list;
		}
	} else
		pool_sess_change_state(pool_sess, RMR_CLT_POOL_SESS_NORMAL);

	mutex_unlock(&pool->sess_lock);

	mutex_lock(&clt_sess->lock);
	list_add_tail(&pool_sess->clt_sess_entry, &clt_sess->pool_sess_list);
	mutex_unlock(&clt_sess->lock);

	return pool_sess;

rem_from_list:
	rmr_clt_del_pool_sess(pool_sess);
err_leave_pool:
	send_msg_leave_pool(pool_sess, create, WAIT);
free_sess:
	rmr_clt_free_pool_sess(pool_sess);
err_out:
	mutex_unlock(&pool->sess_lock);
	return ERR_PTR(err);
}

//reauire g_sess_lock acquired
static struct rmr_clt_sess *__find_and_get_clt_sess(const char *sessname)
{
	struct rmr_clt_sess *sess, *sn;

again:
	list_for_each_entry_safe (sess, sn, &g_sess_list, g_list) {
		if (strcmp(sessname, sess->sessname))
			continue;

		if (rmr_clt_sess_get(sess))
			return sess;

		pr_info("failed to get ref for sess %s\n", sessname);
		goto again; //don't like it
	}

	return NULL;
}

struct rmr_clt_sess *find_and_get_or_create_clt_sess(char *sessname,
						     struct rtrs_addr *paths,
						     size_t path_cnt)
{
	struct rmr_clt_sess *sess;

	mutex_lock(&g_sess_lock);
	sess = __find_and_get_clt_sess(sessname);
	mutex_unlock(&g_sess_lock);

	if (!sess) {
		pr_info("%s: Cannot find rmr_clt_sess with name %s\n", __func__, sessname);
		sess = rmr_clt_create_sess(sessname, paths, path_cnt);
		if (IS_ERR(sess)) {
			return sess;
		}
		pr_info("%s: rmr_clt_sess %s created\n", __func__, sessname);
	}

	return sess;
}

/**
 * rmr_clt_del_pool_sess() - Remove a session from the pool session list.
 * @pool_sess:	Pool session to remove.
 *
 * Removes @pool_sess from the pool's session list, waits for any in-progress
 * SRCU readers to finish, and clears any per-CPU cached references to it.
 *
 * Context: Caller must hold pool->sess_lock.
 */
void rmr_clt_del_pool_sess(struct rmr_clt_pool_sess *pool_sess)
{
	int cpu;
	bool dosync = false;
	struct rmr_clt_pool_sess __rcu **ppcpu_sess;
	struct rmr_pool *pool = pool_sess->pool;
	struct rmr_clt_pool *clt_pool = (struct rmr_clt_pool *)pool->priv;

	list_del_rcu(&pool_sess->entry);
	synchronize_srcu(&pool->sess_list_srcu);

	for_each_possible_cpu(cpu) {
		preempt_disable();
		ppcpu_sess = per_cpu_ptr(clt_pool->pcpu_sess, cpu);
		if (pool_sess == rcu_access_pointer(*ppcpu_sess)) {
			rcu_assign_pointer(*ppcpu_sess, NULL);
			dosync = true;
		}
		preempt_enable();
	}

	if (dosync)
		synchronize_srcu(&pool->sess_list_srcu);
}

/**
 * rmr_clt_destroy_pool_sess() - Send leave_pool and free a pool session
 *
 * @pool_sess:	Pool session to destroy.
 * @delete:	True for a permanent pool deletion; false for a temporary
 *		disassembly.  This flag is forwarded in the leave_pool message
 *		so the server can act accordingly.
 */
void rmr_clt_destroy_pool_sess(struct rmr_clt_pool_sess *pool_sess, bool delete)
{
	struct rmr_clt_sess *clt_sess = pool_sess->clt_sess;

	send_msg_leave_pool(pool_sess, delete, WAIT);
	rmr_clt_free_pool_sess(pool_sess);
	rmr_clt_sess_put(clt_sess);
}

static void rmr_clt_destroy_pool(struct rmr_pool *pool)
{
	struct rmr_clt_pool *clt_pool = (struct rmr_clt_pool *)pool->priv;
	struct rmr_clt_pool_sess *pool_sess, *tmp;

	destroy_clt_pool(pool);

	list_for_each_entry_safe (pool_sess, tmp, &pool->sess_list, entry) {
		mutex_lock(&pool->sess_lock);
		list_del_rcu(&pool_sess->entry);
		mutex_unlock(&pool->sess_lock);

		rmr_clt_destroy_pool_sess(pool_sess, false /* never delete */);
	}

	rmr_put_clt_pool(clt_pool);
}

int rmr_clt_remove_pool_from_sysfs(struct rmr_pool *pool,
				   const struct attribute *sysfs_self)
{
	struct rmr_clt_pool *clt_pool = (struct rmr_clt_pool *)pool->priv;

	if (!pool->sync)
		cancel_delayed_work_sync(&clt_pool->recover_dwork);

	rmr_clt_destroy_pool_sysfs_files(pool, sysfs_self);
	rmr_clt_destroy_pool(pool);
	return 0;
}

/*
 * Pre-requisite: rcu read lock should be held by caller
 */
static struct rmr_clt_pool_sess *
rmr_clt_next_sess(struct rmr_pool *pool, struct rmr_clt_pool_sess *prev)
{
	struct rmr_clt_pool_sess *next;

	next = list_next_or_null_rcu(&pool->sess_list,
				     &prev->entry,
				     struct rmr_clt_pool_sess,
				     entry);
	if (next)
		return next;

	return list_first_or_null_rcu(&pool->sess_list,
				      struct rmr_clt_pool_sess,
				      entry);
}

static inline bool rmr_clt_pool_sess_in_iu(struct rmr_iu *iu,
					   struct rmr_clt_pool_sess *pool_sess)
{
	struct rmr_clt_sess_iu *sess_iu, *tmp_sess_iu;

	list_for_each_entry_safe(sess_iu, tmp_sess_iu,
				 &(iu->sess_list), entry) {

		if (sess_iu->pool_sess == pool_sess)
			return true;
	}

	return false;
}

/*
 * Pre-requisite: rcu read lock should be held by caller
 */
static struct rmr_clt_pool_sess *rmr_clt_round_robin_sess(struct rmr_pool *pool,
							  struct rmr_iu *iu)
{
	struct rmr_clt_pool_sess *old, *next, *pool_sess;
	struct rmr_clt_pool *clt_pool;
	struct rmr_clt_pool_sess __rcu **ppcpu_sess;

	clt_pool = (struct rmr_clt_pool *)pool->priv;
	ppcpu_sess = this_cpu_ptr(clt_pool->pcpu_sess);

	if (iu) {
		list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
					 (srcu_read_lock_held(&pool->sess_list_srcu))) {
			if (rmr_clt_pool_sess_in_iu(iu, pool_sess))
				continue;

			rcu_assign_pointer(*ppcpu_sess, pool_sess);
			return pool_sess;
		}

		return NULL;
	}

	old = rcu_dereference(*ppcpu_sess);
	if (!old) {
		next = rmr_clt_get_first_normal_session(pool);
		if (!next)
			return NULL;
		rcu_assign_pointer(*ppcpu_sess, next);
		return next;
	}

	for (next = rmr_clt_next_sess(pool, old);
	     next && next != old;
	     next = rmr_clt_next_sess(pool, next)) {
		/*
		 * It could happen that the state of pool_sess hasn't been able to
		 * represent the recent rtrs-clt sess state.
		 */
		if (next->clt_sess->state == RMR_CLT_SESS_DISCONNECTED)
			continue;

		if (atomic_read(&next->state) == RMR_CLT_POOL_SESS_NORMAL) {
			rcu_assign_pointer(*ppcpu_sess, next);
			return next;
		}
	}

	/*
	 * There may be just one session with normal state i.e. old.
	 * In this case per-cpu sess pointer does not need update.
	 */
	return rmr_clt_get_first_normal_session(pool);
}

int rmr_clt_query(struct rmr_pool *pool, struct rmr_attrs *attr)
{
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_clt_pool *clt_pool = (struct rmr_clt_pool *)pool->priv;
	int idx;

	if (unlikely(!clt_pool))
		return -EINVAL;

	attr->chunk_size = pool->chunk_size;
	attr->sync = pool->sync;

	attr->queue_depth = U32_MAX;
	attr->max_io_size = U32_MAX;
	attr->max_segments = U32_MAX;

	idx = srcu_read_lock(&pool->sess_list_srcu);

	if (list_empty(&pool->sess_list)) {
		srcu_read_unlock(&pool->sess_list_srcu, idx);
		return -ENOENT;
	}

	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		struct rmr_clt_sess *clt_sess = pool_sess->clt_sess;

		attr->queue_depth = min_t(int, clt_sess->queue_depth, attr->queue_depth);
		attr->max_io_size = min_t(u32, clt_sess->max_io_size, attr->max_io_size);
		attr->max_segments = min_t(u32, clt_sess->max_segments, attr->max_segments);
	}
	attr->pool_kobj = &(pool->kobj);

	srcu_read_unlock(&pool->sess_list_srcu, idx);

	return 0;
}
EXPORT_SYMBOL(rmr_clt_query);

struct rmr_iu *rmr_clt_get_iu(struct rmr_pool *pool, enum rmr_io_flags flag,
			      enum rmr_wait_type wait)
{
	int err = 0, idx;
	struct rmr_clt_pool *clt_pool;
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_iu *iu;
	struct rmr_clt_sess_iu *sess_iu, *tmp_sess_iu;
	bool reset = false;

	clt_pool = (struct rmr_clt_pool *)pool->priv;

	if (!test_bit(RMR_CLT_POOL_STATE_IN_USE, &clt_pool->state)) {
		pr_err("%s: Pool %s not in use state\n", __func__, pool->poolname);
		rmr_clt_dump_state(clt_pool);
		return NULL;
	}

	/*
	 * We get the inflight ref first.
	 * If we see that an IO freeze is in progress, we put the ref, and wait for it to unfreeze
	 *
	 * The while loop protects us from parallel freeze, like
	 * A leg deletion, and right after that a call to rmr_clt_close.
	 *
	 * We are guranteed to not go on an infinite loop, since rmr_clt_close can be called only
	 * once, And, there are limited legs to delete
	 */
	percpu_ref_get(&pool->ids_inflight_ref);
	while (atomic_read(&clt_pool->io_freeze) > 0) {
		percpu_ref_put(&pool->ids_inflight_ref);
		/*
		 * Coincidentally, the rcu lock might be held when the wait event occurs,
		 * violating the constraint that no sleeping during general rcu critical section.
		 * Temporarily release the rcu lock, and re-acquire it after waking up.
		 *
		 * TODO: This approach is simple but may need to be revisited.
		 */
		if (rcu_read_lock_held()) {
			rcu_read_unlock();
			reset = true;
		}

		wait_event(clt_pool->map_update_wq, !atomic_read(&clt_pool->io_freeze));

		if (reset)
			rcu_read_lock();

		/*
		 * Once IO is unfrozen, we check if the state of the pool has changed.
		 * It could be that rmr_clt_close was called, and hence state is not IN_USE.
		 * Or, it could be that the last leg was deleted, and we are not in JOINED state
		 *
		 * In both the case, we cannot service IOs, hence fail.
		 */
		if (!test_bit(RMR_CLT_POOL_STATE_IN_USE, &clt_pool->state) ||
		    !test_bit(RMR_CLT_POOL_STATE_JOINED, &clt_pool->state)) {
			pr_err("%s: Failed to get inflight IO ref.\n", __func__);
			pr_err("%s: Pool %s is not joined or used\n",
				__func__, pool->poolname);
			rmr_clt_dump_state(clt_pool);
			return NULL;
		}

		percpu_ref_get(&pool->ids_inflight_ref);
	}

	iu = rmr_alloc_iu();
	if (unlikely(!iu)) {
		percpu_ref_put(&pool->ids_inflight_ref);
		return NULL;
	}

	idx = srcu_read_lock(&pool->sess_list_srcu);
	if (rmr_op(flag) == RMR_OP_READ) {
		/*
		 * Round robin use of one of the sessions in normal state for READ.
		 *
		 * This call is always from rmr_clt_request, so for READ,
		 * this is the first pool_sess we are trying
		 */
		pool_sess = rmr_clt_round_robin_sess(pool, NULL);
		if (unlikely(!pool_sess)) {
			err = -ENODEV;
			goto put_iu;
		}

		sess_iu = rmr_get_sess_iu(pool_sess, RTRS_IO_CON, (enum wait_type) wait);
		if (unlikely(!sess_iu))
			goto put_iu;

		sess_iu->rmr_iu = iu;
		iu->num_sessions = 1;
		list_add_tail(&(sess_iu->entry), (&iu->sess_list));
	} else {
		/*
		 * For WRITE operations we need to submit to all sessions.
		 */
		list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
					 (srcu_read_lock_held(&pool->sess_list_srcu))) {
			/* Sessions must be in normal state for I/O */
			if (atomic_read(&pool_sess->state) != RMR_CLT_POOL_SESS_NORMAL)
				continue;

			sess_iu = rmr_get_sess_iu(pool_sess,
						  RTRS_IO_CON, (enum wait_type) wait);
			if (unlikely(!sess_iu))
				goto put_sessions;

			sess_iu->rmr_iu = iu;
			/*
			 * The mem_id of sess_iu tracks the next free slot in the permit bitmap
			 * of an RTRS-clt session, which is used to store write IO chunk info by
			 * RMR-server.
			 */
			sess_iu->mem_id = sess_iu->permit->mem_id;
			iu->num_sessions++;
			list_add_tail(&(sess_iu->entry), (&iu->sess_list));
		}
	}

	refcount_set(&iu->refcount, iu->num_sessions);
	iu->errno = 0;

	srcu_read_unlock(&pool->sess_list_srcu, idx);

	return iu;

put_sessions:
	list_for_each_entry_safe(sess_iu, tmp_sess_iu,
				 &(iu->sess_list), entry) {
		if (!list_empty(&sess_iu->entry))
			list_del_init(&sess_iu->entry);
		rmr_put_sess_iu(sess_iu->pool_sess, sess_iu);
	}
put_iu:
	srcu_read_unlock(&pool->sess_list_srcu, idx);
	rmr_put_iu(iu);
	percpu_ref_put(&pool->ids_inflight_ref);

	if (err)
		return ERR_PTR(err);

	return NULL;
}
EXPORT_SYMBOL(rmr_clt_get_iu);

void rmr_clt_put_iu(struct rmr_pool *pool, struct rmr_iu *iu)
{
	rmr_put_iu(iu);
	percpu_ref_put(&pool->ids_inflight_ref);
}
EXPORT_SYMBOL(rmr_clt_put_iu);

/**
 * Returns 1 if the errno represents a condition in the
 * storage server that prevents the operation to be executed.
 * The oposite is an error with respect to the storage server
 * where the operation can be re-tried on a different one.
 *
 * Example is attemp to read a block that does not exists
 * versus server has been crashed.
 *
 * Note that in doubt we have to trigger the re-try.
 */
/*
static inline int rmr_is_op_error(int errno)
{
	switch (-errno) {
	case ENOENT:
	case EINVAL:
	case EEXIST:
	case ENODEV:
		return 1;
	default:
		return 0;
	}
}
*/

static void msg_read_conf(void *priv, int errno)
{
	struct rmr_clt_sess_iu *sess_iu	= (struct rmr_clt_sess_iu *)priv;
	struct rmr_clt_pool_sess *pool_sess = sess_iu->pool_sess;
	struct rmr_iu *iu		= sess_iu->rmr_iu;
	rmr_conf_fn *clt_conf		= iu->conf;

	WARN_ON(atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_CREATED);

	if (errno) {
		if (!iu->errno)
			/* only first error is reported */
			iu->errno = errno;

		pr_err_ratelimited("%s got errno: %d for session %d. Schedule retry.\n",
				   __func__, errno, pool_sess->member_id);
		if (!pool_sess->pool->sync)
			pool_sess_change_state(pool_sess, RMR_CLT_POOL_SESS_FAILED);

		INIT_WORK(&iu->work, retry_failed_read);
		schedule_work(&iu->work);
	} else {
		(*clt_conf)(iu->priv, errno);
	}
}

static void retry_failed_read(struct work_struct *work)
{
	struct rmr_iu *iu = container_of(work, struct rmr_iu, work);
	struct rmr_pool *pool = iu->pool;
	struct rmr_clt_pool *clt_pool = (struct rmr_clt_pool *)pool->priv;
	rmr_conf_fn *clt_conf	= iu->conf;
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_clt_sess_iu *sess_iu;
	struct rtrs_clt_req_ops req_ops;
	struct kvec vec;
	int err, idx;

	idx = srcu_read_lock(&pool->sess_list_srcu);

	pool_sess = rmr_clt_round_robin_sess(pool, iu);
	if (!pool_sess)
		goto give_up;

	sess_iu = rmr_get_sess_iu(pool_sess, RTRS_IO_CON, RTRS_PERMIT_WAIT);
	if (unlikely(!sess_iu))
		goto give_up;

	pr_debug("%s: Pool %s to session %d, chunk [%llu, %llu]\n",
		 __func__, pool->poolname, pool_sess->member_id,
		 le64_to_cpu(iu->msg.id_a), le64_to_cpu(iu->msg.id_b));

	sess_iu->rmr_iu = iu;
	iu->msg.member_id = pool_sess->member_id;
	atomic_inc(&clt_pool->stats.read_retries);

	list_add_tail(&(sess_iu->entry), (&iu->sess_list));

	vec = (struct kvec) {
		.iov_base = &iu->msg,
		.iov_len  = sizeof(iu->msg)
	};

	req_ops = (struct rtrs_clt_req_ops) {
                .priv = sess_iu,
                .conf_fn = msg_read_conf,
        };

	trace_retry_failed_read(READ, sess_iu);

	err = rtrs_clt_request(RMR_OP_READ, &req_ops, pool_sess->clt_sess->rtrs, sess_iu->permit,
			       &vec, 1, le32_to_cpu(iu->msg.length), iu->sg, iu->sg_cnt);

	srcu_read_unlock(&pool->sess_list_srcu, idx);

	if (err)
		/* beware! recursion!! */
		msg_read_conf(sess_iu, err);

	return;
give_up:
	srcu_read_unlock(&pool->sess_list_srcu, idx);
	/* recursion termination! */
	(*clt_conf)(iu->priv, iu->errno);
}

/*
static int rmr_clt_map_remove_id(struct rmr_pool *pool, int srv_id, rmr_id_t id)
{
	struct rmr_dirty_id_map *map;

	pr_debug("pool %s, remove id (%llu, %llu) for stg_id %d\n",
		 pool->poolname, id.a, id.b, srv_id);

	map = rmr_pool_find_map(pool, srv_id);
	if (!map) {
		pr_err("pool %s no map found for pool_id %u\n",
		       pool->poolname, srv_id);
		return -EINVAL;
		//TODO: handle this , probably initialize map, or just throw err?
	}

	if (!rmr_map_empty(map)) {
		void *val;

		val = rmr_map_find(map, id);
		if (!val) {
			pr_debug("pool %s value for id (%llu, %llu) is not in the dirty map\n",
				 pool->poolname, id.a, id.b);
			return 0;
		}
		rmr_map_erase(map, id);
		pr_debug("pool %s, id (%llu, %llu) is removed from map for stg_id %d\n",
			 pool->poolname, id.a, id.b, srv_id);
	}

	return 0;
}
*/

static void msg_io_conf(void *priv, int errno)
{
	struct rmr_clt_sess_iu *sess_iu	= (struct rmr_clt_sess_iu *)priv;
	struct rmr_clt_pool_sess *pool_sess = sess_iu->pool_sess;
	struct rmr_iu *iu		= sess_iu->rmr_iu;
	rmr_conf_fn *clt_conf		= iu->conf;
	void *clt_priv = iu->priv;

	WARN_ON(atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_CREATED);
	WARN_ON(pool_sess->pool->sync);

	if (errno) {
		pr_err("%s: For sess %s, id (%llu, %llu), got errno: %d\n",
		       __func__, pool_sess->sessname, iu->msg.id_a, iu->msg.id_b, errno);
		sess_iu->errno = errno;
		if (!iu->errno)
			/* only first error is reported */
			iu->errno = errno;
		pool_sess_change_state(pool_sess, RMR_CLT_POOL_SESS_FAILED);
		pr_debug("iu->errno %d, errno %d, before dec refcnt %d\n",
			 iu->errno, errno, refcount_read(&iu->refcount));
	} else {
		atomic_inc(&iu->succeeded);
		// TODO: is it ok to clear it here?
		// rmr_clt_map_remove_id(session->pool, session->pool_id, iu->id);
	}

	pr_debug("called for id (%llu, %llu), errno %d, sessname %s\n",
		 iu->msg.id_a, iu->msg.id_b, errno, pool_sess->sessname);

	if (refcount_dec_and_test(&iu->refcount)) {
		if (atomic_read(&iu->succeeded) == 0) {
			/*
			 * None of the IOs succeeded.
			 * Map add is not needed; Just fail the IO.
			 */
			pr_err("Write IO failed. Passing it up. errno %d\n", iu->errno);
			(*clt_conf)(clt_priv, iu->errno);
		} else if (iu->errno) {
			/*
			 * Some IOs failed. Send map update (add).
			 * The clt conf will be called when map update is done.
			 *
			 * We are using the same iu to send map update
			 * So reset the refcount.
			 */
			refcount_set(&iu->refcount, iu->num_sessions);

			/*
			 * we are in interrupt here, so sched map update
			 */
			pr_debug("%s: some IOs failed for %s. Starts map_add\n", __func__,
				 pool_sess->sessname);
			INIT_WORK(&iu->work, sched_map_add);
			schedule_work(&iu->work);
		} else {
			/*
			 * All good.
			 */
			errno = 0;
			(*clt_conf)(clt_priv, errno);
		}
	}
}

static inline void rmr_clt_put_cu(struct rmr_clt_cmd_unit *cmd_unit)
{
	percpu_ref_put(&cmd_unit->clt_pool->pool->ids_inflight_ref);
	kfree(cmd_unit);
}

/**
 * msg_cmd_conf() - Confirmation function called for command user commands sent
 *
 * priv:	Pointer to private data passed to rtrs. sess_iu in this case.
 * errno:	error status passed by rtrs
 */
static void msg_cmd_conf(void *priv, int errno)
{
	struct rmr_clt_sess_iu *sess_iu	= (struct rmr_clt_sess_iu *)priv;
	struct rmr_clt_cmd_unit *cmd_unit = sess_iu->rmr_cmd_unit;
	rmr_conf_fn *clt_conf = cmd_unit->conf;
	void *clt_priv = cmd_unit->priv;
	int total_failed;

	pr_debug("%s: sessname:%s, errno=%d\n", __func__, sess_iu->pool_sess->sessname, errno);
	if (!errno)
		atomic_inc(&cmd_unit->succeeded);

	if (refcount_dec_and_test(&cmd_unit->refcount)) {
		if (atomic_read(&cmd_unit->succeeded) == 0) {
			/*
			 * None of the IOs succeeded.
			 */
			pr_err("CMD failed with err %pe. Passing it up.\n", ERR_PTR(errno));
			(*clt_conf)(clt_priv, errno);
		} else {
			total_failed = cmd_unit->failed_state +
				       (cmd_unit->num_sessions - atomic_read(&cmd_unit->succeeded));
			/*
			 * Pass the number of failures up to the user.
			 */
			(*clt_conf)(clt_priv, total_failed);
		}

		rmr_clt_put_cu(cmd_unit);
	}

	rmr_put_sess_iu(sess_iu->pool_sess, sess_iu);
}

/* The amount of data that belongs to an I/O and the amount of data that
 * should be read or written to the disk (bi_size) can differ.
 *
 * E.g. When WRITE_SAME is used, only a small amount of data is
 * transferred that is then written repeatedly over a lot of sectors.
 *
 * Get the size of data to be transferred via RTRS by summing up the size
 * of the scather-gather list entries.
 */
static size_t rmr_clt_get_sg_size(struct scatterlist *sglist, u32 len)
{
	struct scatterlist *sg;
	size_t tsize = 0;
	int i;

	for_each_sg(sglist, sg, len, i)
		tsize += sg->length;
	return tsize;
}

/**
 * rmr_clt_request() - Request data transfer to/from storage node via given pool
 *
 * @pool:	The Pool
 * @iu:		Iu allocated by pevious rmr_clt_get_iu call.
 * @offset:	offset inside the object to read/write:
 * @length:	length of data starting from offset
 * @flag:	READ/WRITE/REMOVE
 * @prio:	priority of IO
 * @priv:	User provided data, passed back with corresponding
 *		@(conf) confirmation.
 * @conf:	callback function to be called as confirmation
 * @sg:		Pages to be sent/received to/from server.
 * @sg_cnt:	Number of elements in the @sg
 *
 * Description:
 *	Data transfer through the given pool, using the underlying RTRS <-> RDMA
 *	While sending write IOs, if there are FAILED or RECONNECTING pool sessions, that IO
 *	would be added as dirty for such sessions.
 *	This is used by both pserver client, and the rmr server on the storage node to perform
 *	sync reads.
 *
 * Return:
 *	0 on success. This means IO was sent. Final confirmation would be sent via conf function
 *	Error value on failure
 */
int rmr_clt_request(struct rmr_pool *pool, struct rmr_iu *iu,
		    size_t offset, size_t length, enum rmr_io_flags flag, unsigned short prio,
		    void *priv, rmr_conf_fn *conf, struct scatterlist *sg, unsigned int sg_cnt)
{
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_clt_sess_iu *sess_iu, *tmp_sess_iu;
	struct rtrs_clt_req_ops req_ops;
	rmr_id_t id;
	struct kvec vec;
	size_t sg_len;
	int dir, err, idx;
	u32 rmr_flag;

	rmr_get_iu(iu);
	rmr_flag = rmr_op(flag);
	dir = (rmr_flag == RMR_OP_READ) ? READ : WRITE;

	sg_len = rmr_clt_get_sg_size(sg, sg_cnt);
	if (!(flag & RMR_OP_DISCARD || flag & RMR_OP_WRITE_ZEROES))
		WARN_ON(length != sg_len);

	iu->msg.hdr.group_id = cpu_to_le32(pool->group_id);
	iu->msg.hdr.type = cpu_to_le16(RMR_MSG_IO);
	iu->msg.hdr.__padding = 0;

	iu->msg.offset = cpu_to_le32(offset);
	iu->msg.length = cpu_to_le32(length);
	iu->msg.flags = cpu_to_le32(flag);
	iu->msg.prio = cpu_to_le16(prio);

	iu->msg.sync = pool->sync;

	iu->priv = priv;
	iu->conf = conf;
	iu->pool = pool;

	if (rmr_flag != RMR_OP_FLUSH && sg_len) {
		rmr_map_calc_chunk(pool, offset, length, &id);
		/*
		 * We are not ready to process IO requests which are across chunk boundary.
		 * The main area which needs work is triggering sync IO (see rmr-req.c) which
		 * holding the IO which touches multiple chunks. And then making sure other IOs
		 * which overlap these chunks are held properly, and restarted once the corresponding
		 * chunk is synced.
		 */
		BUG_ON(id.a > 1);
		iu->msg.id_a = cpu_to_le64(id.a);
		iu->msg.id_b = cpu_to_le64(id.b);
	}

	if (rmr_flag == RMR_OP_READ) {
		iu->sg = sg;
		iu->sg_cnt = sg_cnt;
	} else if (!pool->sync && rmr_flag == RMR_OP_WRITE) {
		/*
		 * We take this path only for request from client side
		 * Never from rmr_req_remote_read.
		 */
		int failed_cnt = 0;
		int i;

		atomic_set(&iu->succeeded, 0);
		idx = srcu_read_lock(&pool->sess_list_srcu);
		for (i = 0; i < RMR_POOL_MAX_SESS; i++) {
			struct rmr_clt_pool_sess *ps;
			enum rmr_clt_pool_sess_state state;
			u8 mid = pool->pool_md.srv_md[i].member_id;

			if (!mid)
				continue;

			ps = xa_load(&pool->stg_members, mid);
			if (ps) {
				state = atomic_read(&ps->state);
				if (state != RMR_CLT_POOL_SESS_FAILED &&
				    state != RMR_CLT_POOL_SESS_RECONNECTING)
					continue;
			}
			/* ps == NULL (disassembled) or FAILED/RECONNECTING */
			if (WARN_ON(failed_cnt >= RMR_POOL_MAX_SESS))
				break;
			iu->msg.map_ver = cpu_to_le64(pool->map_ver);
			iu->msg.failed_id[failed_cnt] = mid;
			failed_cnt++;
			rmr_clt_map_add_id(pool, mid, id);
		}
		srcu_read_unlock(&pool->sess_list_srcu, idx);
		iu->msg.failed_cnt = failed_cnt;
	} else if (pool->sync) {
		pr_err("rmr_clt_request: Sync sessions do not process writes\n");
		return -EPERM;
	}

	vec = (struct kvec) {
		.iov_base = &iu->msg,
		.iov_len  = sizeof(iu->msg)
	};

	list_for_each_entry_safe(sess_iu, tmp_sess_iu,
				 &(iu->sess_list), entry) {
		struct rmr_clt_sess *clt_sess;

		pool_sess = sess_iu->pool_sess;
		clt_sess = pool_sess->clt_sess;
		iu->msg.member_id = pool_sess->member_id;

		if (atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_REMOVING ||
		    pool_sess->maintenance_mode) {
			/*
			 * The storage for this session is getting removed from
			 * the pool, or is in maintenance mode.
			 * Simply complete this IO with error
			 */
			err = -EAGAIN;
			goto complete_io;
		}

		pr_debug("Sending %x request to pool %s session %s "
			 "chunk (%llu, %llu) offset %lu length %lu)\n",
			 rmr_flag,
			 pool->poolname, pool_sess->sessname,
			 id.a, id.b, offset, length);

		if (rmr_flag == RMR_OP_READ) {
			req_ops = (struct rtrs_clt_req_ops) {
				.priv = sess_iu,
				.conf_fn = msg_read_conf,
			};
		} else {
			req_ops = (struct rtrs_clt_req_ops) {
				.priv = sess_iu,
				.conf_fn = msg_io_conf,
			};

			/*
			 * Update mem_id before transmitting each write IO to the corresponding
			 * server.
			 */
			iu->msg.mem_id = cpu_to_le32(sess_iu->mem_id);
		}

		trace_rmr_clt_request(dir, sess_iu);

		err = rtrs_clt_request(dir, &req_ops, clt_sess->rtrs,
				       sess_iu->permit, &vec, 1, sg_len,
				       sg, sg_cnt);

complete_io:
		if (err) {
			if (rmr_flag == RMR_OP_READ)
				msg_read_conf(sess_iu, err);
			else
				msg_io_conf(sess_iu, err);
		}
	}
	rmr_put_iu(iu);

	return 0;
}
EXPORT_SYMBOL(rmr_clt_request);

/**
 * rmr_clt_get_cu() - Allocate and return a command unit.
 *
 * @pool:	rmr pool for which the command unit is to be allocated
 *
 * Description:
 *	Allocates and returns a command unit for the rmr pool. The command unit contains a list of
 *	session units, for each session which is not in the "REMOVING" state.
 *
 * Return:
 *	Pointer to the command unit
 */
static struct rmr_clt_cmd_unit *rmr_clt_get_cu(struct rmr_pool *pool)
{
	struct rmr_clt_pool *clt_pool = (struct rmr_clt_pool *)pool->priv;
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_clt_cmd_unit *cmd_unit;
	struct rmr_clt_sess_iu *sess_iu, *tmp_sess_iu;
	int idx;

	if (!test_bit(RMR_CLT_POOL_STATE_IN_USE, &clt_pool->state)) {
		pr_err("%s: Pool %s not in use\n", __func__, pool->poolname);
		rmr_clt_dump_state(clt_pool);
		return NULL;
	}

	/*
	 * We get the inflight ref first.
	 * If we see that an IO freeze is in progress, we put the ref, and wait for it to unfreeze
	 *
	 * The while loop protects us from parallel freeze, like
	 * A leg deletion, and right after that a call to rmr_clt_close.
	 *
	 * We are guranteed to not go on an infinite loop, since rmr_clt_close can be called only
	 * once, And, there are limited legs to delete
	 */
	percpu_ref_get(&pool->ids_inflight_ref);
	while (atomic_read(&clt_pool->io_freeze) > 0) {
		percpu_ref_put(&pool->ids_inflight_ref);
		wait_event(clt_pool->map_update_wq, !atomic_read(&clt_pool->io_freeze));

		/*
		 * Once IO is unfrozen, we check if the state of the pool has changed.
		 * It could be that rmr_clt_close was called, and hence state is not IN_USE.
		 * Or, it could be that the last leg was deleted, and we are not in JOINED state
		 *
		 * In both the case, we cannot service IOs, hence fail.
		 */
		if (!test_bit(RMR_CLT_POOL_STATE_IN_USE, &clt_pool->state) ||
		    !test_bit(RMR_CLT_POOL_STATE_JOINED, &clt_pool->state)) {
			pr_err("%s: Failed to get inflight IO ref.\n", __func__);
			pr_err("%s: Pool %s is not joined or used\n", __func__, pool->poolname);
			rmr_clt_dump_state(clt_pool);
			return NULL;
		}

		percpu_ref_get(&pool->ids_inflight_ref);
	}

	cmd_unit = kzalloc(sizeof(*cmd_unit), GFP_KERNEL);
	if (!cmd_unit) {
		percpu_ref_put(&pool->ids_inflight_ref);
		return NULL;
	}

	INIT_LIST_HEAD(&cmd_unit->sess_list);
	cmd_unit->pool = pool;
	cmd_unit->clt_pool = clt_pool;
	atomic_set(&cmd_unit->succeeded, 0);

	idx = srcu_read_lock(&pool->sess_list_srcu);
	/*
	 * Acquire the permits for all sessions.
	 * Continue only if we manage to get permits for all "normal" sessions??
	 */
	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		if (atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_REMOVING)
			continue;

		if (atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_FAILED) {
			cmd_unit->failed_state++;
			continue;
		}

		sess_iu = rmr_get_sess_iu(pool_sess, RTRS_ADMIN_CON, RTRS_PERMIT_NOWAIT);
		if (unlikely(!sess_iu))
			goto put_sessions;

		sess_iu->rmr_cmd_unit = cmd_unit;

		cmd_unit->num_sessions++;
		list_add_tail(&(sess_iu->entry), (&cmd_unit->sess_list));
	}
	srcu_read_unlock(&pool->sess_list_srcu, idx);
	refcount_set(&cmd_unit->refcount, cmd_unit->num_sessions);

	return cmd_unit;

put_sessions:
	srcu_read_unlock(&pool->sess_list_srcu, idx);

	/* Free sess_ius */
	list_for_each_entry_safe(sess_iu, tmp_sess_iu,
				 &(cmd_unit->sess_list), entry) {
		if (!list_empty(&sess_iu->entry))
			list_del_init(&sess_iu->entry);
		rmr_put_sess_iu(sess_iu->pool_sess, sess_iu);
	}

	rmr_clt_put_cu(cmd_unit);

	return NULL;
}

/**
 * rmr_clt_cmd_err_conf() - Calls confirmation function for commands
 *
 * @work:	schedules work
 *
 * Description:
 *	In case of error in the user command path, we cannot call the confirmation function
 *	directly, since it might end up calling confirmation function of the user itself.
 *	Hence a work is scheduled to call the confirmation function in case the code for sending
 *	user commands itself fails.
 */
static void rmr_clt_cmd_err_conf(struct work_struct *work)
{
	struct rmr_clt_sess_iu *sess_iu = container_of(work, struct rmr_clt_sess_iu, work);

	msg_cmd_conf(sess_iu, sess_iu->errno);
}

/**
 * rmr_clt_cmd_with_rsp() - Sends a user command to all sessions of an rmr pool
 *
 * @pool:	rmr pool to which the command is for
 * @conf:	confirmation function to be called after completion
 * @priv:	pointer to priv data, to be returned to user while calling conf function
 * @usr_vec:	kvec containing user data (mostly command messages?)
 * @nr:		number of kvecs
 * @buf:	buf where the response from the user server is to be directed
 *		The buf must be physically contiguous in memory (kmalloc()'d).
 * @buf_len:	length of the buffer
 * @size:	size of the buf to be sent to a single session
 *
 * Description:
 *	This function provides an interface for the user to send commands to the server side.
 *	The command is sent as a read, so that the response from the user srv side can be received
 *	The buffer sent by the user is meant to receive the response from the user server side.
 *	The size of the buffer is set during rmr_clt_open.
 *
 * Return:
 *	0 on success
 *	negative errno in case of error
 *
 * Context:
 *	Inflight commands will block map update, until the inflights are completed.
 */
int rmr_clt_cmd_with_rsp(struct rmr_pool *pool, rmr_conf_fn *conf, void *priv,
			 const struct kvec *usr_vec, size_t nr, void *buf, int buf_len, size_t size)
{
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_clt_sess_iu *sess_iu, *tmp_sess_iu;
	struct rmr_clt_cmd_unit *cmd_unit;
	struct rmr_msg_pool_cmd msg = {};
	struct rtrs_clt_req_ops req_ops;
	struct kvec *vec;
	int i, j, err = 0;

	/*
	 * TODO: kvmalloc() memory is yet to be supported for SG I/O.
	 */
	if (is_vmalloc_addr(buf))
		return -EINVAL;

	if (buf_len != (RMR_POOL_MAX_SESS * size))
		return -EINVAL;

	rmr_clt_init_cmd(pool, &msg);
	msg.cmd_type = RMR_CMD_USER;

	/*
	 * RMR msg struct + user vecs
	 */
	vec = kzalloc((1 + nr) * sizeof(*vec), GFP_KERNEL);
	if (!vec)
		return -ENOMEM;

	/*
	 * RMR msg struct first,
	 * followed by the user kvecs
	 */
	vec[0].iov_base = &msg;
	vec[0].iov_len = sizeof(msg);
	for (i = 1, j = 0; j < nr; i++, j++) {
		vec[i].iov_base = usr_vec[j].iov_base;
		vec[i].iov_len = usr_vec[j].iov_len;

		msg.user_cmd.usr_len += usr_vec[j].iov_len;
	}

	cmd_unit = rmr_clt_get_cu(pool);
	if (!cmd_unit) {
		err = -ENOMEM;
		goto out;
	}

	cmd_unit->conf = conf;
	cmd_unit->priv = priv;

	i = 0;
	list_for_each_entry_safe(sess_iu, tmp_sess_iu,
				 &(cmd_unit->sess_list), entry) {
		pool_sess = sess_iu->pool_sess;

		req_ops = (struct rtrs_clt_req_ops){
			.priv = sess_iu,
			.conf_fn = msg_cmd_conf,
		};

		/*
		 * The user expects each node to be able to send back data of this "size" as
		 * response.
		 * So divide the user buffer into chunks of "size", and send them to each leg.
		 */
		sg_init_one(&sess_iu->sg, buf + (i * size), size);

		trace_rmr_clt_cmd_with_rsp(READ, sess_iu);

		err = rtrs_clt_request(READ, &req_ops, pool_sess->clt_sess->rtrs, sess_iu->permit,
				       vec, (1 + nr), size, &sess_iu->sg, 1);
		if (err) {
			/*
			 * We want to deal with this error just like we deal with the error
			 * received from the conf function returned from rtrs.
			 * This would help us to inform the user the correct number of commands
			 * which failed on the rmr level (rtrs is also rmr level for user).
			 */
			pr_warn("rtrs_clt_request Failed with err %d\n", err);
			sess_iu->errno = err;
			INIT_WORK(&sess_iu->work, rmr_clt_cmd_err_conf);
			schedule_work(&sess_iu->work);
			err = 0;
		}

		i++;
	}

	/*
	 * No session to send command
	 */
	if (i == 0) {
		rmr_clt_put_cu(cmd_unit);
		err = -EINVAL;
	}

out:
	kfree(vec);

	return err;
}
EXPORT_SYMBOL(rmr_clt_cmd_with_rsp);

/**
 * rmr_clt_send_cmd_with_data() - send command containing data buffer as a payload or response
 *
 * @pool:	rmr pool to send command
 * @pool_sess:	client pool session used to send
 * @msg:	initialized command message describing the command
 * @buf:	pointer to the data buffer for data transfers
 * @buflen:	size of the buffer in bytes
 *
 * Description:
 *	Performs sending the command described by msg with a payload or response
 *	in the buf.
 *
 * Return:
 *	0 on success, error code otherwise.
 *
 * Context:
 *	This function blocks while sending the buffer.
 *
 * Locks:
 *	should be called under srcu_read_lock since it uses pool_sess
 */
int rmr_clt_send_cmd_with_data(struct rmr_pool *pool, struct rmr_clt_pool_sess *pool_sess,
			       struct rmr_msg_pool_cmd *msg,
			       void *buf, unsigned int buflen)
{
	struct rmr_clt_sess_iu *sess_iu;
	struct rmr_clt_sess *clt_sess = pool_sess->clt_sess;
	struct kvec vec = {
		.iov_base = msg,
		.iov_len = sizeof(*msg)
	};
	int errno = 0, err = 0;
	int dir;

	switch (msg->cmd_type) {
	case RMR_CMD_MAP_CHECK:
	case RMR_CMD_READ_MAP_BUF:
	case RMR_CMD_MAP_GET_VER:
	case RMR_CMD_MD_SEND:
	case RMR_CMD_MAP_SET_VER:
		dir = READ;
		break;
	case RMR_CMD_MAP_TEST:
	case RMR_CMD_SEND_MAP_BUF:
	case RMR_CMD_SEND_MD_BUF:
		dir = WRITE;
		break;
	default:
		pr_err("%s: pool %s cmd type %u is not supported\n",
		       __func__, pool->poolname, msg->cmd_type);
		return -EINVAL;
	}

	// TODO: why io_con not admin?
	if (clt_sess->state == RMR_CLT_SESS_DISCONNECTED) {
		pr_debug("The rmr client session %s state is disconnected\n", clt_sess->sessname);
		err = -EINVAL;
		goto err;
	}

	sess_iu = rmr_msg_get_iu(pool_sess, RTRS_IO_CON, RTRS_PERMIT_WAIT, 2);
	if (unlikely(!sess_iu)) {
		err = -ENOMEM;
		goto err;
	}

	sess_iu->buf = buf;
	sg_init_one(&sess_iu->sg, buf, buflen);

	err = send_usr_msg(clt_sess->rtrs, dir, sess_iu,
			   &vec, 1, buflen, &sess_iu->sg, 1,
			   msg_pool_cmd_map_content_conf, &errno, WAIT);
	if (unlikely(err)) {
		rmr_msg_put_iu(pool_sess, sess_iu);
	} else {
		err = errno;
	}

	rmr_msg_put_iu(pool_sess, sess_iu);

err:
	return err;
}

/**
 * rmr_clt_pool_member_synced() - check if the pool member has no data to sync
 *
 * @pool:	rmr pool in which we perform the check
 * @member_id:	id of the pool member tto check
 *
 * Description:
 *	Send the check map command to the pool member with  the specified id.
 *	Pool member returns whether he has unsynced chunks or not.
 *
 * Return:
 *	error code if failed to send, 0 if pool member is not synced completely,
 *	1 if pool member is synced (has no dirty chunks in his map).
 *
 * Context:
 *	This function blocks while sending the command.
 *
 * Locks:
 *	no
 */
int rmr_clt_pool_member_synced(struct rmr_pool *pool, u8 member_id)
{
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_msg_pool_cmd_rsp rsp = {};
	struct rmr_msg_pool_cmd msg = {};
	int ret = 0, idx;
	enum rmr_clt_pool_sess_state state;

	pr_debug("start looking for session with member_id=%u\n", member_id);
	idx = srcu_read_lock(&pool->sess_list_srcu);

	pool_sess = __find_sess_by_member_id(pool, member_id);
	if (!pool_sess) {
		pr_err("in pool %s failed to find sess with a member_id=%u\n",
		       pool->poolname, member_id);
		ret = -ENOENT;
		goto out;
	}

	pr_debug("found session %s with member_id=%u\n",
		 pool_sess->sessname, member_id);

	state = atomic_read(&pool_sess->state);
	if (state == RMR_CLT_POOL_SESS_FAILED ||
	    state == RMR_CLT_POOL_SESS_REMOVING) {
		pr_debug("pool %s session %s is in %s state, cannot send cmd %s\n",
			 pool->poolname, pool_sess->sessname,
			 rmr_clt_sess_state_str(state), rmr_get_cmd_name(msg.cmd_type));
		ret = -EINVAL;
		goto out;
	}

	rmr_clt_init_cmd(pool, &msg);
	msg.cmd_type = RMR_CMD_MAP_CHECK;

	pr_debug("send cmd %u to %s\n", msg.cmd_type, pool_sess->sessname);
	ret = rmr_clt_send_cmd_with_data(pool, pool_sess, &msg, &rsp, sizeof(rsp));
	if (ret) {
		pr_err("%s: For pool %s failed to %s, err %d\n",
		       __func__, pool->poolname, rmr_get_cmd_name(msg.cmd_type), ret);
		goto out;
	}

	if (rsp.value)
		ret = 1; // other side reported map is clear

	pr_debug("send cmd %u to %s is done\n", msg.cmd_type, pool_sess->sessname);
out:
	srcu_read_unlock(&pool->sess_list_srcu, idx);

	return ret;
}
EXPORT_SYMBOL(rmr_clt_pool_member_synced);

/**
 * rmr_pool_md_to_buf - Fill the buffer with the metadata
 *
 * @pool:	rmr pool contains the metadata. It must be a non-sync pool,
 *		either client or server pool.
 * @buf:	buffer to fill with the metadata.
 *
 */
static void rmr_clt_md_to_buf(struct rmr_pool *pool, u8 *buf)
{
	struct rmr_pool_md *pool_md;
	struct rmr_srv_md *srv_md;

	if (pool->is_clt) {
		pool_md = (struct rmr_pool_md *)buf;
		/* copy the entire client pool md */
		memcpy(pool_md, &pool->pool_md, sizeof(struct rmr_pool_md));
		return;
	}

	srv_md = (struct rmr_srv_md *)(&buf[RMR_CLT_MD_SIZE]);
	memcpy(srv_md, &pool->pool_md.srv_md[0], RMR_SRV_MD_SIZE);
}

/**
 * rmr_clt_pool_send_md_all() - Send metadata of rmr pool
 *
 * Description:
 *	Send metadata of the src pool to all sessions of the client pool.
 *	1) If the client pool is sync pool, it sends the entire server pool
 *	metadata back after the leader reads the metadata of its connected
 *	nodes.
 *	2) If it is non-sync, send the client pool metadata to storage node
 *	backups.
 */
int rmr_clt_pool_send_md_all(struct rmr_pool *src_pool, struct rmr_pool *clt_pool)
{
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_msg_pool_cmd msg = {};
	void *buf;
	u32 buflen;
	int err = 0, idx;

	if (!clt_pool) {
		pr_err("Cannot send metadata when clt_pool is NULL\n");
		return -EINVAL;
	}

	if (src_pool->sync) {
		pr_err("Cannot send metadata when src_pool is sync\n");
		return -EINVAL;
	}

	buf = kzalloc(RMR_MD_SIZE, GFP_KERNEL);
	buflen = RMR_MD_SIZE;
	if (!buf)
		return -ENOMEM;

	rmr_clt_md_to_buf(src_pool, buf);

	/*
	 * It will continue to send the md to the next session even if the previous send failed.
	 */
	idx = srcu_read_lock(&clt_pool->sess_list_srcu);
	list_for_each_entry_srcu(pool_sess, &clt_pool->sess_list, entry,
				 (srcu_read_lock_held(&clt_pool->sess_list_srcu))) {
		pr_debug("Start sending md for pool %s; to session %s with member_id %d\n",
			 src_pool->poolname, pool_sess->sessname, pool_sess->member_id);

		rmr_clt_init_cmd(clt_pool, &msg);
		msg.cmd_type = RMR_CMD_SEND_MD_BUF;
		msg.send_md_buf_cmd = (struct rmr_msg_send_md_buf_cmd) {
			.sync = clt_pool->sync,
			/* the receiver of buffer is the leader */
			.receiver_id = pool_sess->member_id,
			/* change flags in cmd message */
			.flags = RMR_OP_MD_WRITE,
		};

		err = rmr_clt_send_cmd_with_data(clt_pool, pool_sess, &msg, buf, buflen);
		if (err) {
			pr_debug("Cannot send the clt/srv_md of entire pool to the pool sess %s\n",
				 pool_sess->sessname);
			continue;
		}
	}

	pr_debug("send_md done\n");

	kfree(buf);

	srcu_read_unlock(&clt_pool->sess_list_srcu, idx);
	return err;
}
EXPORT_SYMBOL(rmr_clt_pool_send_md_all);

static int rmr_clt_start_send_md(struct rmr_pool *pool)
{
	return rmr_clt_pool_send_md_all(pool, pool);
}

/**
 * rmr_clt_del_stor_from_pool() - Notify pool members that a storage node is leaving
 *
 * @pool_sess:	The pool session of the departing storage node.
 * @delete:	True for a permanent deletion (%RMR_POOL_INFO_MODE_DELETE);
 *		false for a temporary disassembly (%RMR_POOL_INFO_MODE_DISASSEMBLE).
 *
 * Sends a POOL_INFO REMOVE message to all other active pool members so they
 * can update their dirty maps and membership state accordingly.
 *
 * Return:
 *	0 on success, negative error code on failure.
 */
int rmr_clt_del_stor_from_pool(struct rmr_clt_pool_sess *pool_sess, bool delete)
{
	enum rmr_pool_info_mode mode;
	int err;

	if (delete)
		mode = RMR_POOL_INFO_MODE_DELETE;
	else
		mode = RMR_POOL_INFO_MODE_DISASSEMBLE;

	err = rmr_clt_send_pool_info(pool_sess, RMR_POOL_INFO_OP_REMOVE, mode, false);
	if (err) {
		pr_err("rmr_clt_send_pool_info failed for session\n");
		return err;
	}

	return 0;
}

static int __init rmr_client_init(void)
{
	int err;

	pr_info("Loading module %s, version %s, proto %s\n", KBUILD_MODNAME,
		RMR_VER_STRING, RMR_PROTO_VER_STRING);

	err = rmr_clt_create_sysfs_files();
	if (err) {
		pr_err("Failed to load module,"
		       " creating sysfs device files failed, err: %d\n",
		       err);
		goto out;
	}

	return 0;

out:
	return err;
}

static void __exit rmr_client_exit(void)
{
	struct rmr_pool *pool, *tmp;

	pr_info("Unloading module\n");

	list_for_each_entry_safe(pool, tmp, &pool_list, entry)
		(void) rmr_clt_remove_pool_from_sysfs(pool, NULL);

	rmr_clt_destroy_sysfs_files();
	pr_info("Module unloaded\n");
}

module_init(rmr_client_init);
module_exit(rmr_client_exit);
