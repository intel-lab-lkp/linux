// SPDX-License-Identifier: GPL-2.0+
/*
 * 9P transport over Thunderbolt/USB4 cable using USB4STREAM.
 *
 * Copyright (C) 2026, Intel Corporation
 * Author: Alan Borzeszkowski <alan.borzeszkowski@linux.intel.com>
 */

#include <linux/file.h>
#include <linux/fs_context.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <linux/sizes.h>
#include <linux/thunderbolt_stream.h>
#include <linux/wait.h>
#include <net/9p/9p.h>
#include <net/9p/client.h>
#include <net/9p/transport.h>

/**
 * struct p9_tb_conn - 9P Thunderbolt connection state
 * @client: 9P client instance.
 * @stream: USB4STREAM device.
 * @req_list_lock: Lock protecting active request list.
 * @write_lock: Makes sure only one write happens at a time.
 * @req_list: Sent request list.
 * @reader: Dedicated read thread.
 * @reader_wait: Wake @reader when request is queued.
 */
struct p9_tb_conn {
	struct p9_client *client;
	struct file *stream;
	struct mutex req_list_lock;
	struct mutex write_lock;
	struct list_head req_list;
	struct task_struct *reader;
	wait_queue_head_t reader_wait;
};

static void p9_tb_conn_cancel(struct p9_tb_conn *p9_tb, int err)
{
	struct p9_req_t *req, *temp;
	LIST_HEAD(cancel_list);

	scoped_guard(mutex, &p9_tb->req_list_lock) {
		list_for_each_entry_safe(req, temp, &p9_tb->req_list, req_list) {
			list_move(&req->req_list, &cancel_list);
			WRITE_ONCE(req->status, REQ_STATUS_ERROR);
		}
	}

	list_for_each_entry_safe(req, temp, &cancel_list, req_list) {
		list_del(&req->req_list);
		if (!req->t_err)
			req->t_err = err;
		p9_client_cb(p9_tb->client, req, REQ_STATUS_ERROR);
	}
}

static int p9_tb_read(struct p9_tb_conn *p9_tb, u8 *buf, size_t len)
{
	while (len) {
		int ret;

		ret = kernel_read(p9_tb->stream, buf, len, NULL);
		if (ret <= 0)
			return ret ?: -ESHUTDOWN;

		buf += ret;
		len -= ret;
	}

	return 0;
}

static int p9_tb_recv(struct p9_tb_conn *p9_tb)
{
	u8 header[P9_HDRSZ];
	/* rc.size needs to be set for header parse */
	struct p9_fcall rc = { .sdata = header, .size = P9_HDRSZ };
	struct p9_req_t *p9_req;
	int ret;

	ret = p9_tb_read(p9_tb, header, P9_HDRSZ);
	if (ret)
		return ret;

	ret = p9_parse_header(&rc, &rc.size, NULL, NULL, 0);
	if (ret) {
		p9_debug(P9_DEBUG_ERROR, "error parsing header: %d\n", ret);
		return ret;
	}

	p9_req = p9_tag_lookup(p9_tb->client, rc.tag);
	if (!p9_req) {
		p9_debug(P9_DEBUG_ERROR, "request with tag %d not found\n", rc.tag);
		return -EIO;
	}
	if (rc.size > p9_req->rc.capacity) {
		p9_debug(P9_DEBUG_ERROR,
			 "requested packet size too big: %d for tag %d with capacity %zd\n",
			 rc.size, rc.tag, p9_req->rc.capacity);
		ret = -E2BIG;
		goto out_put;
	}
	memcpy(p9_req->rc.sdata, header, P9_HDRSZ);
	ret = p9_tb_read(p9_tb, p9_req->rc.sdata + P9_HDRSZ, rc.size - P9_HDRSZ);
	if (ret)
		goto out_put;

	p9_req->rc.size = rc.size;
	p9_debug(P9_DEBUG_TRANS, "got packet tag %d size %d\n", rc.tag, rc.size);

	mutex_lock(&p9_tb->req_list_lock);
	if (p9_req->status == REQ_STATUS_SENT) {
		list_del(&p9_req->req_list);
		p9_client_cb(p9_tb->client, p9_req, REQ_STATUS_RCVD);
	} else if (p9_req->status != REQ_STATUS_FLSHD) {
		/* Invalid request - neither sent nor flushed */
		ret = -EIO;
	}
	/* Ignore response for cancelled (flushed) request */
	mutex_unlock(&p9_tb->req_list_lock);

out_put:
	p9_req_put(p9_tb->client, p9_req);
	return ret;
}

static int p9_tb_read_thread(void *data)
{
	struct p9_tb_conn *p9_tb = data;
	int ret;

	while (!kthread_should_stop()) {
		wait_event_interruptible(p9_tb->reader_wait,
					 !list_empty_careful(&p9_tb->req_list) ||
					 kthread_should_stop());
		if (kthread_should_stop())
			break;

		ret = p9_tb_recv(p9_tb);
		if (ret < 0) {
			/* Woken to stop, not real error */
			if (kthread_should_stop())
				break;

			/* Prevent subsequent writes */
			p9_tb->client->status = Disconnected;

			p9_tb_conn_cancel(p9_tb, ret);
		}
	}

	return 0;
}

static void p9_tb_close(struct p9_client *client)
{
	struct p9_tb_conn *p9_tb = client->trans;

	client->status = Disconnected;

	if (p9_tb->reader) {
		/* Break a blocked read */
		kthread_stop(p9_tb->reader);
	}

	p9_tb_conn_cancel(p9_tb, -ECONNRESET);
	filp_close(p9_tb->stream, NULL);
	kfree(p9_tb);
}

static int p9_tb_write(struct p9_tb_conn *p9_tb, struct p9_req_t *req)
{
	const char *p = req->tc.sdata;
	size_t nbytes = req->tc.size;
	int ret;

	scoped_guard(mutex, &p9_tb->req_list_lock) {
		WRITE_ONCE(req->status, REQ_STATUS_SENT);
		list_move_tail(&req->req_list, &p9_tb->req_list);
	}

	if (mutex_lock_interruptible(&p9_tb->write_lock)) {
		ret = -ERESTARTSYS;
		goto err;
	}

	do {
		ret = kernel_write(p9_tb->stream, p, nbytes, NULL);
		if (ret <= 0) {
			mutex_unlock(&p9_tb->write_lock);
			if (ret == 0)
				ret = -EIO;

			if (nbytes != req->tc.size) {
				p9_tb->client->status = Disconnected;
				p9_tb_conn_cancel(p9_tb, ret);
			}
			goto err;
		}
		nbytes -= ret;
		if (nbytes > 0)
			p += ret;
	} while (nbytes != 0);
	mutex_unlock(&p9_tb->write_lock);

	wake_up_interruptible(&p9_tb->reader_wait);

	return 0;

err:
	guard(mutex)(&p9_tb->req_list_lock);
	/*
	 * If request has status other than sent, we know p9_tb_conn_cancel()
	 * already handled it.
	 */
	if (READ_ONCE(req->status) == REQ_STATUS_SENT) {
		list_del(&req->req_list);
		return ret;
	}
	return 0;
}

static int p9_tb_request(struct p9_client *client, struct p9_req_t *req)
{
	struct p9_tb_conn *p9_tb = client->trans;

	if (client->status != Connected)
		return -EIO;

	return p9_tb_write(p9_tb, req);
}

static int p9_tb_create(struct p9_client *client, struct fs_context *fc)
{
	const char *devname = fc->source;
	struct p9_tb_conn *p9_tb;
	struct file *file;
	int ret;

	if (!devname)
		return -EINVAL;

	p9_tb = kzalloc_obj(*p9_tb);
	if (!p9_tb)
		return -ENOMEM;

	file = filp_open(devname, O_RDWR, 0);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto err_free;
	}

	if (!tbstream_dev_from_file(file)) {
		ret = -ENODEV;
		goto err_close;
	}

	p9_tb->stream = file;
	p9_tb->client = client;

	init_waitqueue_head(&p9_tb->reader_wait);
	INIT_LIST_HEAD(&p9_tb->req_list);
	mutex_init(&p9_tb->req_list_lock);
	mutex_init(&p9_tb->write_lock);

	p9_tb->reader = kthread_run(p9_tb_read_thread, p9_tb, "9p/%s",
				    kbasename(devname));
	if (IS_ERR(p9_tb->reader)) {
		ret = PTR_ERR(p9_tb->reader);
		goto err_close;
	}

	client->trans = p9_tb;
	client->status = Connected;

	return 0;

err_close:
	filp_close(file, NULL);
err_free:
	kfree(p9_tb);

	return ret;
}

static int p9_tb_cancel(struct p9_client *client, struct p9_req_t *req)
{
	/*
	 * Once p9_tb_request() returns, 9P request is queued on Thunderbolt
	 * TX ring and is marked as sent. Return '1' to let 9P client continue
	 * request flushing.
	 */
	return 1;
}

static int p9_tb_cancelled(struct p9_client *client, struct p9_req_t *req)
{
	struct p9_tb_conn *p9_tb = client->trans;

	p9_debug(P9_DEBUG_TRANS, "cancelling sent req %p, tag %d\n", req, req->tc.tag);

	/*
	 * If request got flushed and we didn't receive response by now, remove
	 * it from the list.
	 */
	guard(mutex)(&p9_tb->req_list_lock);
	if (READ_ONCE(req->status) != REQ_STATUS_SENT)
		return 0;

	list_del(&req->req_list);
	WRITE_ONCE(req->status, REQ_STATUS_FLSHD);
	p9_req_put(client, req);
	return 0;
}

static struct p9_trans_module p9_tb_trans = {
	.name = "thunderbolt",
	.owner = THIS_MODULE,
	.maxsize = SZ_1M,
	.supports_vmalloc = true,
	.create = p9_tb_create,
	.close = p9_tb_close,
	.request = p9_tb_request,
	.cancel = p9_tb_cancel,
	.cancelled = p9_tb_cancelled,
};

static int __init p9_trans_tb_init(void)
{
	v9fs_register_trans(&p9_tb_trans);
	return 0;
}
module_init(p9_trans_tb_init);

static void __exit p9_trans_tb_exit(void)
{
	v9fs_unregister_trans(&p9_tb_trans);
}
module_exit(p9_trans_tb_exit);

MODULE_ALIAS_9P("thunderbolt");
MODULE_AUTHOR("Alan Borzeszkowski <alan.borzeszkowski@linux.intel.com>");
MODULE_DESCRIPTION("9P transport over Thunderbolt/USB4 cable using USB4STREAM");
MODULE_LICENSE("GPL");
