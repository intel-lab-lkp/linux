// SPDX-License-Identifier: GPL-2.0-only
/*
 * IPMI driver for LSTP USB interface.
 *
 * Copyright (c) 2026, NVIDIA CORPORATION & AFFILIATES.  All rights reserved.
 */

#include <linux/build_bug.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/ipmi_ssif_bmc.h>
#include <linux/kfifo.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/property.h>
#include <linux/slab.h>

#include <linux/usb/lstp.h>

/* ~1 s of queued requests at OpenBMC rates; past that the host has retried. */
#define BUFFER_SIZE 4096

static DEFINE_IDA(lstp_ipmi_ida);

#define IPMI_LSTP_PAYLOAD_MAX 254
#define IPMI_MIN_MSG_LEN 2 /* netfn/lun + cmd */

/*
 * read(2) and write(2) layout with CONFIG_USB_LSTP_IPMI_MSG_NUM. Deployed
 * userspace parses these bytes, so the field widths and offsets are fixed;
 * native byte order, as both ends are the same host. @msg_num exists only on
 * this interface, never on the wire.
 */
struct ipmi_lstp_msg_header {
	__u32 len;
	__u8 msg_num;
} __packed;

struct ipmi_lstp_msg {
	struct ipmi_lstp_msg_header header;
	__u8 payload[IPMI_LSTP_PAYLOAD_MAX];
};

static_assert(sizeof(struct ipmi_lstp_msg_header) == 5, "IPMI read(2) header layout is ABI");
static_assert(offsetof(struct ipmi_lstp_msg_header, msg_num) == 4,
	      "IPMI read(2) header layout is ABI");

/*
 * The layout without CONFIG_USB_LSTP_IPMI_MSG_NUM is the ssif_bmc driver's:
 * the same len and payload, no @msg_num.
 */
static_assert(offsetof(struct ipmi_ssif_msg, payload) ==
		      sizeof_field(struct ipmi_lstp_msg_header, len),
	      "Untagged layout must be the LSTP header without msg_num");
static_assert(IPMI_SSIF_PAYLOAD_MAX == IPMI_LSTP_PAYLOAD_MAX,
	      "Untagged layout must carry the LSTP payload");

#define LSTP_IPMI_MSG_NUM IS_ENABLED(CONFIG_USB_LSTP_IPMI_MSG_NUM)

/* The leading bytes of struct ipmi_lstp_msg_header that userspace sees. */
#define LSTP_IPMI_UAPI_HDR_LEN                                     \
	(LSTP_IPMI_MSG_NUM ? sizeof(struct ipmi_lstp_msg_header) : \
			     sizeof_field(struct ipmi_lstp_msg_header, len))
#define LSTP_IPMI_UAPI_MSG_MAX \
	(LSTP_IPMI_MSG_NUM ? sizeof(struct ipmi_lstp_msg) : sizeof(struct ipmi_ssif_msg))

/* One per bind; the bind and each open fd hold @ref. */
struct lstp_ipmi_ctx {
	struct kref ref;
	struct lstp_channel *ch; /* pinned for the life of @ctx */
	struct miscdevice miscdev;
	int ida_id; /* -1 until allocated */
	bool running; /* exclusive-open flag */
	spinlock_t lock; /* protects @running, @msg_count, @read_tag, @fifo */
	struct mutex tx_lock; /* held across write()'s gone test and send; teardown waits on it */
	wait_queue_head_t req_wq;
	struct kfifo_rec_ptr_2 fifo; /* pending requests, oldest dropped to fit */
	u8 msg_count; /* tag of the outstanding request; write() echoes it */
	u8 read_tag; /* untagged ABI: tag of the request read() handed out last, 0 for none */
	bool stopped; /* node torn down; see lstp_ipmi_gone() */
};

enum lstp_ipmi_cmd {
	LSTP_IPMI_CMD_MESSAGE = 0x00,
};

/* @stopped is per bind and never cleared; see Documentation/driver-api/usb/lstp.rst. */
static inline bool lstp_ipmi_gone(struct lstp_ipmi_ctx *ctx)
{
	return READ_ONCE(ctx->stopped) || lstp_channel_disconnected(ctx->ch);
}

static inline bool lstp_ipmi_readable(struct lstp_ipmi_ctx *ctx)
{
	return !kfifo_is_empty(&ctx->fifo) || lstp_ipmi_gone(ctx);
}

/* Partial-init safe: a failed lstp_ipmi_init() unwinds through it as well. */
static void lstp_ipmi_ctx_release(struct kref *ref)
{
	struct lstp_ipmi_ctx *ctx = container_of(ref, struct lstp_ipmi_ctx, ref);
	struct lstp_channel *ch = ctx->ch;

	kfifo_free(&ctx->fifo);
	if (ctx->ida_id >= 0)
		ida_free(&lstp_ipmi_ida, ctx->ida_id);
	kfree(ctx->miscdev.name);
	mutex_destroy(&ctx->tx_lock);
	kfree(ctx);
	lstp_channel_put(ch);
}

/* Devres action for the bind's reference, booked ahead of the node's own. */
static void lstp_ipmi_ctx_put(void *data)
{
	struct lstp_ipmi_ctx *ctx = data;

	kref_put(&ctx->ref, lstp_ipmi_ctx_release);
}

static inline struct lstp_ipmi_ctx *to_ctx(struct file *file)
{
	return container_of(file->private_data, struct lstp_ipmi_ctx, miscdev);
}

static int lstp_ipmi_open(struct inode *inode, struct file *file)
{
	struct lstp_ipmi_ctx *ctx = to_ctx(file);
	int ret = 0;

	spin_lock_irq(&ctx->lock);
	if (lstp_ipmi_gone(ctx))
		ret = -ENODEV;
	else if (ctx->running)
		ret = -EBUSY;
	else
		ctx->running = true;
	spin_unlock_irq(&ctx->lock);

	if (ret)
		return ret;

	/*
	 * misc_mtx spans this ->open and misc_deregister(), which the bind's
	 * @ref outlasts, so @ctx is live here and this cannot race the final put.
	 */
	kref_get(&ctx->ref);
	return 0;
}

/* Read one queued request: the userspace header, then the payload. */
static ssize_t lstp_ipmi_read(struct file *file, char __user *buf, size_t count, loff_t *ppos)
{
	struct lstp_ipmi_ctx *ctx = to_ctx(file);
	struct ipmi_lstp_msg msg;
	unsigned long flags;
	unsigned int len = 0;
	size_t ulen = 0;

	/*
	 * copy_to_user() may sleep, so drain to @msg under @ctx->lock first.
	 * Loop: two readers on one fd can both wake, only one wins the record.
	 */
	while (!len) {
		/* Before the queue: a request queued when the bind went cannot be answered. */
		if (lstp_ipmi_gone(ctx))
			return -ENODEV;
		if (kfifo_is_empty(&ctx->fifo)) {
			if (file->f_flags & O_NONBLOCK)
				return -EAGAIN;
			int ret = wait_event_interruptible(ctx->req_wq, lstp_ipmi_readable(ctx));

			if (ret)
				return ret;
		}

		spin_lock_irqsave(&ctx->lock, flags);
		/*
		 * Peek before consuming: kfifo_out() drops whatever of the
		 * record does not fit, so a short @count would retire a
		 * request the caller never saw. Leave it queued and let the
		 * caller come back with room for it.
		 */
		len = kfifo_out_peek(&ctx->fifo, &msg, sizeof(msg));
		ulen = len ? LSTP_IPMI_UAPI_HDR_LEN + msg.header.len : 0;
		if (len && ulen <= count) {
			kfifo_skip(&ctx->fifo);
			WRITE_ONCE(ctx->read_tag, msg.header.msg_num);
		}
		spin_unlock_irqrestore(&ctx->lock, flags);

		if (ulen > count)
			return -EINVAL;
	}

	if (copy_to_user(buf, &msg.header, LSTP_IPMI_UAPI_HDR_LEN) ||
	    copy_to_user(buf + LSTP_IPMI_UAPI_HDR_LEN, msg.payload, msg.header.len))
		return -EFAULT;
	return ulen;
}

/*
 * Forward a response. A write whose @msg_num misses the outstanding tag is
 * dropped and reported as consumed, so userspace need not track the race
 * against a request that arrived meanwhile. Without the tag in the ABI, a
 * response answers the request read() handed out last.
 */
static ssize_t lstp_ipmi_write(struct file *file, const char __user *buf, size_t count,
			       loff_t *ppos)
{
	struct lstp_ipmi_ctx *ctx = to_ctx(file);
	struct lstp_channel *ch = ctx->ch;
	struct ipmi_lstp_msg msg;
	ssize_t ret;

	if (count < LSTP_IPMI_UAPI_HDR_LEN || count > LSTP_IPMI_UAPI_MSG_MAX)
		return -EINVAL;

	/* The untagged bound admits struct padding past the payload; drop it. */
	if (copy_from_user(&msg.header, buf, LSTP_IPMI_UAPI_HDR_LEN) ||
	    copy_from_user(msg.payload, buf + LSTP_IPMI_UAPI_HDR_LEN,
			   min(count - LSTP_IPMI_UAPI_HDR_LEN, sizeof(msg.payload))))
		return -EFAULT;

	/* Held through the send; see lstp_ipmi_stop_and_deregister(). */
	guard(mutex)(&ctx->tx_lock);
	if (lstp_ipmi_gone(ctx))
		return -ENODEV;

	if (!LSTP_IPMI_MSG_NUM)
		msg.header.msg_num = READ_ONCE(ctx->read_tag);

	/*
	 * 0 tags no request; it is only @msg_count's value before the first.
	 * Stale tag read is fine; pairs with WRITE_ONCE in the IRQ producer.
	 */
	if (!msg.header.msg_num || msg.header.msg_num != READ_ONCE(ctx->msg_count))
		return count;

	if (!msg.header.len || msg.header.len > IPMI_LSTP_PAYLOAD_MAX ||
	    count < LSTP_IPMI_UAPI_HDR_LEN + msg.header.len)
		return -EINVAL;

	ret = lstp_send(ch, LSTP_IPMI_CMD_MESSAGE, msg.payload, msg.header.len);
	if (ret && ret != -ENODEV)
		dev_err_ratelimited(ch->dev, "%s: ch_%d: Could not forward response (%pe)\n",
				    __func__, ch->ch_id, ERR_PTR(ret));

	return (ret < 0) ? ret : count;
}

static int lstp_ipmi_release(struct inode *inode, struct file *file)
{
	struct lstp_ipmi_ctx *ctx = to_ctx(file);

	spin_lock_irq(&ctx->lock);
	ctx->running = false;
	spin_unlock_irq(&ctx->lock);

	kref_put(&ctx->ref, lstp_ipmi_ctx_release);

	return 0;
}

static __poll_t lstp_ipmi_poll(struct file *file, poll_table *wait)
{
	struct lstp_ipmi_ctx *ctx = to_ctx(file);

	poll_wait(file, &ctx->req_wq, wait);
	if (lstp_ipmi_gone(ctx))
		return EPOLLHUP;
	if (!kfifo_is_empty(&ctx->fifo))
		return EPOLLIN | EPOLLRDNORM;

	return 0;
}

static const struct file_operations lstp_ipmi_fops = {
	.owner = THIS_MODULE,
	.open = lstp_ipmi_open,
	.read = lstp_ipmi_read,
	.write = lstp_ipmi_write,
	.release = lstp_ipmi_release,
	.poll = lstp_ipmi_poll,
};

/*
 * @stopped before the wake: this runs ahead of the framework's fence action, so
 * a reader woken without it would find nothing changed and sleep again. The
 * empty @tx_lock section then waits out a write() that tested @stopped before
 * it went up, so no response is sent once the unwind moves on; a send is
 * bounded by the USB timeout.
 */
static void lstp_ipmi_stop_and_deregister(void *data)
{
	struct lstp_ipmi_ctx *ctx = data;

	misc_deregister(&ctx->miscdev);
	WRITE_ONCE(ctx->stopped, true);
	wake_up_all(&ctx->req_wq);
	scoped_guard(mutex, &ctx->tx_lock) {
	}
}

/*
 * ops->irq_packet. Requests are never NACKed, so drop-oldest is the whole
 * flow-control policy.
 */
static void lstp_ipmi_irq_callback(struct lstp_channel *ch, struct lstp_packet *pkt)
{
	struct lstp_ipmi_ctx *ctx = ch->priv;
	u16 rx_len = le16_to_cpu(pkt->hdr.length);
	struct ipmi_lstp_msg msg;
	unsigned long flags;
	unsigned int dropped = 0;

	if (rx_len < IPMI_MIN_MSG_LEN || rx_len > IPMI_LSTP_PAYLOAD_MAX) {
		dev_warn_ratelimited(ch->dev, "%s: ch_%d: Invalid IPMI request length %u\n",
				     __func__, ch->ch_id, rx_len);
		return;
	}

	unsigned int msg_size = sizeof(msg.header) + rx_len;

	msg.header.len = rx_len;
	memcpy(msg.payload, pkt->payload, rx_len);

	spin_lock_irqsave(&ctx->lock, flags);
	/*
	 * Skip 0 on wrap: it is the initial value, meaning nothing outstanding,
	 * so no request may carry it. Pairs with the READ_ONCE in write().
	 */
	u8 tag = ctx->msg_count + 1;

	msg.header.msg_num = tag ?: 1;
	WRITE_ONCE(ctx->msg_count, msg.header.msg_num);
	while (kfifo_avail(&ctx->fifo) < msg_size) {
		kfifo_skip(&ctx->fifo);
		dropped++;
	}
	kfifo_in(&ctx->fifo, &msg, msg_size);
	spin_unlock_irqrestore(&ctx->lock, flags);

	if (dropped)
		dev_warn_ratelimited(ch->dev, "%s: ch_%d: FIFO full, dropped %u stale request(s)\n",
				     __func__, ch->ch_id, dropped);

	wake_up_all(&ctx->req_wq);
}

/*
 * Build this bind's context and its "ipmi-lstpN" node name. An fd held across
 * the unbind keeps the context, and with it the number, until it closes; the
 * next bind takes the lowest number free.
 */
static int lstp_ipmi_init(struct lstp_channel *ch, const struct lstp_channel_init_info *info)
{
	struct lstp_ipmi_ctx *ctx = kzalloc_obj(*ctx);

	if (!ctx)
		return -ENOMEM;

	kref_init(&ctx->ref);
	ctx->ida_id = -1;
	ctx->ch = lstp_channel_get(ch);
	spin_lock_init(&ctx->lock);
	mutex_init(&ctx->tx_lock);
	init_waitqueue_head(&ctx->req_wq);

	int ret = kfifo_alloc(&ctx->fifo, BUFFER_SIZE, GFP_KERNEL);

	if (ret)
		goto err_put;

	ret = ida_alloc(&lstp_ipmi_ida, GFP_KERNEL);
	if (ret < 0)
		goto err_put;
	ctx->ida_id = ret;
	ctx->miscdev.name = kasprintf(GFP_KERNEL, "ipmi-lstp%d", ctx->ida_id);
	if (!ctx->miscdev.name) {
		ret = -ENOMEM;
		goto err_put;
	}

	if (fwnode_property_present(ch->fwnode, "label"))
		dev_notice_once(ch->dev,
				"%s: ch_%d: Firmware \"label\" no longer names the node, which is %s; use a udev symlink for a stable name\n",
				__func__, ch->ch_id, ctx->miscdev.name);

	ctx->miscdev.minor = MISC_DYNAMIC_MINOR;
	ctx->miscdev.fops = &lstp_ipmi_fops;
	ctx->miscdev.parent = ch->dev;

	ch->priv = ctx;
	dev_dbg(ch->dev, "%s: ch_%d: Initialized as %s\n", __func__, ch->ch_id, ch->display_name);
	return 0;

err_put:
	kref_put(&ctx->ref, lstp_ipmi_ctx_release);
	return ret;
}

static int lstp_ipmi_start(struct lstp_channel *ch, struct lstp_ipmi_ctx *ctx)
{
	int ret = misc_register(&ctx->miscdev);

	if (ret) {
		dev_err(ch->dev, "%s: ch_%d: Could not register miscdevice (%pe)\n", __func__,
			ch->ch_id, ERR_PTR(ret));
		return ret;
	}

	ret = devm_add_action_or_reset(ch->dev, lstp_ipmi_stop_and_deregister, ctx);
	if (ret)
		return ret;

	dev_info(ch->dev, "%s: ch_%d: Started as %s\n", __func__, ch->ch_id, ch->display_name);
	return 0;
}

static int lstp_ipmi_probe(struct auxiliary_device *auxdev, const struct auxiliary_device_id *id)
{
	struct lstp_channel *ch = lstp_channel_from_auxdev(auxdev);
	const struct lstp_channel_init_info *info = dev_get_platdata(&auxdev->dev);

	if (!info)
		return -ENODEV;

	int ret = lstp_ipmi_init(ch, info);

	if (ret)
		return ret;

	/* Booked first, so it unwinds after the node's deregister action. */
	ret = devm_add_action_or_reset(ch->dev, lstp_ipmi_ctx_put, ch->priv);
	if (ret)
		return ret;

	return lstp_ipmi_start(ch, ch->priv);
}

static const struct lstp_ops lstp_ipmi_ops = {
	.irq_packet = lstp_ipmi_irq_callback,
};

static const struct auxiliary_device_id lstp_ipmi_id_table[] = { { .name = "lstp.ipmi" }, {} };
MODULE_DEVICE_TABLE(auxiliary, lstp_ipmi_id_table);

static struct lstp_auxiliary_driver lstp_ipmi_drv = {
	.adrv = {
		.name = "ipmi",
		.probe = lstp_ipmi_probe,
		.id_table = lstp_ipmi_id_table,
	},
	.ops = &lstp_ipmi_ops,
};
module_lstp_auxiliary_driver(lstp_ipmi_drv);

MODULE_IMPORT_NS("LSTP");
MODULE_DESCRIPTION("LSTP BMC-side IPMI driver");
MODULE_LICENSE("GPL");
