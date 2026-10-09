// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2020-2021, The Linux Foundation. All rights reserved. */
/* Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries. */

#include <drm/drm_file.h>
#include <drm/drm_ras.h>
#include <asm/byteorder.h>
#include <linux/completion.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mhi.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/sprintf.h>
#include <linux/srcu.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <uapi/drm/qaic_accel.h>

#include "qaic.h"
#include "qaic_telemetry.h"

#define MAGIC				0x55AA
#define VERSION				0x1

enum fw_errno {
	ERR_READ_ONLY		= -1,
	ERR_NOT_SUPPORTED	= -2,
	ERR_WHILE_PROCESSING	= -3,
	ERR_INVALID_VALUE	= -4,
	ERR_INVALID_HEADER	= -5,
};

enum cmd_type {
	TYPE_READ,  /* read value from device */
	TYPE_WRITE, /* write value to device */
};

enum msg_type {
	MSG_PUSH, /* async push from device */
	MSG_REQ,  /* sync request to device */
	MSG_RESP, /* sync response from device */
};

struct telemetry_data {
	u8	cmd;
	u8	cmd_type;
	u8	status;
	__le64	val; /* signed */
} __packed;

struct telemetry_header {
	__le16	magic;
	__le16	ver;
	__le32	seq_num;
	u8	type;
	u8	id;
	__le16	len;
} __packed;

struct telemetry_msg { /* little endian encoded */
	struct telemetry_header hdr;
	struct telemetry_data data;
} __packed;

struct wrapper_msg {
	struct kref ref_count;
	struct telemetry_msg msg;
};

struct xfer_queue_elem {
	/*
	 * Node in list of ongoing transfer request on telemetry channel.
	 * Maintained by root device struct
	 */
	struct list_head list;
	/* Sequence number of this transfer request */
	u32 seq_num;
	/* This is used to wait on until completion of transfer request */
	struct completion xfer_done;
	/* Received data from device */
	void *buf;
};

struct resp_work {
	/* Work struct to schedule work coming on QAIC_TELEMETRY channel */
	struct work_struct work;
	/* Root struct of device, used to access device resources */
	struct qaic_device *qdev;
	/* Buffer used by MHI for transfer requests */
	void *buf;
};

static void free_wrapper(struct kref *ref)
{
	struct wrapper_msg *wrapper = container_of(ref, struct wrapper_msg, ref_count);

	kfree(wrapper);
}

int telemetry_request(struct qaic_device *qdev, u8 cmd, u8 cmd_type, s64 *val)
{
	struct wrapper_msg *wrapper;
	struct xfer_queue_elem elem;
	struct telemetry_msg *resp;
	struct telemetry_msg *req;
	long ret = 0;

	wrapper = kzalloc_obj(*wrapper);
	if (!wrapper)
		return -ENOMEM;

	kref_init(&wrapper->ref_count);
	req = &wrapper->msg;

	ret = mutex_lock_interruptible(&qdev->tele_mutex);
	if (ret)
		goto free_req;

	req->hdr.magic = cpu_to_le16(MAGIC);
	req->hdr.ver = cpu_to_le16(VERSION);
	req->hdr.seq_num = cpu_to_le32(qdev->tele_next_seq_num++);
	req->hdr.type = MSG_REQ;
	req->hdr.id = 0;
	req->hdr.len = cpu_to_le16(sizeof(req->data));

	req->data.cmd = cmd;
	req->data.cmd_type = cmd_type;
	req->data.status = 0;
	if (cmd_type == TYPE_READ)
		req->data.val = cpu_to_le64(0);
	else
		req->data.val = cpu_to_le64(*val);

	elem.seq_num = qdev->tele_next_seq_num - 1;
	elem.buf = NULL;
	init_completion(&elem.xfer_done);
	if (likely(!qdev->tele_lost_buf)) {
		resp = kzalloc_obj(*resp);
		if (!resp) {
			mutex_unlock(&qdev->tele_mutex);
			ret = -ENOMEM;
			goto free_req;
		}

		ret = mhi_queue_buf(qdev->tele_ch, DMA_FROM_DEVICE, resp, sizeof(*resp), MHI_EOT);
		if (ret) {
			mutex_unlock(&qdev->tele_mutex);
			goto free_resp;
		}
	} else {
		/*
		 * we lost a buffer because we queued a recv buf, but then
		 * queuing the corresponding tx buf failed. To try to avoid
		 * a memory leak, lets reclaim it and use it for this
		 * transaction.
		 */
		qdev->tele_lost_buf = false;
	}

	kref_get(&wrapper->ref_count);
	ret = mhi_queue_buf(qdev->tele_ch, DMA_TO_DEVICE, req, sizeof(*req), MHI_EOT);
	if (ret) {
		qdev->tele_lost_buf = true;
		kref_put(&wrapper->ref_count, free_wrapper);
		mutex_unlock(&qdev->tele_mutex);
		goto free_req;
	}

	list_add_tail(&elem.list, &qdev->tele_xfer_list);
	mutex_unlock(&qdev->tele_mutex);

	ret = wait_for_completion_interruptible_timeout(&elem.xfer_done, HZ);
	/*
	 * not using _interruptable because we have to cleanup or we'll
	 * likely cause memory corruption
	 */
	mutex_lock(&qdev->tele_mutex);
	if (!list_empty(&elem.list))
		list_del(&elem.list);
	if (!ret && !elem.buf)
		ret = -ETIMEDOUT;
	else if (ret > 0 && !elem.buf)
		ret = -EIO;
	mutex_unlock(&qdev->tele_mutex);

	resp = elem.buf;

	if (ret < 0)
		goto free_resp;

	if (le16_to_cpu(resp->hdr.magic) != MAGIC || le16_to_cpu(resp->hdr.ver) != VERSION ||
	    resp->hdr.type != MSG_RESP || resp->hdr.id != 0 ||
	    le16_to_cpu(resp->hdr.len) != sizeof(resp->data) || resp->data.cmd != cmd ||
	    resp->data.cmd_type != cmd_type) {
		ret = -EINVAL;
		goto free_resp;
	}

	if (resp->data.status) {
		switch ((int8_t) resp->data.status) {
		case ERR_READ_ONLY:
			ret = -EROFS;
			break;
		case ERR_NOT_SUPPORTED:
			ret = -EOPNOTSUPP;
			break;
		case ERR_WHILE_PROCESSING:
			ret = -EREMOTEIO;
			break;
		case ERR_INVALID_VALUE:
			ret = -EINVAL;
			break;
		case ERR_INVALID_HEADER:
			ret = -EBADR;
			break;
		default:
			ret = -EPROTO;
		}
		goto free_resp;
	}

	if (cmd_type == TYPE_READ)
		*val = le64_to_cpu(resp->data.val);

	ret = 0;

free_resp:
	kfree(resp);
free_req:
	kref_put(&wrapper->ref_count, free_wrapper);

	return ret;
}

static int qaic_telemetry_mhi_probe(struct mhi_device *mhi_dev, const struct mhi_device_id *id)
{
	struct qaic_device *qdev = pci_get_drvdata(to_pci_dev(mhi_dev->mhi_cntrl->cntrl_dev));
	int ret;

	ret = mhi_prepare_for_transfer(mhi_dev);
	if (ret)
		return ret;

	dev_set_drvdata(&mhi_dev->dev, qdev);
	qdev->tele_ch = mhi_dev;
	qdev->tele_lost_buf = false;

	return 0;
}

static void qaic_telemetry_mhi_remove(struct mhi_device *mhi_dev)
{
	struct qaic_device *qdev = dev_get_drvdata(&mhi_dev->dev);

	mhi_unprepare_from_transfer(qdev->tele_ch);
	qdev->tele_ch = NULL;
}

static void resp_worker(struct work_struct *work)
{
	struct resp_work *resp = container_of(work, struct resp_work, work);
	struct qaic_device *qdev = resp->qdev;
	struct telemetry_msg *msg = resp->buf;
	struct xfer_queue_elem *elem;
	struct xfer_queue_elem *i;
	bool found = false;

	if (le16_to_cpu(msg->hdr.magic) != MAGIC) {
		kfree(msg);
		kfree(resp);
		return;
	}

	mutex_lock(&qdev->tele_mutex);
	list_for_each_entry_safe(elem, i, &qdev->tele_xfer_list, list) {
		if (elem->seq_num == le32_to_cpu(msg->hdr.seq_num)) {
			found = true;
			list_del_init(&elem->list);
			elem->buf = msg;
			complete_all(&elem->xfer_done);
			break;
		}
	}
	mutex_unlock(&qdev->tele_mutex);

	if (!found)
		/* request must have timed out, drop packet */
		kfree(msg);

	kfree(resp);
}

static void qaic_telemetry_mhi_ul_xfer_cb(struct mhi_device *mhi_dev,
					  struct mhi_result *mhi_result)
{
	struct telemetry_msg *msg = mhi_result->buf_addr;
	struct wrapper_msg *wrapper;

	wrapper = container_of(msg, struct wrapper_msg, msg);
	kref_put(&wrapper->ref_count, free_wrapper);
}

static void qaic_telemetry_mhi_dl_xfer_cb(struct mhi_device *mhi_dev,
					  struct mhi_result *mhi_result)
{
	struct qaic_device *qdev = dev_get_drvdata(&mhi_dev->dev);
	struct telemetry_msg *msg = mhi_result->buf_addr;
	struct resp_work *resp;

	if (mhi_result->transaction_status) {
		kfree(msg);
		return;
	}

	resp = kmalloc(sizeof(*resp), GFP_ATOMIC);
	if (!resp) {
		pci_err(qdev->pdev, "dl_xfer_cb alloc fail, dropping message\n");
		kfree(msg);
		return;
	}

	INIT_WORK(&resp->work, resp_worker);
	resp->qdev = qdev;
	resp->buf = msg;
	queue_work(qdev->tele_wq, &resp->work);
}

static const struct mhi_device_id qaic_telemetry_mhi_match_table[] = {
	{ .chan = "QAIC_TELEMETRY", },
	{},
};

static struct mhi_driver qaic_telemetry_mhi_driver = {
	.id_table = qaic_telemetry_mhi_match_table,
	.remove = qaic_telemetry_mhi_remove,
	.probe = qaic_telemetry_mhi_probe,
	.ul_xfer_cb = qaic_telemetry_mhi_ul_xfer_cb,
	.dl_xfer_cb = qaic_telemetry_mhi_dl_xfer_cb,
	.driver = {
		.name = "qaic_telemetry",
	},
};

int qaic_telemetry_register(void)
{
	return mhi_driver_register(&qaic_telemetry_mhi_driver);
}

void qaic_telemetry_unregister(void)
{
	mhi_driver_unregister(&qaic_telemetry_mhi_driver);
}

void wake_all_telemetry(struct qaic_device *qdev)
{
	struct xfer_queue_elem *elem;
	struct xfer_queue_elem *i;

	mutex_lock(&qdev->tele_mutex);
	list_for_each_entry_safe(elem, i, &qdev->tele_xfer_list, list) {
		list_del_init(&elem->list);
		complete_all(&elem->xfer_done);
	}
	qdev->tele_lost_buf = false;
	mutex_unlock(&qdev->tele_mutex);
}

static int qaic_telemetry_pre(struct drm_ras_node *ep)
{
	struct qaic_device *qdev = ep->priv;
	struct inode *dev_inode;
	struct path path;
	char devpath[24];
	int ret;

	snprintf(devpath, sizeof(devpath), "/dev/accel/%s", to_accel_kdev(qdev->qddev)->kobj.name);
	ret = kern_path(devpath, LOOKUP_FOLLOW, &path);
	if (ret)
		return -EINVAL;

	dev_inode = d_inode(path.dentry);
	ret = inode_permission(&nop_mnt_idmap, dev_inode, MAY_READ | MAY_WRITE);
	path_put(&path);

	return ret;
}

static int qaic_telemetry_read(struct drm_ras_node *ep, u32 *field, u64 *value, s32 *status)
{
	struct qaic_device *qdev = ep->priv;

	*status = (s32)telemetry_request(qdev, *field, TYPE_READ, value);

	return *status;
}

static int qaic_telemetry_write(struct drm_ras_node *ep, u32 *field, u64 *value, s32 *status)
{
	struct qaic_device *qdev = ep->priv;

	*status = (s32)telemetry_request(qdev, *field, TYPE_WRITE, value);

	return *status;
}

int qaic_drm_ras_register_node(struct qaic_device *qdev)
{
	struct drm_ras_node *node;
	struct pci_dev *pdev = qdev->pdev;
	const char *device_name, *node_name;
	int ret;

	node = kzalloc_obj(struct drm_ras_node);
	if (!node)
		return -ENOMEM;

	device_name = kasprintf(GFP_KERNEL, "%04x:%02x:%02x.%d",
				pci_domain_nr(pdev->bus), pdev->bus->number,
				PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));
	if (!device_name) {
		kfree(node);
		return -ENOMEM;
	}

	node_name = kasprintf(GFP_KERNEL, "qaic_telemetry");
	if (!node_name) {
		kfree(device_name);
		kfree(node);
		return -ENOMEM;
	}

	node->device_name = device_name;
	node->node_name = node_name;
	node->type = DRM_RAS_NODE_TYPE_TELEMETRY;
	node->priv = qdev;
	node->telemetry_read = qaic_telemetry_read;
	node->telemetry_write = qaic_telemetry_write;
	node->telemetry_pre = qaic_telemetry_pre;

	ret = drm_ras_node_register(node);
	if (ret) {
		kfree(device_name);
		kfree(node_name);
		kfree(qdev->ras_node);
		return ret;
	}

	qdev->ras_node = node;
	return 0;
}

void qaic_drm_ras_unregister_node(struct qaic_device *qdev)
{
	drm_ras_node_unregister(qdev->ras_node);
	kfree(qdev->ras_node->node_name);
	kfree(qdev->ras_node->device_name);
	kfree(qdev->ras_node);
}
