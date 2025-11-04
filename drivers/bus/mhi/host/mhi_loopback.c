// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/mhi.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/string.h>
#include <linux/random.h>
#include <linux/kernel.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/cleanup.h>
#include <linux/sizes.h>

#define MHI_LOOPBACK_DEFAULT_TRE_SIZE   32
#define MHI_LOOPBACK_DEFAULT_NUM_TRE    1
#define MHI_LOOPBACK_TIMEOUT_MS         5000
#define MHI_LOOPBACK_MAX_TRE_SIZE       SZ_64K

struct mhi_loopback {
	struct mhi_device *mdev;
	struct mutex lb_mutex;
	struct completion comp;
	atomic_t num_completions_received;
	char result[32];
	u32 num_tre;
	u32 size;
	bool loopback_in_progress;
};

static ssize_t size_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct mhi_loopback *mhi_lb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", mhi_lb->size);
}

static ssize_t size_store(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct mhi_loopback *mhi_lb = dev_get_drvdata(dev);
	u32 val;

	if (kstrtou32(buf, 0, &val)) {
		dev_err(dev, "Invalid size value\n");
		return -EINVAL;
	}

	if (val == 0 || val > MHI_LOOPBACK_MAX_TRE_SIZE) {
		dev_err(dev, "Size must be between 1 and %u bytes\n",
			MHI_LOOPBACK_MAX_TRE_SIZE);
		return -EINVAL;
	}

	guard(mutex)(&mhi_lb->lb_mutex);
	if (mhi_lb->loopback_in_progress)
		return -EBUSY;

	mhi_lb->size = val;
	return count;
}
static DEVICE_ATTR_RW(size);

static ssize_t num_tre_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct mhi_loopback *mhi_lb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", mhi_lb->num_tre);
}

static ssize_t num_tre_store(struct device *dev,
			     struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct mhi_loopback *mhi_lb = dev_get_drvdata(dev);
	u32 val;
	int el_num;

	if (kstrtou32(buf, 0, &val)) {
		dev_err(dev, "Invalid num_tre value\n");
		return -EINVAL;
	}

	if (val == 0) {
		dev_err(dev, "Number of TREs cannot be zero\n");
		return -EINVAL;
	}

	guard(mutex)(&mhi_lb->lb_mutex);
	if (mhi_lb->loopback_in_progress)
		return -EBUSY;

	el_num = mhi_get_free_desc_count(mhi_lb->mdev, DMA_TO_DEVICE);
	if (val > el_num) {
		dev_err(dev, "num_tre (%u) exceeds ring capacity (%d)\n", val, el_num);
		return -EINVAL;
	}

	mhi_lb->num_tre = val;
	return count;
}
static DEVICE_ATTR_RW(num_tre);

static ssize_t start_store(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct mhi_loopback *mhi_lb = dev_get_drvdata(dev);
	void *send_buf __free(kfree) = NULL;
	void *recv_buf __free(kfree) = NULL;
	u32 total_size, tre_count, tre_size;
	int ret, i;

	guard(mutex)(&mhi_lb->lb_mutex);

	if (mhi_lb->loopback_in_progress)
		return -EBUSY;

	atomic_set(&mhi_lb->num_completions_received, 0);
	mhi_lb->loopback_in_progress = true;

	tre_size = mhi_lb->size;
	tre_count = mhi_lb->num_tre;

	strscpy(mhi_lb->result, "Loopback started", sizeof(mhi_lb->result));

	total_size = tre_count * tre_size;

	recv_buf = kzalloc(total_size, GFP_KERNEL);
	if (!recv_buf) {
		strscpy(mhi_lb->result, "Memory allocation failed", sizeof(mhi_lb->result));
		mhi_lb->loopback_in_progress = false;
		return -ENOMEM;
	}

	send_buf = kzalloc(total_size, GFP_KERNEL);
	if (!send_buf) {
		strscpy(mhi_lb->result, "Memory allocation failed", sizeof(mhi_lb->result));
		mhi_lb->loopback_in_progress = false;
		return -ENOMEM;
	}

	for (i = 0; i < tre_count; i++) {
		ret = mhi_queue_buf(mhi_lb->mdev, DMA_FROM_DEVICE, recv_buf + (i * tre_size),
				    tre_size, MHI_EOT);
		if (ret) {
			dev_err(dev, "Unable to queue read TRE %d: %d\n", i, ret);
			strscpy(mhi_lb->result, "Queue tre failed", sizeof(mhi_lb->result));
			mhi_lb->loopback_in_progress = false;
			return ret;
		}
	}

	get_random_bytes(send_buf, total_size);

	reinit_completion(&mhi_lb->comp);

	for (i = 0; i < tre_count - 1; i++) {
		ret = mhi_queue_buf(mhi_lb->mdev, DMA_TO_DEVICE, send_buf + (i * tre_size),
				    tre_size, MHI_CHAIN);
		if (ret) {
			dev_err(dev, "Unable to queue send TRE %d (chained): %d\n", i, ret);
			strscpy(mhi_lb->result, "Queue send failed", sizeof(mhi_lb->result));
			mhi_lb->loopback_in_progress = false;
			return ret;
		}
	}

	ret = mhi_queue_buf(mhi_lb->mdev, DMA_TO_DEVICE, send_buf + (i * tre_size),
			    tre_size, MHI_EOT);
	if (ret) {
		dev_err(dev, "Unable to queue final TRE: %d\n", ret);
		strscpy(mhi_lb->result, "Queue final tre failed", sizeof(mhi_lb->result));
		mhi_lb->loopback_in_progress = false;
		return ret;
	}

	if (!wait_for_completion_timeout(&mhi_lb->comp,
					 msecs_to_jiffies(MHI_LOOPBACK_TIMEOUT_MS))) {
		strscpy(mhi_lb->result, "Loopback timeout", sizeof(mhi_lb->result));
		dev_err(dev, "Loopback test timed out\n");
		mhi_lb->loopback_in_progress = false;
		return -ETIMEDOUT;
	}

	ret = memcmp(send_buf, recv_buf, total_size);
	if (!ret) {
		strscpy(mhi_lb->result, "Loopback successful", sizeof(mhi_lb->result));
		dev_info(dev, "Loopback test passed\n");
	} else {
		strscpy(mhi_lb->result, "Loopback data mismatch", sizeof(mhi_lb->result));
		dev_err(dev, "Loopback test failed\n");
		ret = -EIO;
	}

	mhi_lb->loopback_in_progress = false;
	return ret;
}

static DEVICE_ATTR_WO(start);

static ssize_t status_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct mhi_loopback *mhi_lb = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%s\n", mhi_lb->result);
}
static DEVICE_ATTR_RO(status);

static void mhi_loopback_dl_callback(struct mhi_device *mhi_dev,
				     struct mhi_result *mhi_res)
{
	struct mhi_loopback *mhi_lb = dev_get_drvdata(&mhi_dev->dev);

	if (!mhi_res->transaction_status) {
		if (atomic_inc_return(&mhi_lb->num_completions_received) >= mhi_lb->num_tre) {
			atomic_set(&mhi_lb->num_completions_received, 0);
			complete(&mhi_lb->comp);
		}
	} else {
		dev_err(&mhi_dev->dev, "DL callback error: status %d\n",
			mhi_res->transaction_status);
		atomic_set(&mhi_lb->num_completions_received, 0);
		complete(&mhi_lb->comp);
	}
}

static void mhi_loopback_ul_callback(struct mhi_device *mhi_dev,
				     struct mhi_result *mhi_res)
{
}

static int mhi_loopback_probe(struct mhi_device *mhi_dev,
			      const struct mhi_device_id *id)
{
	struct mhi_loopback *mhi_lb;
	int rc;

	mhi_lb = devm_kzalloc(&mhi_dev->dev, sizeof(*mhi_lb), GFP_KERNEL);
	if (!mhi_lb)
		return -ENOMEM;

	mhi_lb->mdev = mhi_dev;

	dev_set_drvdata(&mhi_dev->dev, mhi_lb);

	mhi_lb->size = MHI_LOOPBACK_DEFAULT_TRE_SIZE;
	mhi_lb->num_tre = MHI_LOOPBACK_DEFAULT_NUM_TRE;
	mhi_lb->loopback_in_progress = false;

	mutex_init(&mhi_lb->lb_mutex);
	strscpy(mhi_lb->result, "Loopback not started", sizeof(mhi_lb->result));

	rc = sysfs_create_file(&mhi_dev->dev.kobj, &dev_attr_size.attr);
	if (rc) {
		dev_err(&mhi_dev->dev, "failed to create size sysfs file\n");
		goto out;
	}

	rc = sysfs_create_file(&mhi_dev->dev.kobj, &dev_attr_num_tre.attr);
	if (rc) {
		dev_err(&mhi_dev->dev, "failed to create num_tre sysfs file\n");
		goto del_size_sysfs;
	}

	rc = sysfs_create_file(&mhi_dev->dev.kobj, &dev_attr_start.attr);
	if (rc) {
		dev_err(&mhi_dev->dev, "failed to create start sysfs file\n");
		goto del_num_tre_sysfs;
	}

	rc = sysfs_create_file(&mhi_dev->dev.kobj, &dev_attr_status.attr);
	if (rc) {
		dev_err(&mhi_dev->dev, "failed to create status sysfs file\n");
		goto del_start_sysfs;
	}

	rc = mhi_prepare_for_transfer(mhi_lb->mdev);
	if (rc) {
		dev_err(&mhi_dev->dev, "failed to prepare for transfers\n");
		goto del_status_sysfs;
	}

	init_completion(&mhi_lb->comp);

	return 0;

del_status_sysfs:
	sysfs_remove_file(&mhi_dev->dev.kobj, &dev_attr_status.attr);
del_start_sysfs:
	sysfs_remove_file(&mhi_dev->dev.kobj, &dev_attr_start.attr);
del_num_tre_sysfs:
	sysfs_remove_file(&mhi_dev->dev.kobj, &dev_attr_num_tre.attr);
del_size_sysfs:
	sysfs_remove_file(&mhi_dev->dev.kobj, &dev_attr_size.attr);
out:
	return rc;
}

static void mhi_loopback_remove(struct mhi_device *mhi_dev)
{
	struct mhi_loopback *mhi_lb = dev_get_drvdata(&mhi_dev->dev);

	if (mhi_lb)
		complete(&mhi_lb->comp);

	sysfs_remove_file(&mhi_dev->dev.kobj, &dev_attr_status.attr);
	sysfs_remove_file(&mhi_dev->dev.kobj, &dev_attr_start.attr);
	sysfs_remove_file(&mhi_dev->dev.kobj, &dev_attr_num_tre.attr);
	sysfs_remove_file(&mhi_dev->dev.kobj, &dev_attr_size.attr);
	mhi_unprepare_from_transfer(mhi_dev);
	dev_set_drvdata(&mhi_dev->dev, NULL);
}

static const struct mhi_device_id mhi_loopback_id_table[] = {
	{ .chan = "LOOPBACK"},
	{}
};
MODULE_DEVICE_TABLE(mhi, mhi_loopback_id_table);

static struct mhi_driver mhi_loopback_driver = {
	.probe = mhi_loopback_probe,
	.remove = mhi_loopback_remove,
	.dl_xfer_cb = mhi_loopback_dl_callback,
	.ul_xfer_cb = mhi_loopback_ul_callback,
	.id_table = mhi_loopback_id_table,
	.driver = {
		.name = "mhi_loopback",
	},
};

module_mhi_driver(mhi_loopback_driver);

MODULE_AUTHOR("Krishna chaitanya chundru <krishna.chundru@oss.qualcomm.com>");
MODULE_AUTHOR("Sumit Kumar <sumit.kumar@oss.qualcomm.com>");
MODULE_DESCRIPTION("MHI Host Loopback Driver");
MODULE_LICENSE("GPL");
