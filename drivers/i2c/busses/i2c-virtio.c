// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Virtio I2C Bus Driver
 *
 * The Virtio I2C Specification:
 * https://raw.githubusercontent.com/oasis-tcs/virtio-spec/master/virtio-i2c.tex
 *
 * Copyright (c) 2021 Intel Corporation. All rights reserved.
 */

#include <linux/acpi.h>
#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/overflow.h>
#include <linux/virtio.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>
#include <linux/virtio_i2c.h>

/**
 * struct virtio_i2c - virtio I2C data
 * @vdev: virtio device for this controller
 * @adap: I2C adapter for this controller
 * @vq: the virtio virtqueue for communication
 */
struct virtio_i2c {
	struct virtio_device *vdev;
	struct i2c_adapter adap;
	struct virtqueue *vq;
};

struct virtio_i2c_xfer;

/**
 * struct virtio_i2c_req - the virtio I2C request structure
 * @xfer: owning transfer
 * @completion: completion of virtio I2C message
 * @read: true if this is a read message (I2C_M_RD is set)
 * @out_hdr: the OUT header of the virtio I2C message
 * @in_hdr: the IN header of the virtio I2C message
 */
struct virtio_i2c_req {
	struct virtio_i2c_xfer *xfer;
	struct completion completion;
	bool read;
	struct virtio_i2c_out_hdr out_hdr	____cacheline_aligned;
	struct virtio_i2c_in_hdr in_hdr		____cacheline_aligned;
};

/**
 * struct virtio_i2c_xfer - a queued I2C transfer
 * @ref: one ref for the caller, plus one per in-flight virtqueue request
 * @bounce_buf_base: start of bounce buffer region
 * @reqs: the virtio I2C requests
 *
 * Allocation layout:
 * - struct virtio_i2c_xfer xfer
 * - struct virtio_i2c_req reqs[num]
 * - padding to dma_get_cache_alignment()
 * - u8 bounce_buf[virtio_i2c_bounce_size(msgs[0].len)]
 *   ...
 * - u8 bounce_buf[virtio_i2c_bounce_size(msgs[num-1].len)]
 */
struct virtio_i2c_xfer {
	struct kref ref;
	u8 *bounce_buf_base;
	struct virtio_i2c_req reqs[];
};

static size_t virtio_i2c_bounce_size(unsigned int len)
{
	return ALIGN(len, dma_get_cache_alignment());
}

static void virtio_i2c_xfer_release(struct kref *ref)
{
	struct virtio_i2c_xfer *xfer = container_of(ref, struct virtio_i2c_xfer, ref);
	kfree(xfer);
}

static void virtio_i2c_msg_done(struct virtqueue *vq)
{
	struct virtio_i2c_req *req;
	unsigned int len;

	while ((req = virtqueue_get_buf(vq, &len))) {
		complete(&req->completion);
		kref_put(&req->xfer->ref, virtio_i2c_xfer_release);
	}
}

static int virtio_i2c_prepare_xfer(struct virtqueue *vq,
				   struct virtio_i2c_xfer *xfer,
				   struct i2c_msg *msgs, int num)
{
	struct scatterlist *sgs[3], out_hdr, msg_buf, in_hdr;
	struct virtio_i2c_req *reqs = xfer->reqs;
	u8 *bounce_buf = xfer->bounce_buf_base;
	int i;

	for (i = 0; i < num; i++) {
		int outcnt = 0, incnt = 0;

		reqs[i].xfer = xfer;
		reqs[i].read = !!(msgs[i].flags & I2C_M_RD);
		init_completion(&reqs[i].completion);

		/*
		 * Only 7-bit mode supported for this moment. For the address
		 * format, Please check the Virtio I2C Specification.
		 */
		reqs[i].out_hdr.addr = cpu_to_le16(msgs[i].addr << 1);

		if (msgs[i].flags & I2C_M_RD)
			reqs[i].out_hdr.flags |= cpu_to_le32(VIRTIO_I2C_FLAGS_M_RD);

		if (i != num - 1)
			reqs[i].out_hdr.flags |= cpu_to_le32(VIRTIO_I2C_FLAGS_FAIL_NEXT);

		sg_init_one(&out_hdr, &reqs[i].out_hdr, sizeof(reqs[i].out_hdr));
		sgs[outcnt++] = &out_hdr;

		if (msgs[i].len) {
			/*
			 * Even if msg->flags has I2C_M_DMA_SAFE set, a bounce
			 * buffer is required because the transfer may be
			 * interrupted, after which msg->buf is no longer valid.
			 */
			if (!(msgs[i].flags & I2C_M_RD))
				memcpy(bounce_buf, msgs[i].buf, msgs[i].len);

			sg_init_one(&msg_buf, bounce_buf, msgs[i].len);

			if (msgs[i].flags & I2C_M_RD)
				sgs[outcnt + incnt++] = &msg_buf;
			else
				sgs[outcnt++] = &msg_buf;
		}
		bounce_buf += virtio_i2c_bounce_size(msgs[i].len);

		sg_init_one(&in_hdr, &reqs[i].in_hdr, sizeof(reqs[i].in_hdr));
		sgs[outcnt + incnt++] = &in_hdr;

		/* This reference is released in virtio_i2c_msg_done(). */
		kref_get(&xfer->ref);

		if (virtqueue_add_sgs(vq, sgs, outcnt, incnt, &reqs[i], GFP_KERNEL)) {
			kref_put(&xfer->ref, virtio_i2c_xfer_release);
			break;
		}
	}

	return i;
}

static int virtio_i2c_complete_xfer(struct virtio_i2c_xfer *xfer,
				    struct i2c_msg *msgs,
				    int num)
{
	struct virtio_i2c_req *reqs = xfer->reqs;
	u8 *bounce_buf = xfer->bounce_buf_base;
	bool failed = false;
	int i, j = 0;

	for (i = 0; i < num; i++) {
		struct virtio_i2c_req *req = &reqs[i];
		struct i2c_msg *msg = &msgs[i];

		if (wait_for_completion_interruptible(&req->completion))
			return -EINTR;

		if (req->in_hdr.status != VIRTIO_I2C_MSG_OK) {
			/*
			 * Don't break yet. Try to wait until all requests
			 * complete to ensure that the virtqueue has enough
			 * descriptor slots for the next transfer.
			 */
			failed = true;
		}

		if (!failed) {
			if (req->read)
				memcpy(msg->buf, bounce_buf, msg->len);
			j++;
		}

		bounce_buf += virtio_i2c_bounce_size(msg->len);
	}

	return j;
}

static int virtio_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
			   int num)
{
	struct virtio_i2c *vi = i2c_get_adapdata(adap);
	struct virtqueue *vq = vi->vq;
	struct virtio_i2c_xfer *xfer;
	size_t alloc_size;
	int i, count;

	alloc_size = struct_size(xfer, reqs, num);
	if (check_add_overflow(alloc_size,
			       dma_get_cache_alignment() - 1,
			       &alloc_size)) /* padding for PTR_ALIGN() */
		return -EOVERFLOW;
	for (i = 0; i < num; i++) {
		if (check_add_overflow(alloc_size,
				       virtio_i2c_bounce_size(msgs[i].len),
				       &alloc_size))
			return -EOVERFLOW;
	}

	xfer = kzalloc(alloc_size, GFP_KERNEL);
	if (!xfer)
		return -ENOMEM;

	kref_init(&xfer->ref);
	xfer->bounce_buf_base = PTR_ALIGN((u8 *)(xfer->reqs + num),
					  dma_get_cache_alignment());

	count = virtio_i2c_prepare_xfer(vq, xfer, msgs, num);
	if (!count)
		goto err_free;

	/*
	 * For the case where count < num, i.e. we weren't able to queue all the
	 * msgs, ideally we should abort right away and return early, but some
	 * of the messages are already sent to the remote I2C controller and the
	 * virtqueue will be left in undefined state in that case. We kick the
	 * remote here to clear the virtqueue, so we can try another set of
	 * messages later on.
	 */
	virtqueue_kick(vq);

	count = virtio_i2c_complete_xfer(xfer, msgs, count);

err_free:
	kref_put(&xfer->ref, virtio_i2c_xfer_release);
	return count;
}

static void virtio_i2c_del_vqs(struct virtio_device *vdev)
{
	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
}

static int virtio_i2c_setup_vqs(struct virtio_i2c *vi)
{
	struct virtio_device *vdev = vi->vdev;

	vi->vq = virtio_find_single_vq(vdev, virtio_i2c_msg_done, "msg");
	return PTR_ERR_OR_ZERO(vi->vq);
}

static u32 virtio_i2c_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm virtio_algorithm = {
	.xfer = virtio_i2c_xfer,
	.functionality = virtio_i2c_func,
};

static int virtio_i2c_probe(struct virtio_device *vdev)
{
	struct virtio_i2c *vi;
	int ret;

	if (!virtio_has_feature(vdev, VIRTIO_I2C_F_ZERO_LENGTH_REQUEST))
		return dev_err_probe(&vdev->dev, -EINVAL,
				     "Zero-length request feature is mandatory\n");

	vi = devm_kzalloc(&vdev->dev, sizeof(*vi), GFP_KERNEL);
	if (!vi)
		return -ENOMEM;

	vdev->priv = vi;
	vi->vdev = vdev;

	ret = virtio_i2c_setup_vqs(vi);
	if (ret)
		return ret;

	vi->adap.owner = THIS_MODULE;
	snprintf(vi->adap.name, sizeof(vi->adap.name),
		 "i2c_virtio at virtio bus %d", vdev->index);
	vi->adap.algo = &virtio_algorithm;
	vi->adap.dev.parent = &vdev->dev;
	vi->adap.dev.of_node = vdev->dev.of_node;
	i2c_set_adapdata(&vi->adap, vi);

	/*
	 * Setup ACPI node for controlled devices which will be probed through
	 * ACPI.
	 */
	ACPI_COMPANION_SET(&vi->adap.dev, ACPI_COMPANION(vdev->dev.parent));

	ret = i2c_add_adapter(&vi->adap);
	if (ret)
		virtio_i2c_del_vqs(vdev);

	return ret;
}

static void virtio_i2c_remove(struct virtio_device *vdev)
{
	struct virtio_i2c *vi = vdev->priv;

	i2c_del_adapter(&vi->adap);
	virtio_i2c_del_vqs(vdev);
}

static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_I2C_ADAPTER, VIRTIO_DEV_ANY_ID },
	{}
};
MODULE_DEVICE_TABLE(virtio, id_table);

static int virtio_i2c_freeze(struct virtio_device *vdev)
{
	virtio_i2c_del_vqs(vdev);
	return 0;
}

static int virtio_i2c_restore(struct virtio_device *vdev)
{
	return virtio_i2c_setup_vqs(vdev->priv);
}

static const unsigned int features[] = {
	VIRTIO_I2C_F_ZERO_LENGTH_REQUEST,
};

static struct virtio_driver virtio_i2c_driver = {
	.feature_table		= features,
	.feature_table_size	= ARRAY_SIZE(features),
	.id_table		= id_table,
	.probe			= virtio_i2c_probe,
	.remove			= virtio_i2c_remove,
	.driver			= {
		.name	= "i2c_virtio",
	},
	.freeze			= pm_sleep_ptr(virtio_i2c_freeze),
	.restore		= pm_sleep_ptr(virtio_i2c_restore),
};
module_virtio_driver(virtio_i2c_driver);

MODULE_AUTHOR("Jie Deng <jie.deng@intel.com>");
MODULE_AUTHOR("Conghui Chen <conghui.chen@intel.com>");
MODULE_DESCRIPTION("Virtio i2c bus driver");
MODULE_LICENSE("GPL");
