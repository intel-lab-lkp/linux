// SPDX-License-Identifier: GPL-2.0
/*
 * Amazon Nitro Secure Module driver.
 *
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * The Nitro Secure Module implements the following commands via CBOR over virtio:
 *
 *   DescribePCR(u16 index) -> { bool lock, u8[] data }
 *   ExtendPCR(u16 index, u8[] data) -> { u8[] data }
 *   LockPCR(u16 index) -> {}
 *   LockPCRs(u16 range_from_zero) -> {}
 *   DescribeNSM() -> { u16 version_major, u16 version_minor,
 *                      u16 version_patch, String module_id,
 *                      u16 max_pcrs, u16[] locked_pcrs, String digest }
 *   Attestation(u8[] user_data, u8[] nonce, u8[] public_key) -> { u8[] document }
 *   GetRandom() -> { u8[] random }
 *
 * This driver exposes them directly to user space as ioctls on /dev/nsm.
 */

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/hw_random.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/virtio.h>
#include <linux/wait.h>
#include <linux/cbor/decoder.h>
#include <linux/cbor/encoder.h>
#include <linux/cbor/parser.h>
#include <uapi/linux/nsm.h>

/* Timeout for NSM virtqueue respose in milliseconds. */
#define NSM_DEFAULT_TIMEOUT_MSECS (120000) /* 2 minutes */

struct nsm {
	struct list_head      node;
	struct virtio_device *vdev;
	struct virtqueue     *vq;
	struct mutex          lock;
	wait_queue_head_t     wq;
	bool                  device_notified;
	struct miscdevice     misc;
	struct hwrng          hwrng;
	struct work_struct    misc_init;
	u16                   major;
	u16                   minor;
	u16                   patch;
	u16                   max_pcrs;
	char                  module_id[256];
	char                  digest[16];
};

/* NSM device ID */
static const struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_NITRO_SEC_MOD, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

/* Full NSM request/response message */
struct nsm_msg {
	struct nsm_data_req req;
	struct nsm_data_resp resp;
};

static inline struct nsm *file_to_nsm(struct file *file)
{
	return container_of(file->private_data, struct nsm, misc);
}

static inline struct nsm *hwrng_to_nsm(struct hwrng *rng)
{
	return container_of(rng, struct nsm, hwrng);
}

static inline struct nsm *misc_dev_to_nsm(struct miscdevice *misc)
{
	return container_of(misc, struct nsm, misc);
}

static bool cbor_is_str(cbor_reader_t *reader, cbor_item_t *item,
			const char *str)
{
	int len = strlen(str);

	if (item->size != len)
		return false;
	return !memcmp(&reader->msg[item->offset], str, len);
}

/* Copy the request of a raw message to kernel space */
static int fill_req_raw(struct nsm *nsm, struct nsm_data_req *req,
			struct nsm_raw *raw)
{
	/* Only root has access to raw requests */
	if (!capable(CAP_SYS_ADMIN))
		return -EACCES;

	/* Verify the user input size. */
	if (raw->request.len > sizeof(req->data))
		return -EMSGSIZE;

	/* Copy the request payload */
	if (copy_from_user(req->data, u64_to_user_ptr(raw->request.addr),
			   raw->request.len))
		return -EFAULT;

	req->len = raw->request.len;

	return 0;
}

static int _fill_req_raw(struct nsm *nsm, struct nsm_data_req *req,
			 void *buffer)
{
	return fill_req_raw(nsm, req, buffer);
}

/* Copy the response of a raw message back to user-space */
static int parse_resp_raw(struct nsm *nsm, struct nsm_data_resp *resp,
			  struct nsm_raw *raw)
{
	/* Truncate any message that does not fit. */
	raw->response.len = min_t(u64, raw->response.len, resp->len);

	/* Copy the response content to user space */
	if (copy_to_user(u64_to_user_ptr(raw->response.addr),
			 resp->data, raw->response.len))
		return -EFAULT;

	return 0;
}

static int _parse_resp_raw(struct nsm *nsm, struct nsm_data_resp *resp,
			   void *buffer)
{
	return parse_resp_raw(nsm, resp, buffer);
}

/* Virtqueue interrupt handler */
static void nsm_vq_callback(struct virtqueue *vq)
{
	struct nsm *nsm = vq->vdev->priv;

	nsm->device_notified = true;
	wake_up(&nsm->wq);
}

/* Forward a message to the NSM device and wait for the response from it */
static int nsm_sendrecv_msg(struct nsm *nsm, struct nsm_msg *msg)
{
	struct device *dev = &nsm->vdev->dev;
	struct scatterlist sg_in, sg_out;
	struct virtqueue *vq = nsm->vq;
	unsigned int len;
	void *queue_buf;
	bool kicked;
	int rc;

	/* Initialize scatter-gather lists with request and response buffers. */
	sg_init_one(&sg_out, msg->req.data, msg->req.len);
	sg_init_one(&sg_in, msg->resp.data, sizeof(msg->resp.data));

	mutex_lock(&nsm->lock);

	/* Add the request buffer (read by the device). */
	rc = virtqueue_add_outbuf(vq, &sg_out, 1, msg->req.data, GFP_KERNEL);
	if (rc) {
		mutex_unlock(&nsm->lock);
		return rc;
	}

	/* Add the response buffer (written by the device). */
	rc = virtqueue_add_inbuf(vq, &sg_in, 1, msg->resp.data, GFP_KERNEL);
	if (rc)
		goto cleanup;

	nsm->device_notified = false;
	kicked = virtqueue_kick(vq);
	if (!kicked) {
		/* Cannot kick the virtqueue. */
		rc = -EIO;
		goto cleanup;
	}

	/* If the kick succeeded, wait for the device's response. */
	rc = wait_event_timeout(nsm->wq,
		nsm->device_notified == true,
		msecs_to_jiffies(NSM_DEFAULT_TIMEOUT_MSECS));
	if (!rc) {
		rc = -ETIMEDOUT;
		goto cleanup;
	}

	queue_buf = virtqueue_get_buf(vq, &len);
	if (!queue_buf || (queue_buf != msg->req.data)) {
		dev_err(dev, "wrong request buffer.");
		rc = -ENODATA;
		goto cleanup;
	}

	queue_buf = virtqueue_get_buf(vq, &len);
	if (!queue_buf || (queue_buf != msg->resp.data)) {
		dev_err(dev, "wrong response buffer.");
		rc = -ENODATA;
		goto cleanup;
	}

	msg->resp.len = len;

	rc = 0;

cleanup:
	if (rc) {
		/* Clean the virtqueue. */
		while (virtqueue_get_buf(vq, &len) != NULL)
			;
	}

	mutex_unlock(&nsm->lock);
	return rc;
}

/*
 * DescribePCR request structure:
 *
 * {
 *   Map(1) {
 *     key = String("DescribePCR"),
 *     value = Map(1) {
 *       key = String("index"),
 *       value = Int(pcr)
 *     }
 *   }
 * }
 */
static int fill_req_describe_pcr(struct nsm *nsm, struct nsm_data_req *req,
				 union nsm_describe_pcr *desc_union)
{
	struct nsm_describe_pcr_req *desc = &desc_union->req;
	cbor_writer_t writer;
	cbor_error_t err;

	cbor_writer_init(&writer, req->data, sizeof(req->data));
	err = cbor_encode_map(&writer, 1);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_null_terminated_text_string(&writer, "DescribePCR");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_map(&writer, 1);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_null_terminated_text_string(&writer, "index");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_unsigned_integer(&writer, desc->index);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	req->len = cbor_writer_len(&writer);

	return 0;
}

static int _fill_req_describe_pcr(struct nsm *nsm, struct nsm_data_req *req,
				  void *buffer)
{
	return fill_req_describe_pcr(nsm, req, buffer);
}

/*
 * DescribePCR response structure:
 *
 * {
 *   Map(1) {
 *     key = String("DescribePCR"),
 *     value = Map(2) {
 *       key = String("data"),
 *       value = Array(),
 *       key = String("lock"),
 *       value = Bool()
 *     }
 *   }
 * }
 */
static int parse_resp_describe_pcr(struct nsm *nsm, struct nsm_data_resp *resp,
				   union nsm_describe_pcr *desc_union)
{
	struct nsm_describe_pcr_resp *desc = &desc_union->resp;
	struct device *dev = &nsm->vdev->dev;
	cbor_item_t items[7];
	cbor_reader_t reader;
	cbor_error_t err;
	size_t n;

	cbor_reader_init(&reader, items, ARRAY_SIZE(items));
	err = cbor_parse(&reader, resp->data, resp->len, &n);
	if (err != CBOR_SUCCESS) {
		dev_dbg(dev, "Invalid DescribePCR response");
		return -EINVAL;
	}

	if (n != 7) {
		dev_dbg(dev, "Invalid DescribePCR response elements (%zd)", n);
		return -EINVAL;
	}

	if (!cbor_is_str(&reader, &items[1], "DescribePCR")) {
		dev_dbg(dev, "Invalid DescribePCR response items[1]");
		return -EINVAL;
	}

	if (!cbor_is_str(&reader, &items[3], "data")) {
		dev_dbg(dev, "Invalid DescribePCR response items[3]");
		return -EINVAL;
	}

	if (items[4].type != CBOR_ITEM_STRING) {
		dev_dbg(dev, "Invalid DescribePCR response items[4]");
		return -EINVAL;
	}

	desc->data.len = items[4].size;
	memcpy(desc->data.data, &resp->data[items[4].offset],
	       desc->data.len);

	if (!cbor_is_str(&reader, &items[5], "lock")) {
		dev_dbg(dev, "Invalid DescribePCR response items[5]");
		return -EINVAL;
	}

	if (items[6].type != CBOR_ITEM_SIMPLE_VALUE) {
		dev_dbg(dev, "Invalid DescribePCR response items[6]");
		return -EINVAL;
	}

	desc->lock = 0;
	err = cbor_decode(&reader, &items[6], &desc->lock, sizeof(u16));
	if (err != CBOR_SUCCESS) {
		dev_dbg(dev, "Invalid DescribePCR respons items[6]");
		return -EINVAL;
	}

	return 0;
}

static int _parse_resp_describe_pcr(struct nsm *nsm, struct nsm_data_resp *resp,
				    void *buffer)
{
	return parse_resp_describe_pcr(nsm, resp, buffer);
}

/*
 * ExtendPCR request structure:
 *
 * {
 *   Map(1) {
 *     key = String("ExtendPCR"),
 *     value = Map(2) {
 *       key = String("index"),
 *       value = Int(pcr),
 *       key = String("data"),
 *       value = Array(data),
 *     }
 *   }
 * }
 */
static int fill_req_extend_pcr(struct nsm *nsm, struct nsm_data_req *req,
			       union nsm_extend_pcr *extend_union)
{
	struct nsm_extend_pcr_req *extend = &extend_union->req;
	cbor_writer_t writer;
	cbor_error_t err;

	cbor_writer_init(&writer, req->data, sizeof(req->data));
	err = cbor_encode_map(&writer, 1);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_null_terminated_text_string(&writer, "ExtendPCR");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_map(&writer, 2);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_null_terminated_text_string(&writer, "index");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_unsigned_integer(&writer, extend->index);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_null_terminated_text_string(&writer, "data");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	if (extend->data.len > sizeof(extend->data.data))
		return -EINVAL;

	err = cbor_encode_byte_string(&writer, extend->data.data,
				      extend->data.len);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	req->len = cbor_writer_len(&writer);

	return 0;
}

static int _fill_req_extend_pcr(struct nsm *nsm, struct nsm_data_req *req,
				void *buffer)
{
	return fill_req_extend_pcr(nsm, req, buffer);
}

/*
 * ExtendPCR response structure:
 *
 * {
 *   Map(1) {
 *     key = String("ExtendPCR"),
 *     value = Map(1) {
 *       key = String("data"),
 *       value = Array()
 *     }
 *   }
 * }
 */
static int parse_resp_extend_pcr(struct nsm *nsm, struct nsm_data_resp *resp,
				 union nsm_extend_pcr *extend_union)
{
	struct nsm_extend_pcr_resp *extend = &extend_union->resp;
	struct device *dev = &nsm->vdev->dev;
	cbor_item_t items[7];
	cbor_reader_t reader;
	cbor_error_t err;
	size_t n;

	cbor_reader_init(&reader, items, ARRAY_SIZE(items));
	err = cbor_parse(&reader, resp->data, resp->len, &n);
	if (err != CBOR_SUCCESS) {
		dev_dbg(dev, "Invalid ExtendPCR response");
		return -EINVAL;
	}

	if (n != 5) {
		dev_dbg(dev, "Invalid ExtendPCR response elements (%zd)", n);
		return -EINVAL;
	}

	if (!cbor_is_str(&reader, &items[1], "ExtendPCR")) {
		dev_dbg(dev, "Invalid ExtendPCR response items[1]");
		return -EINVAL;
	}

	if (!cbor_is_str(&reader, &items[3], "data")) {
		dev_dbg(dev, "Invalid ExtendPCR response items[3]");
		return -EINVAL;
	}

	if (items[4].type != CBOR_ITEM_STRING) {
		dev_dbg(dev, "Invalid ExtendPCR response items[4]");
		return -EINVAL;
	}

	extend->data.len = items[4].size;
	memcpy(extend->data.data, &resp->data[items[4].offset],
	       extend->data.len);

	return 0;
}

static int _parse_resp_extend_pcr(struct nsm *nsm, struct nsm_data_resp *resp,
				  void *buffer)
{
	return parse_resp_extend_pcr(nsm, resp, buffer);
}

/*
 * LockPCR request structure:
 *
 * {
 *   Map(1) {
 *     key = String("LockPCR"),
 *     value = Map(1) {
 *       key = String("index"),
 *       value = Int(pcr)
 *     }
 *   }
 * }
 */
static int fill_req_lock_pcr(struct nsm *nsm, struct nsm_data_req *req,
			     union nsm_lock_pcr *lock_union)
{
	struct nsm_lock_pcr_req *lock = &lock_union->req;
	cbor_writer_t writer;
	cbor_error_t err;

	cbor_writer_init(&writer, req->data, sizeof(req->data));
	err = cbor_encode_map(&writer, 1);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_null_terminated_text_string(&writer, "LockPCR");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_map(&writer, 1);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_null_terminated_text_string(&writer, "index");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_unsigned_integer(&writer, lock->index);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	req->len = cbor_writer_len(&writer);

	return 0;
}

static int _fill_req_lock_pcr(struct nsm *nsm, struct nsm_data_req *req,
			      void *buffer)
{
	return fill_req_lock_pcr(nsm, req, buffer);
}

/*
 * LockPCR error response structure:
 *
 * {
 *   Map(1) {
 *     key = String("Error"),
 *     value = String("ReadOnlyIndex"),
 *   }
 * }
 *
 * Any other response means success.
 */
static int parse_resp_lock_pcr(struct nsm *nsm, struct nsm_data_resp *resp)
{
	struct device *dev = &nsm->vdev->dev;
	cbor_item_t items[7];
	cbor_reader_t reader;
	cbor_error_t err;
	size_t n;

	cbor_reader_init(&reader, items, ARRAY_SIZE(items));
	err = cbor_parse(&reader, resp->data, resp->len, &n);
	if (err != CBOR_SUCCESS) {
		dev_dbg(dev, "Invalid LockPCR response");
		return -EINVAL;
	}

	if (n == 3 &&
	    cbor_is_str(&reader, &items[1], "Error"))
		return -EINVAL;

	return 0;
}

static int _parse_resp_lock_pcr(struct nsm *nsm, struct nsm_data_resp *resp,
				void *buffer)
{
	return parse_resp_lock_pcr(nsm, resp);
}

/*
 * LockPCRs request structure:
 *
 * {
 *   Map(1) {
 *     key = String("LockPCRs"),
 *     value = Map(1) {
 *       key = String("range"),
 *       value = Int(pcr)
 *     }
 *   }
 * }
 */
static int fill_req_lock_pcrs(struct nsm *nsm, struct nsm_data_req *req,
			      union nsm_lock_pcrs *lock_union)
{
	struct nsm_lock_pcrs_req *lock = &lock_union->req;
	cbor_writer_t writer;
	cbor_error_t err;

	cbor_writer_init(&writer, req->data, sizeof(req->data));
	err = cbor_encode_map(&writer, 1);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_null_terminated_text_string(&writer, "LockPCRs");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_map(&writer, 1);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_null_terminated_text_string(&writer, "range");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_unsigned_integer(&writer, lock->range_from_zero);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	req->len = cbor_writer_len(&writer);

	return 0;
}

static int _fill_req_lock_pcrs(struct nsm *nsm, struct nsm_data_req *req,
			       void *buffer)
{
	return fill_req_lock_pcrs(nsm, req, buffer);
}

/*
 * LockPCRs error response structure:
 *
 * {
 *   Map(1) {
 *     key = String("Error"),
 *     value = String("ReadOnlyIndex"),
 *   }
 * }
 *
 * Any other response means success.
 */
static int parse_resp_lock_pcrs(struct nsm *nsm, struct nsm_data_resp *resp)
{
	struct device *dev = &nsm->vdev->dev;
	cbor_item_t items[7];
	cbor_reader_t reader;
	cbor_error_t err;
	size_t n;

	cbor_reader_init(&reader, items, ARRAY_SIZE(items));
	err = cbor_parse(&reader, resp->data, resp->len, &n);
	if (err != CBOR_SUCCESS) {
		dev_dbg(dev, "Invalid LockPCRs response");
		return -EINVAL;
	}

	if (n == 3 &&
	    cbor_is_str(&reader, &items[1], "Error"))
		return -EINVAL;

	return 0;
}

static int _parse_resp_lock_pcrs(struct nsm *nsm, struct nsm_data_resp *resp,
				 void *buffer)
{
	return parse_resp_lock_pcrs(nsm, resp);
}

/*
 * DescribeNSM request structure:
 *
 * {
 *   String("DescribeNSM"),
 * }
 */
static int fill_req_describe_nsm(struct nsm *nsm, struct nsm_data_req *req)
{
	cbor_writer_t writer;
	cbor_error_t err;

	cbor_writer_init(&writer, req->data, sizeof(req->data));

	err = cbor_encode_null_terminated_text_string(&writer, "DescribeNSM");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	req->len = cbor_writer_len(&writer);

	return 0;
}

static int _fill_req_describe_nsm(struct nsm *nsm, struct nsm_data_req *req,
				  void *buffer)
{
	return fill_req_describe_nsm(nsm, req);
}

/*
 * DescribeNSM response structure:
 *
 * {
 *   Map(1) {
 *     key = String("DescribeNSM"),
 *     value = Map(7) {
 *       key = String("digest"),
 *       value = String("SHA384"),
 *       key = String("max_pcrs"),
 *       value = Int(32),
 *       key = String("module_id"),
 *       value = String("i-1234-enc5678"),
 *       key = String("locked_pcrs"),
 *       value = Array<u16>() (libcbor lists every element as item)
 *       key = String("version_major"),
 *       value = Int(1),
 *       key = String("version_minor"),
 *       value = Int(0),
 *       key = String("version_patch"),
 *       value = Int(0)
 *     }
 *   }
 * }
 */
static int parse_resp_describe_nsm(struct nsm *nsm, struct nsm_data_resp *resp,
				   union nsm_describe_nsm *desc_union)
{
	struct nsm_describe_nsm_resp *desc = &desc_union->resp;
	struct device *dev = &nsm->vdev->dev;
	cbor_item_t items[64];
	cbor_reader_t reader;
	cbor_error_t err;
	size_t n;
	int i;

	cbor_reader_init(&reader, items, ARRAY_SIZE(items));
	err = cbor_parse(&reader, resp->data, resp->len, &n);
	if (err != CBOR_SUCCESS) {
		dev_dbg(dev, "Invalid DescribeNSM response");
		return -EINVAL;
	}

	if (n < 11) {
		dev_dbg(dev, "Invalid DescribeNSM response elements (%zd)", n);
		return -EINVAL;
	}

	if (!cbor_is_str(&reader, &items[1], "DescribeNSM")) {
		dev_dbg(dev, "Invalid DescribeNSM response items[1]");
		return -EINVAL;
	}

	if (!cbor_is_str(&reader, &items[3], "digest")) {
		dev_dbg(dev, "Invalid DescribeNSM response items[3]");
		return -EINVAL;
	}

	if (items[4].type != CBOR_ITEM_STRING) {
		dev_dbg(dev, "Invalid DescribeNSM response items[4]");
		return -EINVAL;
	}

	memset(desc->digest, 0, sizeof(desc->digest));
	memcpy(desc->digest, &resp->data[items[4].offset],
	       min_t(size_t, items[4].size, sizeof(desc->digest)));

	if (!cbor_is_str(&reader, &items[5], "max_pcrs")) {
		dev_dbg(dev, "Invalid DescribeNSM response items[5]");
		return -EINVAL;
	}

	desc->max_pcrs = 0;
	err = cbor_decode(&reader, &items[6], &desc->max_pcrs,
			  sizeof(desc->max_pcrs));
	if (err != CBOR_SUCCESS) {
		dev_dbg(dev, "Invalid DescribeNSM respons items[6]");
		return -EINVAL;
	}

	if (!cbor_is_str(&reader, &items[7], "module_id")) {
		dev_dbg(dev, "Invalid DescribeNSM response items[7]");
		return -EINVAL;
	}

	memset(desc->module_id, 0, sizeof(desc->module_id));
	memcpy(desc->module_id, &resp->data[items[8].offset],
	       min_t(size_t, items[8].size, sizeof(desc->module_id)));

	if (!cbor_is_str(&reader, &items[9], "locked_pcrs")) {
		dev_dbg(dev, "Invalid DescribeNSM response items[9]");
		return -EINVAL;
	}

	desc->locked_pcrs.u16s = min_t(size_t, ARRAY_SIZE(desc->locked_pcrs.u16),
				       items[10].size);
	memcpy(desc->locked_pcrs.u16, &resp->data[items[10].offset],
	       desc->locked_pcrs.u16s * sizeof(u16));

	/* libcbor interprets each array element as item, use dynamic index */
	i = 11 + desc->locked_pcrs.u16s;

	if (i > n)
		return -EINVAL;

	if (!cbor_is_str(&reader, &items[i], "version_major")) {
		dev_dbg(dev, "Invalid DescribeNSM response items[%d]", i);
		return -EINVAL;
	}

	if (++i > n)
		return -EINVAL;

	desc->major = 0;
	err = cbor_decode(&reader, &items[i], &desc->major,
			  sizeof(desc->major));

	if (++i > n)
		return -EINVAL;

	if (!cbor_is_str(&reader, &items[i], "version_minor")) {
		dev_dbg(dev, "Invalid DescribeNSM response items[%d]", i);
		return -EINVAL;
	}

	if (++i > n)
		return -EINVAL;

	desc->minor = 0;
	err = cbor_decode(&reader, &items[i], &desc->minor,
			  sizeof(desc->minor));

	if (++i > n)
		return -EINVAL;

	if (!cbor_is_str(&reader, &items[i], "version_patch")) {
		dev_dbg(dev, "Invalid DescribeNSM response items[%d]", i);
		return -EINVAL;
	}

	if (++i > n)
		return -EINVAL;

	desc->patch = 0;
	err = cbor_decode(&reader, &items[i], &desc->patch,
			  sizeof(desc->patch));

	return 0;
}

static int _parse_resp_describe_nsm(struct nsm *nsm, struct nsm_data_resp *resp,
				    void *buffer)
{
	return parse_resp_describe_nsm(nsm, resp, buffer);
}

/*
 * Attestation request structure:
 *
 *   Map(1) {
 *     key = String("Attestation"),
 *     value = Map(3) {
 *       key = String("user_data"),
 *       value = Array(),
 *       key = String("nonce"),
 *       value = Array(),
 *       key = String("public_key"),
 *       value = Array(),
 *     }
 *   }
 * }
 */
static int fill_req_attestation(struct nsm *nsm, struct nsm_data_req *req,
				union nsm_attestation *att_union)
{
	struct nsm_attestation_req *att = &att_union->req;
	cbor_writer_t writer;
	cbor_error_t err;

	cbor_writer_init(&writer, req->data, sizeof(req->data));

	err = cbor_encode_map(&writer, 1);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_null_terminated_text_string(&writer, "Attestation");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_map(&writer, 3);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_null_terminated_text_string(&writer, "user_data");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	att->user_data.len = min_t(u32, att->user_data.len,
				   sizeof(att->user_data.data));
	err = cbor_encode_byte_string(&writer, att->user_data.data,
				      att->user_data.len);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_null_terminated_text_string(&writer, "nonce");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	att->nonce.len = min_t(u32, att->nonce.len, sizeof(att->nonce.data));
	err = cbor_encode_byte_string(&writer, att->nonce.data,
				      att->nonce.len);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	err = cbor_encode_null_terminated_text_string(&writer, "public_key");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	att->public_key.len = min_t(u32, att->public_key.len,
				    sizeof(att->public_key.data));
	err = cbor_encode_byte_string(&writer, att->public_key.data,
				      att->public_key.len);
	if (err != CBOR_SUCCESS)
		return -ENOMEM;

	req->len = cbor_writer_len(&writer);

	return 0;
}

static int _fill_req_attestation(struct nsm *nsm, struct nsm_data_req *req,
				 void *buffer)
{
	return fill_req_attestation(nsm, req, buffer);
}

/*
 * Attestation response structure:
 *
 * {
 *   Map(1) {
 *     key = String("Attestation"),
 *     value = Map(1) {
 *       key = String("document"),
 *       value = Array()
 *     }
 *   }
 * }
 */
static int parse_resp_attestation(struct nsm *nsm, struct nsm_data_resp *resp,
				  union nsm_attestation *att_union)
{
	struct nsm_attestation_resp *att = &att_union->resp;
	struct device *dev = &nsm->vdev->dev;
	cbor_item_t items[6];
	cbor_reader_t reader;
	cbor_error_t err;
	size_t n;

	cbor_reader_init(&reader, items, ARRAY_SIZE(items));
	err = cbor_parse(&reader, resp->data, resp->len, &n);
	if (err != CBOR_SUCCESS) {
		dev_dbg(dev, "Invalid Attestation response");
		return -EINVAL;
	}

	if (n != 5) {
		dev_dbg(dev, "Invalid Attestation response elements (%zd)", n);
		return -EINVAL;
	}

	if (!cbor_is_str(&reader, &items[1], "Attestation")) {
		dev_dbg(dev, "Invalid Attestation response items[1]");
		return -EINVAL;
	}

	if (!cbor_is_str(&reader, &items[3], "document")) {
		dev_dbg(dev, "Invalid Attestation response items[3]");
		return -EINVAL;
	}

	if (items[4].type != CBOR_ITEM_STRING) {
		dev_dbg(dev, "Invalid Attestation response items[4]");
		return -EINVAL;
	}

	att->document.len = items[4].size;
	memcpy(att->document.data, &resp->data[items[4].offset],
	       att->document.len);

	return 0;
}

static int _parse_resp_attestation(struct nsm *nsm, struct nsm_data_resp *resp,
				   void *buffer)
{
	return parse_resp_attestation(nsm, resp, buffer);
}

/*
 * GetRandom request structure:
 *
 * {
 *   String("GetRandom")
 * }
 */
static int fill_req_get_random(struct nsm *nsm, struct nsm_data_req *req)
{
	cbor_writer_t writer;
	cbor_error_t err;

	cbor_writer_init(&writer, req->data, sizeof(req->data));
	err = cbor_encode_null_terminated_text_string(&writer, "GetRandom");
	if (err != CBOR_SUCCESS)
		return -ENOMEM;
	req->len = cbor_writer_len(&writer);

	return 0;
}

static int _fill_req_get_random(struct nsm *nsm, struct nsm_data_req *req,
				void *buffer)
{
	return fill_req_get_random(nsm, req);
}

/*
 * GetRandom response structure:
 *
 * {
 *   Map(1) {
 *     key = String("GetRandom"),
 *     value = Map(1) {
 *       key = String("random"),
 *       value = Array()
 *     }
 *   }
 * }
 */
static int parse_resp_get_random(struct nsm *nsm, struct nsm_data_resp *resp,
				 union nsm_get_random *random_union)
{
	struct nsm_get_random_resp *random = &random_union->resp;
	struct device *dev = &nsm->vdev->dev;
	cbor_item_t items[6];
	cbor_reader_t reader;
	cbor_error_t err;
	size_t n;

	cbor_reader_init(&reader, items, ARRAY_SIZE(items));
	err = cbor_parse(&reader, resp->data, resp->len, &n);
	if (err != CBOR_SUCCESS) {
		dev_dbg(dev, "Invalid GetRandom response");
		return -EINVAL;
	}

	if (n != 5) {
		dev_dbg(dev, "Invalid GetRandom response elements (%zd)", n);
		return -EINVAL;
	}

	if (!cbor_is_str(&reader, &items[1], "GetRandom")) {
		dev_dbg(dev, "Invalid GetRandom response items[1]");
		return -EINVAL;
	}

	if (!cbor_is_str(&reader, &items[3], "random")) {
		dev_dbg(dev, "Invalid GetRandom response items[3]");
		return -EINVAL;
	}

	if (items[4].type != CBOR_ITEM_STRING) {
		dev_dbg(dev, "Invalid GetRandom response items[4]");
		return -EINVAL;
	}

	random->random.len = items[4].size;
	memcpy(random->random.data, &resp->data[items[4].offset],
	       random->random.len);

	return 0;
}

static int _parse_resp_get_random(struct nsm *nsm, struct nsm_data_resp *resp,
				  void *buffer)
{
	return parse_resp_get_random(nsm, resp, buffer);
}

/*
 * HwRNG implementation
 */
static int nsm_rng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	struct nsm *nsm = hwrng_to_nsm(rng);
	struct device *dev = &nsm->vdev->dev;
	union nsm_get_random *random = NULL;
	struct nsm_msg *msg;
	int rc = 0;

	/* NSM always needs to wait for a response */
	if (!wait)
		return 0;

	msg = devm_kzalloc(dev, sizeof(*msg), GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	rc = -ENOMEM;
	random = devm_kzalloc(dev, sizeof(*random), GFP_KERNEL);
	if (!random)
		goto out;

	rc = fill_req_get_random(nsm, &msg->req);
	if (rc != 0)
		goto out;

	rc = nsm_sendrecv_msg(nsm, msg);
	if (rc != 0)
		goto out;

	rc = parse_resp_get_random(nsm, &msg->resp, random);
	if (rc != 0)
		goto out;

	rc = min_t(size_t, random->resp.random.len, max);
	memcpy(data, random->resp.random.data, rc);

	dev_dbg(dev, "RNG: returning rand bytes = %d", rc);
out:
	devm_kfree(dev, random);
	devm_kfree(dev, msg);
	return rc;
}

static ssize_t version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct miscdevice *misc_dev = dev_get_drvdata(dev);
	struct nsm *nsm = misc_dev_to_nsm(misc_dev);

	return sysfs_emit(buf, "%d.%d.%d\n", nsm->major, nsm->minor, nsm->patch);
}
static DEVICE_ATTR_RO(version);

static ssize_t max_pcrs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct miscdevice *misc_dev = dev_get_drvdata(dev);
	struct nsm *nsm = misc_dev_to_nsm(misc_dev);

	return sysfs_emit(buf, "%d\n", nsm->max_pcrs);
}
static DEVICE_ATTR_RO(max_pcrs);

static ssize_t digest_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct miscdevice *misc_dev = dev_get_drvdata(dev);
	struct nsm *nsm = misc_dev_to_nsm(misc_dev);

	return sysfs_emit(buf, "%s\n", nsm->digest);
}
static DEVICE_ATTR_RO(digest);

static ssize_t module_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct miscdevice *misc_dev = dev_get_drvdata(dev);
	struct nsm *nsm = misc_dev_to_nsm(misc_dev);

	return sysfs_emit(buf, "%s\n", nsm->module_id);
}
static DEVICE_ATTR_RO(module_id);

/* Sysfs files to access NSM state in addition to ioctls */
static struct attribute *nsm_dev_attrs[] = {
	&dev_attr_version.attr,
	&dev_attr_max_pcrs.attr,
	&dev_attr_digest.attr,
	&dev_attr_module_id.attr,
	NULL
};
ATTRIBUTE_GROUPS(nsm_dev);

/*
 * IOCTL handlers. We always follow the same pattern for each of these: We copy
 * the full user struct to kernel space, then we generate a device message from
 * it, send it to the device and parse the response back into the in-kernel
 * struct. Then we copy the full result back to user space.
 */
struct nsm_ioctl_handler {
	int cmd;
	int (*fill_req)(struct nsm *nsm, struct nsm_data_req *req, void *buffer);
	int (*parse_resp)(struct nsm *nsm, struct nsm_data_resp *resp, void *buffer);
};

static const struct nsm_ioctl_handler nsm_ioctls[] = {
	{ NSM_IOCTL_RAW, _fill_req_raw, _parse_resp_raw },
	{ NSM_IOCTL_DESCRIBE_PCR, _fill_req_describe_pcr, _parse_resp_describe_pcr },
	{ NSM_IOCTL_EXTEND_PCR, _fill_req_extend_pcr, _parse_resp_extend_pcr },
	{ NSM_IOCTL_LOCK_PCR, _fill_req_lock_pcr, _parse_resp_lock_pcr },
	{ NSM_IOCTL_LOCK_PCRS, _fill_req_lock_pcrs, _parse_resp_lock_pcrs },
	{ NSM_IOCTL_DESCRIBE_NSM, _fill_req_describe_nsm, _parse_resp_describe_nsm },
	{ NSM_IOCTL_ATTESTATION, _fill_req_attestation, _parse_resp_attestation },
	{ NSM_IOCTL_RANDOM, _fill_req_get_random, _parse_resp_get_random },
};

static const struct nsm_ioctl_handler *nsm_dev_ioctl_handler(int cmd)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(nsm_ioctls); i++)
		if (nsm_ioctls[i].cmd == cmd)
			return &nsm_ioctls[i];

	return NULL;
}

static long nsm_dev_ioctl(struct file *file, unsigned int cmd,
	unsigned long arg)
{
	void __user *argp = u64_to_user_ptr((u64)arg);
	const struct nsm_ioctl_handler *handler;
	struct nsm *nsm = file_to_nsm(file);
	struct device *dev = &nsm->vdev->dev;
	struct nsm_msg *msg = NULL;
	void *buffer = NULL;
	int r = 0;

	/* Find handler for this ioctl */
	r = -EINVAL;
	handler = nsm_dev_ioctl_handler(cmd);
	if (!handler)
		goto out;

	/* Allocate message buffers to device */
	r = -ENOMEM;
	msg = devm_kzalloc(dev, sizeof(*msg), GFP_KERNEL);
	if (!msg)
		goto out;

	/* Allocate kernel copy of ioctl argument */
	r = -ENOMEM;
	buffer = devm_kmalloc(dev, _IOC_SIZE(cmd), GFP_KERNEL);
	if (!buffer)
		goto out;

	/* Copy user argument struct to kernel argument struct */
	r = -EFAULT;
	if (copy_from_user(buffer, argp, _IOC_SIZE(cmd)))
		goto out;

	/* Convert kernel argument struct to device request */
	r = handler->fill_req(nsm, &msg->req, buffer);
	if (r)
		goto out;

	/* Send message to NSM and read reply */
	r = nsm_sendrecv_msg(nsm, msg);
	if (r)
		goto out;

	/* Parse device response into kernel argument struct */
	r = handler->parse_resp(nsm, &msg->resp, buffer);
	if (r)
		goto out;

	/* Copy kernel argument struct  back to user argument struct */
	r = -EFAULT;
	if (copy_to_user(argp, buffer, _IOC_SIZE(cmd)))
		goto out;

	r = 0;

out:
	devm_kfree(dev, msg);
	devm_kfree(dev, buffer);
	return r;
}

static int nsm_dev_file_open(struct inode *node, struct file *file)
{
	return 0;
}

static int nsm_dev_file_close(struct inode *inode, struct file *file)
{
	return 0;
}

static int nsm_device_init_vq(struct virtio_device *vdev)
{
	struct virtqueue *vq = virtio_find_single_vq(vdev,
		nsm_vq_callback, "nsm.vq.0");
	struct nsm *nsm = vdev->priv;

	if (IS_ERR(vq))
		return PTR_ERR(vq);

	nsm->vq = vq;

	return 0;
}

static const struct file_operations nsm_dev_fops = {
	.open = nsm_dev_file_open,
	.release = nsm_dev_file_close,
	.unlocked_ioctl = nsm_dev_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int nsm_init_sysfs(struct nsm *nsm)
{
	struct device *dev = &nsm->vdev->dev;
	union nsm_describe_nsm *desc = NULL;
	struct nsm_msg *msg = NULL;
	int rc = 0;

	rc = -ENOMEM;
	msg = devm_kzalloc(dev, sizeof(*msg), GFP_KERNEL);
	if (!msg)
		goto out;

	rc = -ENOMEM;
	desc = devm_kzalloc(dev, sizeof(*desc), GFP_KERNEL);
	if (!desc)
		goto out;

	rc = fill_req_describe_nsm(nsm, &msg->req);
	if (rc != 0)
		goto out;

	rc = nsm_sendrecv_msg(nsm, msg);
	if (rc != 0)
		goto out;

	rc = parse_resp_describe_nsm(nsm, &msg->resp, desc);
	if (rc != 0)
		goto out;

	nsm->major = desc->resp.major;
	nsm->minor = desc->resp.minor;
	nsm->patch = desc->resp.patch;
	memcpy(nsm->module_id, desc->resp.module_id, sizeof(nsm->module_id));
	memcpy(nsm->digest, desc->resp.digest, sizeof(nsm->digest));

out:
	kfree(desc);
	kfree(msg);
	return rc;
}

static void nsm_misc_init(struct work_struct *work)
{
	struct nsm *nsm = container_of(work, struct nsm, misc_init);
	struct device *dev = &nsm->vdev->dev;
	int rc;

	/* Initialize sysfs data */
	rc = nsm_init_sysfs(nsm);
	if (rc) {
		dev_err(dev, "Initializing sysfs failed: %d\n", rc);
		return;
	}

	/* Register /dev/nsm device node */
	nsm->misc = (struct miscdevice) {
		.minor	= MISC_DYNAMIC_MINOR,
		.name	= "nsm",
		.fops	= &nsm_dev_fops,
		.mode	= 0666,
		.groups = nsm_dev_groups,
	};

	rc = misc_register(&nsm->misc);
	if (rc)
		dev_err(dev, "misc device registration error: %d.\n", rc);
}

/* Handler for probing the NSM device */
static int nsm_device_probe(struct virtio_device *vdev)
{
	struct device *dev = &vdev->dev;
	struct nsm *nsm;
	int rc;

	nsm = devm_kzalloc(&vdev->dev, sizeof(*nsm), GFP_KERNEL);
	if (!nsm)
		return -ENOMEM;

	vdev->priv = nsm;
	nsm->vdev = vdev;

	rc = nsm_device_init_vq(vdev);
	if (rc) {
		dev_err(dev, "queue failed to initialize: %d.\n", rc);
		goto err_init_vq;
	}

	mutex_init(&nsm->lock);
	init_waitqueue_head(&nsm->wq);

	/* Register as hwrng provider */
	nsm->hwrng = (struct hwrng) {
		.read = nsm_rng_read,
		.name = "nsm-hwrng",
		.quality = 1000,
	};

	rc = devm_hwrng_register(&vdev->dev, &nsm->hwrng);
	if (rc) {
		dev_err(dev, "RNG initialization error: %d.\n", rc);
		goto err_hwrng;
	}

	/*
	 * To initialize the misc device's sysfs properties we need to
	 * communicate with NSM which only works after probe is finished.
	 * Trigger work so we can delay the misc device registration.
	 */
	INIT_WORK(&nsm->misc_init, nsm_misc_init);
	schedule_work(&nsm->misc_init);

	return 0;

err_hwrng:
	vdev->config->del_vqs(vdev);
err_init_vq:
	kfree(nsm);
	return rc;
}

/* Handler for removing the NSM device */
static void nsm_device_remove(struct virtio_device *vdev)
{
	struct nsm *nsm = vdev->priv;

	hwrng_unregister(&nsm->hwrng);

	vdev->config->del_vqs(vdev);
	misc_deregister(&nsm->misc);
	list_del(&nsm->node);
}

/* NSM device configuration structure */
static struct virtio_driver virtio_nsm_driver = {
	.feature_table             = 0,
	.feature_table_size        = 0,
	.feature_table_legacy      = 0,
	.feature_table_size_legacy = 0,
	.driver.name               = KBUILD_MODNAME,
	.driver.owner              = THIS_MODULE,
	.id_table                  = id_table,
	.probe                     = nsm_device_probe,
	.remove                    = nsm_device_remove,
};

module_virtio_driver(virtio_nsm_driver);
MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio NSM driver");
MODULE_LICENSE("GPL");
