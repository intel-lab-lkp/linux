// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Reliable multicast over RTRS (RMR) — client MAP-exchange management
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

void send_map_check(struct rmr_clt_pool_sess *pool_sess)
{
	struct rmr_msg_pool_cmd msg = {};
	struct rmr_pool *pool = pool_sess->pool;
	int err;

	rmr_clt_init_cmd(pool, &msg);
	msg.cmd_type = RMR_CMD_MAP_CHECK;

	err = rmr_clt_pool_send_cmd(pool_sess, &msg, WAIT);
	if (err) {
		pr_err("%s: For sess %s, %s failed with err %d\n",
		       __func__, pool_sess->sessname, rmr_get_cmd_name(msg.cmd_type), err);
		return;
	}
}

void send_store_check(struct rmr_clt_pool_sess *pool_sess)
{
	struct rmr_msg_pool_cmd msg = {};
	struct rmr_pool *pool = pool_sess->pool;
	int err;

	rmr_clt_init_cmd(pool, &msg);
	msg.cmd_type = RMR_CMD_STORE_CHECK;

	err = rmr_clt_pool_send_cmd(pool_sess, &msg, WAIT); //am : why wait ?
	if (err) {
		pr_err("%s: For sess %s, %s failed with err %d\n",
		       __func__, pool_sess->sessname, rmr_get_cmd_name(msg.cmd_type), err);
		pr_err("sess %s failed to send store check with err %d\n",
		       pool_sess->sessname, err);
	}
}

int send_map_get_version(struct rmr_clt_pool_sess *pool_sess, u64 *ver)
{
	struct rmr_msg_pool_cmd_rsp rsp = {};
	struct rmr_msg_pool_cmd msg = {};
	struct rmr_pool *pool = pool_sess->pool;
	int err;

	rmr_clt_init_cmd(pool, &msg);
	msg.cmd_type = RMR_CMD_MAP_GET_VER;

	err = rmr_clt_send_cmd_with_data(pool, pool_sess, &msg, &rsp, sizeof(rsp));
	if (err) {
		pr_err("%s: For sess %s, %s failed with err %d\n",
			__func__, pool_sess->sessname, rmr_get_cmd_name(msg.cmd_type), err);
		return -EINVAL;
	}

	*ver = rsp.value;

	return 0;
}

int send_discard(struct rmr_clt_pool_sess *pool_sess, u8 cmd_type, u8 member_id)
{
	struct rmr_msg_pool_cmd msg = {};
	struct rmr_pool *pool = pool_sess->pool;
	int err;

	rmr_clt_init_cmd(pool, &msg);
	msg.cmd_type = cmd_type;
	msg.send_discard_cmd.member_id = member_id;

	err = rmr_clt_pool_send_cmd(pool_sess, &msg, WAIT);
	if (err) {
		pr_err("%s: For sess %s, %s failed with err %d\n",
		       __func__, pool_sess->sessname, rmr_get_cmd_name(msg.cmd_type), err);
	}

	return err;
}

int rmr_clt_handle_map_check_rsp(struct rmr_clt_pool_sess *pool_sess,
					struct rmr_msg_pool_cmd_rsp *rsp)
{
	struct rmr_pool *pool = pool_sess->pool;
	struct rmr_dirty_id_map *map;

	pr_debug("pool %s sess %s member_id %u, rsp->value=%llu\n",
		 pool->poolname, pool_sess->sessname, rsp->member_id, rsp->value);
	if (!rsp->value) // map is not empty on stg
		return 0;

	pr_debug("pool %s server with id %u has empty dirty map, lets clean it.\n",
		 pool->poolname, rsp->member_id);
	map = rmr_pool_find_map(pool, rsp->member_id);
	if (!map) {
		pr_err("%s: pool %s no map found for member_id %u\n",
		       __func__, pool->poolname, rsp->member_id);
		return -EINVAL;
		//TODO: handle this, how?
	}

	if (!rmr_map_empty(map)) {
		pr_debug("pool %s dirty map for member_id %d is not empty, map->ts %lu (now %lu)\n",
			 pool->poolname, rsp->member_id, map->ts, jiffies);
		if (time_after(jiffies, map->ts + msecs_to_jiffies(RMR_MAP_CLEAN_DELAY_MS))) {
			pr_info("%s: pool %s clear dirty map for member_id %d\n",
				__func__, pool->poolname, rsp->member_id);
			rmr_map_unset_dirty_all(map);
			map->ts = jiffies;
		}
	}

	pr_debug("pool %s map with member_id %u cleaned\n",
		 pool->poolname, map->member_id);
	return 0;
}

int rmr_clt_handle_store_check_rsp(struct rmr_clt_pool_sess *pool_sess,
					  struct rmr_msg_pool_cmd_rsp *rsp)
{
	struct rmr_pool *pool = pool_sess->pool;
	int err = 0;

	pr_debug("pool %s sess %s member_id %u, rsp->value=%llu\n",
		 pool->poolname, pool_sess->sessname, rsp->member_id, rsp->value);
	if (!rsp->value) {
		pr_debug("pool %s sess %s (state=%d) reported that store is not available, changing state\n",
			 pool->poolname, pool_sess->sessname, atomic_read(&pool_sess->state));
		return 0;
	}
	pr_info("pool %s sess %s (state=%d) reported that store is available, changing state\n",
		pool->poolname, pool_sess->sessname, atomic_read(&pool_sess->state));

	pool_sess_change_state(pool_sess, RMR_CLT_POOL_SESS_RECONNECTING);

	if (!pool_sess->maintenance_mode) {
		err = rmr_clt_pool_try_enable(pool);
		if (err) {
			pr_err("%s: pool %s try_enable failed for sess %s: %d\n",
			       __func__, pool->poolname, pool_sess->sessname, err);
			return err;
		}
	}

	return 0;
}

/*
 * Pre-requisite: rcu read lock should be held by caller
 */
static struct rmr_clt_pool_sess *rmr_clt_get_first_reconnecting_session(struct rmr_pool *pool)
{
	struct rmr_clt_pool_sess *pool_sess;

	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		if (atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_RECONNECTING)
			return pool_sess;
	}

	return NULL;
}

/**
 * rmr_clt_pool_map_xfer() - transfer dirty maps between rmr client and server
 *
 * @pool:	the rmr pool used for map transfers
 * @pool_sess:	client pool session that is used for map transfer
 * @cmd_type:	pool command type generated for this transfer, for now only
 *		RMR_CMD_READ_MAP_BUF, RMR_CMD_SEND_MAP_BUF, RMR_CMD_MAP_TEST are used
 * @buf:	pointer to the data buffer for data transfers
 * @buflen:	size of the buffer in bytes
 * @map_idx:	index of the map in dirty map array from which we start to send or receive
 *		the data
 * @offset:	key in the map from which we start to send/receive the data about the maps
 *
 * Description:
 *	Performs transfer of the information about the dirty maps starting from the map with
 *	position map_idx in the array of dirty maps and from the start_key at that map.
 *	cmd types are handled as follows:
 *	RMR_CMD_READ_MAP_BUF - read the information about the maps from the pool and fill buf
 *	RMR_CMD_SEND_MAP_BUF - send buf with filled data to the pull
 *	RMR_CMD_MAP_TEST - send the buf with data to the pool to perform map comparison
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
static int rmr_clt_pool_map_xfer(struct rmr_pool *pool, struct rmr_clt_pool_sess *pool_sess,
				 int cmd_type, void *buf, unsigned int buflen,
				 u8 map_idx, u64 slp_idx)
{
	struct rmr_msg_pool_cmd msg = {};
	int err;

	rmr_clt_init_cmd(pool, &msg);
	msg.cmd_type = cmd_type;

	msg.map_buf_cmd.map_idx = map_idx;
	msg.map_buf_cmd.slp_idx = slp_idx;

	err = rmr_clt_send_cmd_with_data(pool, pool_sess, &msg, buf, buflen);
	if (err) {
		pr_debug("pool %s failed to send map xfer cmd %u, err %d\n",
			 pool->poolname, cmd_type, err);
		return err;
	}

	return 0;
}

int rmr_clt_read_map(struct rmr_pool *pool)
{
	struct rmr_clt_pool_sess *pool_sess = NULL;
	struct rmr_map_buf_hdr *map_buf_hdr;
	u8 map_idx = 0;
	u64 slp_idx = 0;
	int err = 0, buflen, idx;
	void *buf;

	idx = srcu_read_lock(&pool->sess_list_srcu);

	pool_sess = rmr_clt_get_first_reconnecting_session(pool);
	if (pool_sess == NULL) {
		srcu_read_unlock(&pool->sess_list_srcu, idx);
		pr_err("%s: No created session found\n", __func__);
		return -EINVAL;
	}

	buflen = RTRS_IO_LIMIT;
	buf = kzalloc(buflen, GFP_KERNEL);
	if (!buf) {
		pr_err("%s: Error allocating buffer\n", __func__);
		err = -ENOMEM;
		goto ret;
	}

	while (true) {
		err = rmr_clt_pool_map_xfer(pool, pool_sess, RMR_CMD_READ_MAP_BUF,
					    buf, buflen, map_idx, slp_idx);
		if (err) {
			pr_debug("rmr_clt_pool_map_xfer failed for pool %s, err %d\n",
				 pool->poolname, err);
			goto ret_free;
		}

		map_buf_hdr = (struct rmr_map_buf_hdr *)buf;
		if (map_buf_hdr->member_id == 0)
			break;

		err = rmr_pool_save_map(pool, buf, buflen, false);
		if (err) {
			pr_err("rmr_pool_save_map failed\n");
			goto ret_free;
		}

		map_idx = map_buf_hdr->map_idx;
		slp_idx = map_buf_hdr->slp_idx;
	}

ret_free:
	kfree(buf);

ret:
	srcu_read_unlock(&pool->sess_list_srcu, idx);

	return err;
}

/**
 * rmr_clt_spread_map() - Spread the map contained in storage node connected by pool_sess_chosen
 *
 * @pool:		The pool
 * @pool_sess_chosen:	pool session from where the map is to be updated from
 * @enable:		Whether the last MAP_DONE command should have the enable param set or not
 * @skip_normal:	If true, freeze IOs before spreading and silently skip any NORMAL
 *			sessions encountered in the loop (used in Case 1 recovery where
 *			pool_sess_chosen is itself a NORMAL session that is still serving IOs).
 *			If false, encountering a NORMAL session is treated as an error.
 *
 * Description:
 *	This function spreads the map contained in the storage node connected by given pool
 *	session. The param enable denotes whether the map update should result in the storage
 *	going to NORMAL state or not. This is controlled by the enable param in the last MAP_DONE
 *	message.
 *
 * Return:
 *	0 on success
 *	Error value on failure
 *
 * Context:
 *	srcu_read_lock should be held while calling this function.
 */
int rmr_clt_spread_map(struct rmr_pool *pool, struct rmr_clt_pool_sess *pool_sess_chosen,
			      bool enable, bool skip_normal)
{
	struct rmr_clt_pool *clt_pool = (struct rmr_clt_pool *)pool->priv;
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_msg_pool_cmd msg = {};
	int state, err = 0;

	rmr_clt_init_cmd(pool, &msg);

	/*
	 * If we expect NORMAL session, then we should expect IOs running.
	 * Which is why we should freeze IOs before doing map_update.
	 */
	if (skip_normal) {
		/* Freeze IOs */
		rmr_clt_pool_io_freeze(clt_pool);

		/* Wait for all completion */
		rmr_clt_pool_io_wait_complete(clt_pool);
	}

	/*
	 * TODO: Use rmr_clt_handle_discard to check whether the pool
	 * session has pending discard request to be sent.
	 *
	 * Enable this when we fix replace.
	 *
	err = rmr_clt_handle_discard(pool);
	if (err) {
		pr_err("%s: discard handling failed\n", __func__);
		goto err;
	}
	*/

	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		if (pool_sess == pool_sess_chosen)
			continue;

		state = atomic_read(&pool_sess->state);
		if (state == RMR_CLT_POOL_SESS_NORMAL) {
			if (skip_normal)
				continue;
			pr_err("%s: pool %s unexpected NORMAL session %s during spread\n",
			       __func__, pool->poolname, pool_sess->sessname);
			err = -EINVAL;
			goto err_out;
		}

		if (state != RMR_CLT_POOL_SESS_RECONNECTING ||
		    pool_sess->maintenance_mode)
			continue;

		msg.cmd_type = RMR_CMD_MAP_READY;

		err = rmr_clt_pool_send_cmd(pool_sess, &msg, WAIT);
		if (err) {
			pr_err("%s: %s failed\n", __func__, rmr_get_cmd_name(msg.cmd_type));
			goto err_dis;
		}

		msg.cmd_type = RMR_CMD_MAP_SEND;
		msg.map_send_cmd.receiver_member_id = pool_sess->member_id;
		err = rmr_clt_pool_send_cmd(pool_sess_chosen, &msg, WAIT);
		if (err) {
			pr_err("%s: %s failed\n", __func__, rmr_get_cmd_name(msg.cmd_type));
			goto err_dis;
		}

		msg.cmd_type = RMR_CMD_MAP_DONE;
		msg.map_done_cmd.enable = enable;

		err = rmr_clt_pool_send_cmd(pool_sess, &msg, WAIT);
		if (err) {
			pr_err("%s: %s failed\n", __func__, rmr_get_cmd_name(msg.cmd_type));
			goto err_dis;
		}
	}

	/* Unfreeze IOs and wake up */
	if (skip_normal)
		rmr_clt_pool_io_unfreeze(clt_pool);

	return 0;

err_dis:
	list_for_each_entry_srcu(pool_sess, &pool->sess_list, entry,
				 (srcu_read_lock_held(&pool->sess_list_srcu))) {
		if (pool_sess == pool_sess_chosen)
			continue;

		if (atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_NORMAL) {
			if (skip_normal)
				continue;
			pr_err("%s: pool %s unexpected NORMAL session %s during spread\n",
			       __func__, pool->poolname, pool_sess->sessname);
		}

		msg.cmd_type = RMR_CMD_MAP_DISABLE;
		rmr_clt_pool_send_cmd(pool_sess, &msg, WAIT);
	}

err_out:
	/* Unfreeze IOs and wake up */
	if (skip_normal)
		rmr_clt_pool_io_unfreeze(clt_pool);

	return err;
}

/**
 * rmr_clt_set_pool_sess_mm() - Set the rmr clt pool session to maintenance mode
 *
 * @pool_sess:	The rmr clt pool session to set in maintenance mode
 *
 * Description:
 *	This function does the necessary work required, like setting the pool session to
 *	maintenance mode and updating the state.
 *	It then also communicates this state change to the corresponding storage node.
 *
 * Return:
 *	0 on success
 *	Error value on failure
 */
int rmr_clt_set_pool_sess_mm(struct rmr_clt_pool_sess *pool_sess)
{
	struct rmr_pool *pool = pool_sess->pool;
	int err;

	pr_info("%s: Putting sess %s of pool %s in maintenance mode\n",
		__func__, pool_sess->sessname, pool->poolname);

	if (pool_sess->maintenance_mode)
		goto send_message;

	/*
	 * If the pool_sess is to be put in maintenance mode,
	 * update relevant states and params, Then send message to storage node.
	 *
	 * We do not need any kind of locking for this, because of the way IO units (IU) are
	 * allocated & sent. The mm mode update & the state change can happen at multiple places.
	 *
	 * 1) If the state changes before the pool_sess is picked up into the IU, then we are safe
	 * 2) If the state changes after the pool_sess is picked up into the IU, but before,
	 * rmr_clt_request, it will be failed in rmr_clt_request.
	 * 3) If the state changes after rmr_clt_request, the IO would be sent to the storage node
	 * for that pool_sess. Then we have 2 cases,
	 *   a) The message for maintenance_mode is received by the storage node before the IO,
	 *   then the storage node will fail the IO. Failure would then be handled by the client.
	 *   b) The message for maintenance_mode is received by the storage node after the IO,
	 *   then the storage node will process the IO, and return success to client. In this case
	 *   also we are fine, since the IO got processes successfully.
	 */
	pool->map_ver++;
	pool_sess->maintenance_mode = true;
	pool_sess_change_state(pool_sess, RMR_CLT_POOL_SESS_RECONNECTING);

send_message:
	err = send_msg_enable_pool(pool_sess, 0);
	if (err) {
		pr_err("%s: send_msg_enable_pool failed for pool %s. Err %d\n",
		       __func__, pool->poolname, err);
	}

	return err;
}

/**
 * rmr_clt_unset_pool_sess_mm() - Clear the rmr clt pool sessions maintenance mode
 *
 * @pool_sess:	The rmr clt pool session to clear maintenance mode of
 *
 * Description:
 *	This function clears the maintenance mode of the given rmr clt pool session.
 *	It also does the map_update which essentially brings the pool_session and its
 *	corresponding storage node to NORMAL state.
 *
 * Return:
 *	0 on success
 *	Error value on failure
 */
int rmr_clt_unset_pool_sess_mm(struct rmr_clt_pool_sess *pool_sess)
{
	struct rmr_pool *pool = pool_sess->pool;
	int err;

	pr_info("%s: Putting to sess %s of pool %s out of maintenance mode\n",
		__func__, pool_sess->sessname, pool->poolname);

	/*
	 * Cannot be in NORMAL and CREATED states while in maintenance mode.
	 */
	WARN_ON(atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_NORMAL);
	WARN_ON(atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_CREATED);

	/*
	 * If this pool_sess is getting removed, we fail unset maintenance mode
	 */
	if (atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_REMOVING)
		return -EINVAL;

	/*
	 * First unset mm of storage node
	 */
	err = send_msg_enable_pool(pool_sess, 1);
	if (err) {
		pr_err("Failed to send enable to pool %s. Err %d\n",
		       pool->poolname, err);
		return -EINVAL;
	}

	/* Now do this */
	pool_sess->maintenance_mode = false;

	/*
	 * For FAILED states, further action would happen when it goes to RECONNECTING state
	 */
	if (atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_FAILED)
		return 0;

	/*
	 * Since we are in RECONNECTING state, we do map update here.
	 */
	err = rmr_clt_pool_try_enable(pool);
	if (err) {
		pr_err("%s: pool %s try_enable failed for sess %s: %d\n",
		       __func__, pool->poolname, pool_sess->sessname, err);
		return err;
	}

	return 0;
}

void msg_pool_cmd_map_content_conf(struct work_struct *work)
{
	struct rmr_clt_sess_iu *sess_iu = container_of(work, struct rmr_clt_sess_iu, work);
	struct rmr_clt_pool_sess *pool_sess = sess_iu->pool_sess;

	pr_debug("%s: session %s conf with errno %d\n",
		 __func__, pool_sess->sessname, sess_iu->errno);

	wake_up_iu_comp(sess_iu);
	rmr_msg_put_iu(pool_sess, sess_iu);
}

static void send_map_update_done(struct work_struct *work)
{
	struct rmr_clt_sess_iu *sess_iu = container_of(work, struct rmr_clt_sess_iu, work);
	struct rmr_iu *iu = sess_iu->rmr_iu;
	struct rmr_clt_pool_sess *pool_sess = sess_iu->pool_sess;
	int errno = sess_iu->errno;

	pr_debug("%s: Session %s, err %d, iu %p\n",
		 __func__, pool_sess->sessname, errno, iu);
	WARN_ON(atomic_read(&pool_sess->state) == RMR_CLT_POOL_SESS_CREATED);

	/*
	 * We leave "iu->errno" set from the IO failure.
	 * Even though one map_add succeeds, we clear `iu->errno`
	 * and the main IO succeeds. And all other map_adds
	 * simply trigger session state change to FAILURE.
	 */
	if (!errno) {
		iu->errno = 0;
	} else {
		pr_err_ratelimited("%s: for sess %s got errno: %d\n",
				__func__, pool_sess->sessname, errno);

		if (iu->errno)
			/* only the last error is reported */
			iu->errno = errno;
		pool_sess_change_state(pool_sess, RMR_CLT_POOL_SESS_FAILED);
	}

	pr_debug("%s: Before dec and test iu %p refcnt=%d\n",
		 __func__, iu, refcount_read(&iu->refcount));

	if (refcount_dec_and_test(&iu->refcount)) {
		rmr_conf_fn *conf = iu->conf;

		pr_debug("all maps updated, call conf %p withh errno %d\n",
			 conf, errno);
		(*conf)(iu->priv, iu->errno);
	}
}

/**
 * rmr_clt_send_map_update() - Send map update to all connected storage nodes
 *
 * @pool:	The client pool of whose sessions the update is to be sent
 * @iu:		The IO unit containing the information for the update
 *
 * Description:
 *	Send map update, using the underlying RTRS <-> RDMA
 *	Currently we use the same rmr_iu as IO, since it saves us time.
 *	When an IO fails, and a MAP_ADD is to be sent, the code reuses the
 *	same rmr_iu used for IO. This way we do not spend time acquiring
 *	and initializing another rmr_iu.
 *
 *	A map update currently can either be a MAP_ADD or a MAP_CLEAR.
 *	The caller must make sure the basic and required information for both
 *	the above commands is updated in the rmr_iu.
 *	Basic being the pool group_id, msg hdr type, etc.
 *	Required being the following,
 *		MAP_ADD requires the rmr_id_t chunk numbers, failed_id array and failed_cnt
 *		MAP_CLEAR requires the rmr_id_t and the member_id
 *
 * Return:
 *	0 on success. This means the map_update was sent successfully.
 *	The subsequent status (err or not) goes to iu->conf call,
 *	so the caller should check that too.
 *
 *	Error value on failure. When this function returns error,
 *	be aware that the iu->conf will not be called.
 */
int rmr_clt_send_map_update(struct rmr_pool *pool, struct rmr_iu *iu)
{
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_clt_sess_iu *sess_iu, *tmp_sess_iu;
	struct rtrs_clt_req_ops req_ops;
	struct kvec vec;
	int err;

	pr_debug("%s: rmr_id (%llu, %llu), msg %d, refcnt=%d\n", __func__,
		 iu->msg.id_a, iu->msg.id_b, iu->msg.hdr.type, refcount_read(&iu->refcount));

	if (!pool) {
		pr_err("Cannot send map update. pool is NULL\n");
		return -EINVAL;
	}

	rmr_get_iu(iu);

	vec = (struct kvec){
		.iov_base = &iu->msg,
		.iov_len = sizeof(iu->msg)
	};

	list_for_each_entry_safe(sess_iu, tmp_sess_iu, &(iu->sess_list), entry) {
		struct rmr_clt_sess *clt_sess;
		enum rmr_clt_pool_sess_state state;

		pool_sess = sess_iu->pool_sess;
		clt_sess = pool_sess->clt_sess;

		INIT_WORK(&sess_iu->work, send_map_update_done);

		req_ops = (struct rtrs_clt_req_ops) {
			.priv = sess_iu,
			.conf_fn = msg_conf,
		};

		state = atomic_read(&pool_sess->state);
		if (state == RMR_CLT_POOL_SESS_FAILED ||
		    state == RMR_CLT_POOL_SESS_REMOVING) {
			/*
			 * Sessions in failed state is probably the reason why we sending
			 * map add in the first place.
			 * We can skip those sessions, since map update will take care of this.
			 */
			pr_debug("%s: skipped sess %s\n", __func__, sess_iu->pool_sess->sessname);
			sess_iu->errno = -EINVAL;
			schedule_work(&sess_iu->work);
			continue;
		}

		pr_debug("Sending request flags %u to pool %s session %s "
			 "chunk [%llu, %llu] offset %u length %u)\n",
			 iu->msg.flags, pool->poolname, pool_sess->sessname,
			 iu->msg.id_a, iu->msg.id_b,
			 iu->msg.offset, iu->msg.length);

		trace_send_map_update(WRITE, sess_iu);

		err = rtrs_clt_request(WRITE, &req_ops, clt_sess->rtrs,
				       sess_iu->permit, &vec, 1, 0, NULL, 0);

		/* we can ignore errno since we called rmr_clt_send_map_update with NO_WAIT */
		if (err) {
			sess_iu->errno = err;

			pr_err("%s: Failed with err %d, schedule work\n",
			       __func__, err);
			schedule_work(&sess_iu->work);
		}
	}
	rmr_put_iu(iu);

	/*
	 * We are handling err through iu->conf
	 */
	return 0;
}
EXPORT_SYMBOL(rmr_clt_send_map_update);

int rmr_clt_map_add_id(struct rmr_pool *pool, int stg_id, rmr_id_t id)
{
	struct rmr_dirty_id_map *map;

	map = rmr_pool_find_map(pool, stg_id);
	if (!map) {
		pr_err("in pool %s cannot find map for member_id %u\n",
		       pool->poolname, stg_id);
		return -EINVAL;
	}

	map->ts = jiffies;
	rmr_map_set_dirty(map, id, 0);

	pr_debug("pool %s id (%llu, %llu) inserted to the dirty map\n",
		 pool->poolname, id.a, id.b);

	return 0;
}

void sched_map_add(struct work_struct *work)
{
	struct rmr_iu *iu = container_of(work, struct rmr_iu, work);
	struct rmr_pool *pool = iu->pool;
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_clt_sess_iu *sess_iu;
	rmr_conf_fn *clt_conf = iu->conf;
	void *clt_priv = iu->priv;
	int failed_cnt = 0, err = 0;
	rmr_id_t id;

	pr_debug("scheduled work process for rmr iu %p send map add id (%llu, %llu), poolname %s\n",
		 iu, iu->msg.id_a, iu->msg.id_b, pool->poolname);

	/*
	 * For MAP_ADD, we need failed_id, failed_cnt, and rmr_id_t for chunk number.
	 *
	 * We reuse the iu which was used for this IO.
	 * It already has the chunk number, the clt_conf function to be called,
	 * and other important things.
	 */
	iu->msg.hdr.type = cpu_to_le16(RMR_MSG_MAP_ADD);

	id.a = le64_to_cpu(iu->msg.id_a);
	id.b = le64_to_cpu(iu->msg.id_b);
	list_for_each_entry(sess_iu, &(iu->sess_list), entry) {
		pool_sess = sess_iu->pool_sess;

		if (sess_iu->errno) {
			iu->msg.map_ver = cpu_to_le64(pool->map_ver);
			iu->msg.failed_id[failed_cnt] = pool_sess->member_id;
			failed_cnt++;

			rmr_clt_map_add_id(pool, pool_sess->member_id, id);
		}
	}
	iu->msg.failed_cnt = failed_cnt;

	err = rmr_clt_send_map_update(pool, iu);
	if (err) {
		pr_err("error sending map add for id (%llu, %llu), err=%d\n",
		       iu->msg.id_a, iu->msg.id_b, err);
		(*clt_conf)(clt_priv, err);
	}
}

/**
 * rmr_clt_send_map() - Send dirty map entries
 *
 * @map_src_pool:	Pool whose map is to be sent
 * @clt_pool:		Client pool through which the dest session is selected
 * @map_send_cmd:	Command structure containing the member_id of the target session
 *			where the map is to be sent. If NULL then send to all of the session
 *
 * Return:
 *	0 on success, err code otherwise.
 *
 * Description:
 *	Sends all the dirty entries from the map in "map_src_pool" to the session with
 *	member_id equal to member_id mentioned in the map_send_cmd.
 *	The session where to send the map is picked from the clt_pool. If
 *	map_send_cmd is NULL then send cmd to all of the sessions in clt_pool.
 *
 * Context:
 *	This function blocks while sending the map.
 */
int rmr_clt_send_map(struct rmr_pool *map_src_pool, struct rmr_pool *clt_pool,
		     const struct rmr_msg_map_send_cmd *map_send_cmd, rmr_map_filter filter)
{
	struct rmr_clt_pool_sess *pool_sess;
	struct rmr_msg_pool_cmd msg = {};
	bool sess_found = false;
	void *bitmap_buf;
	int err = 0, idx;

	if (!clt_pool) {
		pr_err("Cannot send map, when clt_pool is NULL\n");
		return -EINVAL;
	}

	bitmap_buf = kzalloc(RTRS_IO_LIMIT, GFP_KERNEL);
	if (!bitmap_buf) {
		pr_err("%s: pool %s error allocating buffer to send map\n",
		       __func__, map_src_pool->poolname);
		return -ENOMEM;
	}

	idx = srcu_read_lock(&clt_pool->sess_list_srcu);
	list_for_each_entry_srcu(pool_sess, &clt_pool->sess_list, entry,
				 (srcu_read_lock_held(&clt_pool->sess_list_srcu))) {
		int bytes = 0;
		u8 map_idx = 0;
		u64 slp_idx = 0;

		/* if we have a command then skip all the sessions that are not in command */
		if (map_send_cmd && pool_sess->member_id != map_send_cmd->receiver_member_id)
			continue;

		sess_found = true;
		pr_info("Start sending dirty map for pool %s; to session %s with member_id %d\n",
			map_src_pool->poolname, pool_sess->sessname, pool_sess->member_id);

		while ((bytes = rmr_pool_maps_to_buf(map_src_pool, &map_idx, &slp_idx,
						     bitmap_buf, RTRS_IO_LIMIT, filter)) > 0) {
			pr_debug("mapped %d bytes to bitmap_buf\n", bytes);

			err = rmr_clt_pool_map_xfer(clt_pool, pool_sess, RMR_CMD_SEND_MAP_BUF,
						    bitmap_buf, bytes, 0, 0);
			if (err) {
				pr_err("%s: Failed to send bitmap_buf, from %s to %s err %d\n",
				       __func__, map_src_pool->poolname, clt_pool->poolname, err);
				goto err_free;
			}
		}

		rmr_clt_init_cmd(map_src_pool, &msg);
		msg.cmd_type = RMR_CMD_MAP_BUF_DONE;
		msg.map_buf_done_cmd.map_version = map_src_pool->map_ver;

		err = rmr_clt_pool_send_cmd(pool_sess, &msg, WAIT);
		if (err) {
			pr_err("%s: For pool %s, %s failed\n",
			       __func__, map_src_pool->poolname, rmr_get_cmd_name(msg.cmd_type));
			goto err_free;
		}
	}

	if (map_send_cmd && !sess_found) {
		pr_err("pool %s failed to find sess with member_id %u to send map\n",
		       clt_pool->poolname, map_send_cmd->receiver_member_id);
		err = -EINVAL;
		goto err_free;
	}

	pr_info("%s: Sending map done\n", __func__);

err_free:
	kfree(bitmap_buf);
	srcu_read_unlock(&clt_pool->sess_list_srcu, idx);

	return err;
}
EXPORT_SYMBOL(rmr_clt_send_map);

int rmr_clt_test_map(struct rmr_pool *src_pool, struct rmr_pool *dst_pool)
{
	struct rmr_clt_pool_sess *pool_sess;
	void *bitmap_buf;
	int err, idx;

	pr_info("test maps from src_pool=%s to dst_pool=%s...\n",
		src_pool->poolname, dst_pool->poolname);

	bitmap_buf = kzalloc(RTRS_IO_LIMIT, GFP_KERNEL);
	if (!bitmap_buf) {
		pr_err("%s: Error allocating buffer\n", __func__);
		err = -ENOMEM;
		goto err;
	}

	idx = srcu_read_lock(&dst_pool->sess_list_srcu);
	list_for_each_entry_srcu(pool_sess, &dst_pool->sess_list, entry,
				 (srcu_read_lock_held(&dst_pool->sess_list_srcu))) {
		enum rmr_clt_pool_sess_state state;
		int bytes = 0;
		u8 map_idx = 0;
		u64 slp_idx = 0;

		state = atomic_read(&pool_sess->state);
		if (state == RMR_CLT_POOL_SESS_CREATED ||
		    state == RMR_CLT_POOL_SESS_FAILED) {
			pr_warn("sess %s is in created/failed state, skip map test.\n",
				pool_sess->sessname);
			continue;
		}
		pr_info("perform map test for sess %s\n", pool_sess->sessname);
		while ((bytes = rmr_pool_maps_to_buf(src_pool, &map_idx, &slp_idx,
						     bitmap_buf, RTRS_IO_LIMIT,
						     MAP_NO_FILTER)) > 0) {
			pr_debug("mapped %d bytes to bitmap_buf\n", bytes);

			err = rmr_clt_pool_map_xfer(dst_pool, pool_sess, RMR_CMD_MAP_TEST,
						    bitmap_buf, bytes, 0, 0);
			if (err) {
				pr_err("%s: For sess %s failed test map, src_pool %s dst_pool %s err %d\n",
				       __func__, pool_sess->sessname, src_pool->poolname,
				       dst_pool->poolname, err);
				srcu_read_unlock(&dst_pool->sess_list_srcu, idx);
				goto err_free;
			}
		}
		pr_info("sess %s map test done\n", pool_sess->sessname);
	}
	srcu_read_unlock(&dst_pool->sess_list_srcu, idx);

err_free:
	kfree(bitmap_buf);
err:
	pr_info("test maps from src_pool=%s to dst_pool=%s done, err %d\n",
		src_pool->poolname, dst_pool->poolname, err);

	return err;
}
EXPORT_SYMBOL(rmr_clt_test_map);
