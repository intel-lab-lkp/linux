// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/mhi_ep.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/skbuff.h>

struct mhi_ep_loopback {
	struct workqueue_struct *loopback_wq;
	struct mhi_ep_device *mdev;
};

struct mhi_ep_loopback_work {
	struct mhi_ep_device *mdev;
	struct work_struct work;
	struct sk_buff *skb;
};

static void mhi_ep_loopback_work_handler(struct work_struct *work)
{
	int ret;
	struct mhi_ep_loopback_work *mhi_ep_lb_work = container_of(work,
								struct mhi_ep_loopback_work, work);

	ret = mhi_ep_queue_skb(mhi_ep_lb_work->mdev, mhi_ep_lb_work->skb);
	if (ret) {
		dev_err(&mhi_ep_lb_work->mdev->dev, "Failed to send the packet\n");
		kfree_skb(mhi_ep_lb_work->skb);
	}

	kfree(mhi_ep_lb_work);
}

static void mhi_ep_loopback_ul_callback(struct mhi_ep_device *mhi_dev,
					struct mhi_result *mhi_res)
{
	struct mhi_ep_loopback *mhi_ep_lb = dev_get_drvdata(&mhi_dev->dev);
	struct mhi_ep_loopback_work *mhi_ep_lb_work;
	struct sk_buff *skb;

	if (!(mhi_res->transaction_status)) {
		skb = alloc_skb(mhi_res->bytes_xferd, GFP_KERNEL);
		if (!skb) {
			dev_err(&mhi_dev->dev, "Failed to allocate skb\n");
			return;
		}

		skb_put_data(skb, mhi_res->buf_addr, mhi_res->bytes_xferd);

		mhi_ep_lb_work = kmalloc(sizeof(*mhi_ep_lb_work), GFP_KERNEL);
		if (!mhi_ep_lb_work) {
			dev_err(&mhi_dev->dev, "Unable to allocate the work structure\n");
			kfree_skb(skb);
			return;
		}

		INIT_WORK(&mhi_ep_lb_work->work, mhi_ep_loopback_work_handler);
		mhi_ep_lb_work->mdev = mhi_dev;
		mhi_ep_lb_work->skb = skb;

		queue_work(mhi_ep_lb->loopback_wq, &mhi_ep_lb_work->work);
	}
}

static void mhi_ep_loopback_dl_callback(struct mhi_ep_device *mhi_dev,
					struct mhi_result *mhi_res)
{
	struct sk_buff *skb;

	if (mhi_res->transaction_status)
		return;

	skb = mhi_res->buf_addr;
	if (skb)
		kfree_skb(skb);
}

static int mhi_ep_loopback_probe(struct mhi_ep_device *mhi_dev, const struct mhi_device_id *id)
{
	struct mhi_ep_loopback *mhi_ep_lb;

	mhi_ep_lb = devm_kzalloc(&mhi_dev->dev, sizeof(struct mhi_ep_loopback), GFP_KERNEL);
	if (!mhi_ep_lb)
		return -ENOMEM;

	mhi_ep_lb->loopback_wq = alloc_ordered_workqueue("mhi_loopback", WQ_MEM_RECLAIM);
	if (!mhi_ep_lb->loopback_wq) {
		dev_err(&mhi_dev->dev, "Failed to create workqueue.\n");
		return -ENOMEM;
	}

	mhi_ep_lb->mdev = mhi_dev;
	dev_set_drvdata(&mhi_dev->dev, mhi_ep_lb);

	return 0;
}

static void mhi_ep_loopback_remove(struct mhi_ep_device *mhi_dev)
{
	struct mhi_ep_loopback *mhi_ep_lb = dev_get_drvdata(&mhi_dev->dev);

	destroy_workqueue(mhi_ep_lb->loopback_wq);
	dev_set_drvdata(&mhi_dev->dev, NULL);
}

static const struct mhi_device_id mhi_ep_loopback_id_table[] = {
	{ .chan = "LOOPBACK"},
	{}
};
MODULE_DEVICE_TABLE(mhi, mhi_ep_loopback_id_table);

static struct mhi_ep_driver mhi_ep_loopback_driver = {
	.probe = mhi_ep_loopback_probe,
	.remove = mhi_ep_loopback_remove,
	.dl_xfer_cb = mhi_ep_loopback_dl_callback,
	.ul_xfer_cb = mhi_ep_loopback_ul_callback,
	.id_table = mhi_ep_loopback_id_table,
	.driver = {
		.name = "mhi_ep_loopback",
		.owner = THIS_MODULE,
	},
};

module_mhi_ep_driver(mhi_ep_loopback_driver);

MODULE_AUTHOR("Krishna chaitanya chundru <krishna.chundru@oss.qualcomm.com>");
MODULE_AUTHOR("Sumit Kumar <sumit.kumar@oss.qualcomm.com>");
MODULE_DESCRIPTION("MHI Endpoint Loopback driver");
MODULE_LICENSE("GPL");
