// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2016-2018, The Linux Foundation. All rights reserved.
 */

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/wait.h>

#include <soc/qcom/rpmh.h>

#include "rpmh-internal.h"

#define RPMH_TIMEOUT_MS			msecs_to_jiffies(10000)

#define DEFINE_RPMH_MSG_ONSTACK(device, s, name)	\
	struct rpmh_request name = {			\
		.msg = {				\
			.state = s,			\
			.cmds = name.cmd,		\
			.num_cmds = 0,			\
			.wait_for_compl = true,		\
		},					\
		.cmd = { { 0 } },			\
		.dev = device,				\
	}

#define ctrlr_to_drv(ctrlr) container_of(ctrlr, struct rsc_drv, client)

/**
 * struct cache_req - the request object for caching
 *
 * @addr: the address of the resource
 * @sleep_val: the sleep vote
 * @wake_val: the wake vote
 * @list: linked list obj
 */
struct cache_req {
	u32 addr;
	u32 sleep_val;
	u32 wake_val;
	struct list_head list;
};

/**
 * struct batch_cache_req - An entry in our batch cache
 *
 * @list: linked list obj
 * @count: number of messages
 * @rpm_msgs: the messages
 */

struct batch_cache_req {
	struct list_head list;
	int count;
	struct rpmh_request rpm_msgs[];
};

static struct rpmh_ctrlr *get_rpmh_ctrlr(const struct device *dev)
{
	struct rsc_drv *drv = dev_get_drvdata(dev->parent);

	return &drv->client;
}

static void rpmh_put_req(struct rpmh_request *rpm_msg)
{
	if (refcount_dec_and_test(&rpm_msg->refcnt))
		kfree(rpm_msg);
}

/**
 * rpmh_tx_done() - Signal the completion of a RPMH transfer.
 * @msg: The request that was previously sent.
 *
 * Called by the RSC driver when an active-only transfer is complete.
 * Signals any blocking waiter and drops the reference owned by the
 * controller.
 */
void rpmh_tx_done(const struct tcs_request *msg)
{
	struct rpmh_request *rpm_msg = container_of(msg, struct rpmh_request,
						    msg);

	/* Signal the blocking thread we are done, if there is one */
	complete(&rpm_msg->completion);
	rpmh_put_req(rpm_msg);
}

static struct cache_req *__find_req(struct rpmh_ctrlr *ctrlr, u32 addr)
{
	struct cache_req *p, *req = NULL;

	list_for_each_entry(p, &ctrlr->cache, list) {
		if (p->addr == addr) {
			req = p;
			break;
		}
	}

	return req;
}

static struct cache_req *cache_rpm_request(struct rpmh_ctrlr *ctrlr,
					   enum rpmh_state state,
					   struct tcs_cmd *cmd)
{
	struct cache_req *req;
	unsigned long flags;
	u32 old_sleep_val, old_wake_val;

	spin_lock_irqsave(&ctrlr->cache_lock, flags);
	req = __find_req(ctrlr, cmd->addr);
	if (req)
		goto existing;

	req = kzalloc_obj(*req, GFP_ATOMIC);
	if (!req) {
		req = ERR_PTR(-ENOMEM);
		goto unlock;
	}

	req->addr = cmd->addr;
	req->sleep_val = req->wake_val = UINT_MAX;
	list_add_tail(&req->list, &ctrlr->cache);

existing:
	old_sleep_val = req->sleep_val;
	old_wake_val = req->wake_val;

	switch (state) {
	case RPMH_ACTIVE_ONLY_STATE:
	case RPMH_WAKE_ONLY_STATE:
		req->wake_val = cmd->data;
		break;
	case RPMH_SLEEP_STATE:
		req->sleep_val = cmd->data;
		break;
	}

	ctrlr->dirty |= (req->sleep_val != old_sleep_val ||
			 req->wake_val != old_wake_val) &&
			 req->sleep_val != UINT_MAX &&
			 req->wake_val != UINT_MAX;

unlock:
	spin_unlock_irqrestore(&ctrlr->cache_lock, flags);

	return req;
}

/**
 * __rpmh_write() - Cache and send the RPMH request.
 *
 * @dev: The device making the request
 * @state: Active/Sleep request type
 * @rpm_msg: The data that needs to be sent (cmds).
 *
 * Cache the RPMH request and send if the state is ACTIVE_ONLY.
 * SLEEP/WAKE_ONLY requests are not sent to the controller at
 * this time. Use rpmh_flush() to send them to the controller.
 *
 * The controller's reference to @rpm_msg is consumed, either by
 * rpmh_tx_done() or on error.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int __rpmh_write(const struct device *dev, enum rpmh_state state,
			struct rpmh_request *rpm_msg)
{
	struct rpmh_ctrlr *ctrlr = get_rpmh_ctrlr(dev);
	int ret = -EINVAL;
	struct cache_req *req;
	int i;

	if (rpm_msg->msg.is_read)
		goto send_data;

	/* Cache the request in our store and link the payload */
	for (i = 0; i < rpm_msg->msg.num_cmds; i++) {
		req = cache_rpm_request(ctrlr, state, &rpm_msg->msg.cmds[i]);
		if (IS_ERR(req)) {
			rpmh_put_req(rpm_msg);
			return PTR_ERR(req);
		}
	}

send_data:
	if (state == RPMH_ACTIVE_ONLY_STATE) {
		ret = rpmh_rsc_send_data(ctrlr_to_drv(ctrlr), &rpm_msg->msg);
		if (ret)
			rpmh_put_req(rpm_msg);
	} else {
		/* Clean up our call by spoofing tx_done */
		ret = 0;
		rpmh_tx_done(&rpm_msg->msg);
	}

	return ret;
}

static int __fill_rpmh_msg(struct rpmh_request *req, enum rpmh_state state,
		const struct tcs_cmd *cmd, u32 n, bool is_read)
{
	if (!cmd || !n || n > MAX_RPMH_PAYLOAD)
		return -EINVAL;

	memcpy(req->cmd, cmd, n * sizeof(*cmd));

	req->msg.state = state;
	req->msg.cmds = req->cmd;
	req->msg.num_cmds = n;
	req->msg.is_read = is_read;

	return 0;
}

/*
 * Allocate a request that can be handed to the controller. The request starts
 * with a single reference, owned by the controller and dropped in
 * rpmh_tx_done(). Blocking callers must take an additional reference for
 * themselves, so that the request outlives a timed out wait in case the
 * hardware completes it later.
 */
static struct rpmh_request *rpmh_alloc_req(const struct device *dev,
					   enum rpmh_state state,
					   const struct tcs_cmd *cmd, u32 n,
					   bool is_read, bool wait_for_compl)
{
	struct rpmh_request *rpm_msg;
	int ret;

	rpm_msg = kzalloc_obj(*rpm_msg, GFP_ATOMIC);
	if (!rpm_msg)
		return ERR_PTR(-ENOMEM);

	ret = __fill_rpmh_msg(rpm_msg, state, cmd, n, is_read);
	if (ret) {
		kfree(rpm_msg);
		return ERR_PTR(ret);
	}

	rpm_msg->msg.wait_for_compl = wait_for_compl;
	rpm_msg->dev = dev;
	init_completion(&rpm_msg->completion);
	refcount_set(&rpm_msg->refcnt, 1);

	return rpm_msg;
}

/*
 * Send a request and wait for it to complete. If @data is not NULL, the
 * request is a read of the single command in @cmd and the response is
 * returned in @data.
 */
static int __rpmh_write_wait(const struct device *dev, enum rpmh_state state,
			     const struct tcs_cmd *cmd, u32 n, u32 *data)
{
	struct rpmh_request *rpm_msg;
	int ret;

	rpm_msg = rpmh_alloc_req(dev, state, cmd, n, !!data, true);
	if (IS_ERR(rpm_msg))
		return PTR_ERR(rpm_msg);

	/* Our own reference, the controller owns the initial one */
	refcount_inc(&rpm_msg->refcnt);

	ret = __rpmh_write(dev, state, rpm_msg);
	if (ret)
		goto out;

	if (!wait_for_completion_timeout(&rpm_msg->completion,
					 RPMH_TIMEOUT_MS)) {
		/*
		 * The request is still owned by the controller and is freed
		 * by rpmh_tx_done() if the hardware ever completes it. Reads
		 * are known to time out on some platforms, so only warn about
		 * writes.
		 */
		WARN_ON(!data);
		ret = -ETIMEDOUT;
		goto out;
	}

	if (data)
		*data = rpm_msg->cmd[0].data;

out:
	rpmh_put_req(rpm_msg);

	return ret;
}

/**
 * rpmh_read: Read a resource value
 *
 * @dev: The device making the request
 * @cmd: The payload having address of resource to read
 *
 * Reads the value for the resource address given in tcs_cmd->addr
 * and returns the tcs_cmd->data filled with same.
 *
 * Context: May sleep. Do not call from atomic contexts.
 *
 * Return: 0 on success, negative errno on failure
 */
int rpmh_read(const struct device *dev, struct tcs_cmd *cmd)
{
	if (!cmd)
		return -EINVAL;

	return __rpmh_write_wait(dev, RPMH_ACTIVE_ONLY_STATE, cmd, 1,
				 &cmd->data);
}
EXPORT_SYMBOL_GPL(rpmh_read);

/**
 * rpmh_write_async() - Write a set of RPMH commands.
 *
 * @dev: The device making the request
 * @state: Active/sleep set
 * @cmd: The payload data
 * @n: The number of elements in payload
 *
 * Write a set of RPMH commands, the order of commands is maintained
 * and will be sent as a single shot.
 *
 * Return: 0 on success, negative error code on failure.
 */
int rpmh_write_async(const struct device *dev, enum rpmh_state state,
		     const struct tcs_cmd *cmd, u32 n)
{
	struct rpmh_request *rpm_msg;

	rpm_msg = rpmh_alloc_req(dev, state, cmd, n, false, false);
	if (IS_ERR(rpm_msg))
		return PTR_ERR(rpm_msg);

	return __rpmh_write(dev, state, rpm_msg);
}
EXPORT_SYMBOL_GPL(rpmh_write_async);

/**
 * rpmh_write() - Write a set of RPMH commands and block until response
 *
 * @dev: The device making the request
 * @state: Active/sleep set
 * @cmd: The payload data
 * @n: The number of elements in @cmd
 *
 * May sleep. Do not call from atomic contexts.
 *
 * Return: 0 on success, negative error code on failure.
 */
int rpmh_write(const struct device *dev, enum rpmh_state state,
	       const struct tcs_cmd *cmd, u32 n)
{
	return __rpmh_write_wait(dev, state, cmd, n, NULL);
}
EXPORT_SYMBOL_GPL(rpmh_write);

static void cache_batch(struct rpmh_ctrlr *ctrlr, struct batch_cache_req *req)
{
	unsigned long flags;

	spin_lock_irqsave(&ctrlr->cache_lock, flags);
	list_add_tail(&req->list, &ctrlr->batch_cache);
	ctrlr->dirty = true;
	spin_unlock_irqrestore(&ctrlr->cache_lock, flags);
}

static int flush_batch(struct rpmh_ctrlr *ctrlr)
{
	struct batch_cache_req *req;
	const struct rpmh_request *rpm_msg;
	int ret = 0;
	int i;

	/* Send Sleep/Wake requests to the controller, expect no response */
	list_for_each_entry(req, &ctrlr->batch_cache, list) {
		for (i = 0; i < req->count; i++) {
			rpm_msg = req->rpm_msgs + i;
			ret = rpmh_rsc_write_ctrl_data(ctrlr_to_drv(ctrlr),
						       &rpm_msg->msg);
			if (ret)
				break;
		}
	}

	return ret;
}

static int rpmh_write_batch_active(const struct device *dev,
				   const struct tcs_cmd *cmd, u32 *n,
				   int count)
{
	struct rsc_drv *drv = ctrlr_to_drv(get_rpmh_ctrlr(dev));
	struct rpmh_request **rpm_msgs;
	unsigned long time_left;
	int ret = 0, nr_alloc, i, j;

	rpm_msgs = kzalloc_objs(*rpm_msgs, count, GFP_ATOMIC);
	if (!rpm_msgs)
		return -ENOMEM;

	for (nr_alloc = 0; nr_alloc < count; nr_alloc++) {
		struct rpmh_request *rpm_msg;

		rpm_msg = rpmh_alloc_req(dev, RPMH_ACTIVE_ONLY_STATE, cmd,
					 n[nr_alloc], false, false);
		if (IS_ERR(rpm_msg)) {
			ret = PTR_ERR(rpm_msg);
			break;
		}

		/* Our own reference, the controller owns the initial one */
		refcount_inc(&rpm_msg->refcnt);
		rpm_msgs[nr_alloc] = rpm_msg;
		cmd += n[nr_alloc];
	}

	for (i = 0; !ret && i < count; i++) {
		ret = rpmh_rsc_send_data(drv, &rpm_msgs[i]->msg);
		if (ret) {
			pr_err("Error(%d) sending RPMH message addr=%#x\n",
			       ret, rpm_msgs[i]->msg.cmds[0].addr);
			break;
		}
	}

	/* The controller never got the requests that were not sent */
	for (j = i; j < nr_alloc; j++)
		rpmh_put_req(rpm_msgs[j]);

	time_left = RPMH_TIMEOUT_MS;
	while (i--) {
		time_left = wait_for_completion_timeout(&rpm_msgs[i]->completion,
							time_left);
		if (WARN_ON(!time_left)) {
			/*
			 * The requests still in flight are owned by the
			 * controller and are freed by rpmh_tx_done() if the
			 * hardware ever completes them.
			 */
			ret = -ETIMEDOUT;
			break;
		}
	}

	while (nr_alloc--)
		rpmh_put_req(rpm_msgs[nr_alloc]);
	kfree(rpm_msgs);

	return ret;
}

/**
 * rpmh_write_batch() - Write multiple sets of RPMH commands and wait for the
 * batch to finish.
 *
 * @dev: the device making the request
 * @state: Active/sleep set
 * @cmd: The payload data
 * @n: The array of count of elements in each batch, 0 terminated.
 *
 * If the request state is ACTIVE_ONLY, the requests are sent to the
 * controller immediately and the function waits until all commands are
 * complete. If the state is SLEEP or WAKE_ONLY, the requests are cached
 * and the function returns immediately; they are sent to the controller
 * later by rpmh_flush().
 *
 * May sleep. Do not call from atomic contexts for ACTIVE_ONLY requests.
 *
 * Return: 0 on success, negative error code on failure.
 */
int rpmh_write_batch(const struct device *dev, enum rpmh_state state,
		     const struct tcs_cmd *cmd, u32 *n)
{
	struct batch_cache_req *req;
	struct rpmh_ctrlr *ctrlr = get_rpmh_ctrlr(dev);
	int count = 0;
	int i;

	if (!cmd || !n)
		return -EINVAL;

	while (n[count] > 0)
		count++;
	if (!count)
		return -EINVAL;

	if (state == RPMH_ACTIVE_ONLY_STATE)
		return rpmh_write_batch_active(dev, cmd, n, count);

	req = kzalloc_flex(*req, rpm_msgs, count, GFP_ATOMIC);
	if (!req)
		return -ENOMEM;

	req->count = count;

	for (i = 0; i < count; i++) {
		__fill_rpmh_msg(req->rpm_msgs + i, state, cmd, n[i], false);
		cmd += n[i];
	}

	cache_batch(ctrlr, req);

	return 0;
}
EXPORT_SYMBOL_GPL(rpmh_write_batch);

static int is_req_valid(struct cache_req *req)
{
	return (req->sleep_val != UINT_MAX &&
		req->wake_val != UINT_MAX &&
		req->sleep_val != req->wake_val);
}

static int send_single(struct rpmh_ctrlr *ctrlr, enum rpmh_state state,
		       u32 addr, u32 data)
{
	DEFINE_RPMH_MSG_ONSTACK(NULL, state, rpm_msg);

	/* Wake sets are always complete and sleep sets are not */
	rpm_msg.msg.wait_for_compl = (state == RPMH_WAKE_ONLY_STATE);
	rpm_msg.cmd[0].addr = addr;
	rpm_msg.cmd[0].data = data;
	rpm_msg.msg.num_cmds = 1;

	return rpmh_rsc_write_ctrl_data(ctrlr_to_drv(ctrlr), &rpm_msg.msg);
}

/**
 * rpmh_flush() - Flushes the buffered sleep and wake sets to TCSes
 *
 * @ctrlr: Controller making request to flush cached data
 *
 * Return:
 * * 0          - Success
 * * Error code - Otherwise
 */
int rpmh_flush(struct rpmh_ctrlr *ctrlr)
{
	struct cache_req *p;
	int ret = 0;

	lockdep_assert_irqs_disabled();

	/*
	 * Currently rpmh_flush() is only called when we think we're running
	 * on the last processor.  If the lock is busy it means another
	 * processor is up and it's better to abort than spin.
	 */
	if (!spin_trylock(&ctrlr->cache_lock))
		return -EBUSY;

	if (!ctrlr->dirty) {
		pr_debug("Skipping flush, TCS has latest data.\n");
		goto write_next_wakeup;
	}

	/* Invalidate the TCSes first to avoid stale data */
	rpmh_rsc_invalidate(ctrlr_to_drv(ctrlr));

	/* First flush the cached batch requests */
	ret = flush_batch(ctrlr);
	if (ret)
		goto exit;

	list_for_each_entry(p, &ctrlr->cache, list) {
		if (!is_req_valid(p)) {
			pr_debug("%s: skipping RPMH req: a:%#x s:%#x w:%#x",
				 __func__, p->addr, p->sleep_val, p->wake_val);
			continue;
		}
		ret = send_single(ctrlr, RPMH_SLEEP_STATE, p->addr,
				  p->sleep_val);
		if (ret)
			goto exit;
		ret = send_single(ctrlr, RPMH_WAKE_ONLY_STATE, p->addr,
				  p->wake_val);
		if (ret)
			goto exit;
	}

	ctrlr->dirty = false;

write_next_wakeup:
	rpmh_rsc_write_next_wakeup(ctrlr_to_drv(ctrlr));
exit:
	spin_unlock(&ctrlr->cache_lock);
	return ret;
}

/**
 * rpmh_invalidate() - Invalidate sleep and wake sets in batch_cache
 *
 * @dev: The device making the request
 *
 * Invalidate the sleep and wake values in batch_cache.
 */
void rpmh_invalidate(const struct device *dev)
{
	struct rpmh_ctrlr *ctrlr = get_rpmh_ctrlr(dev);
	struct batch_cache_req *req, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&ctrlr->cache_lock, flags);
	list_for_each_entry_safe(req, tmp, &ctrlr->batch_cache, list)
		kfree(req);
	INIT_LIST_HEAD(&ctrlr->batch_cache);
	ctrlr->dirty = true;
	spin_unlock_irqrestore(&ctrlr->cache_lock, flags);
}
EXPORT_SYMBOL_GPL(rpmh_invalidate);
