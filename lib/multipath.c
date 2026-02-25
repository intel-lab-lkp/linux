// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2018 Christoph Hellwig.
 * Copyright (c) 2026 Oracle and/or its affiliates.
 */
#include <linux/module.h>
#include <linux/multipath.h>

static struct workqueue_struct *mpath_wq;

static void mpath_free_head(struct kref *ref)
{
	struct mpath_head *mpath_head =
		container_of(ref, struct mpath_head, ref);

	cleanup_srcu_struct(&mpath_head->srcu);
	kfree(mpath_head);
}

int mpath_get_head(struct mpath_head *mpath_head)
{
	if (!kref_get_unless_zero(&mpath_head->ref)) {
		return -ENXIO;
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mpath_get_head);

void mpath_put_head(struct mpath_head *mpath_head)
{
	kref_put(&mpath_head->ref, mpath_free_head);
}
EXPORT_SYMBOL_GPL(mpath_put_head);

static void mpath_free_disk(struct kref *ref)
{
	struct mpath_disk *mpath_disk =
		container_of(ref, struct mpath_disk, ref);
	struct mpath_head *mpath_head = mpath_disk->mpath_head;

	put_disk(mpath_disk->disk);
	mpath_put_head(mpath_head);
	kfree(mpath_disk);
}

void mpath_put_disk(struct mpath_disk *mpath_disk)
{
	kref_put(&mpath_disk->ref, mpath_free_disk);
}
EXPORT_SYMBOL_GPL(mpath_put_disk);

static int mpath_get_disk(struct mpath_disk *mpath_disk)
{
	if (!kref_get_unless_zero(&mpath_disk->ref)) {
		return -ENXIO;
	}
	return 0;
}

static int mpath_bdev_open(struct gendisk *disk, blk_mode_t mode)
{
	struct mpath_disk *mpath_disk = disk->private_data;

	return mpath_get_disk(mpath_disk);
}

static void mpath_bdev_release(struct gendisk *disk)
{
	struct mpath_disk *mpath_disk = disk->private_data;

	mpath_put_disk(mpath_disk);
}

const struct block_device_operations mpath_ops = {
	.owner          = THIS_MODULE,
	.open		= mpath_bdev_open,
	.release	= mpath_bdev_release,
};
EXPORT_SYMBOL_GPL(mpath_ops);

static void multipath_partition_scan_work(struct work_struct *work)
{
	struct mpath_disk *mpath_disk =
		container_of(work, struct mpath_disk, partition_scan_work);

	if (WARN_ON_ONCE(!test_and_clear_bit(GD_SUPPRESS_PART_SCAN,
					     &mpath_disk->disk->state)))
		return;

	mutex_lock(&mpath_disk->disk->open_mutex);
	bdev_disk_changed(mpath_disk->disk, false);
	mutex_unlock(&mpath_disk->disk->open_mutex);
}

void mpath_remove_disk(struct mpath_disk *mpath_disk)
{
	struct mpath_head *mpath_head = mpath_disk->mpath_head;

	if (test_and_clear_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags)) {
		struct gendisk *disk = mpath_disk->disk;

		del_gendisk(disk);
	}
}
EXPORT_SYMBOL_GPL(mpath_remove_disk);

void mpath_unregister_disk(struct mpath_disk *mpath_disk)
{
	mpath_remove_disk(mpath_disk);
	mpath_put_disk(mpath_disk);
}
EXPORT_SYMBOL_GPL(mpath_unregister_disk);

struct mpath_disk *mpath_alloc_head_disk(struct queue_limits *lim, int numa_node)
{
	struct mpath_disk *mpath_disk;

	mpath_disk = kzalloc(sizeof(*mpath_disk), GFP_KERNEL);
	if (!mpath_disk)
		return NULL;

	INIT_WORK(&mpath_disk->partition_scan_work,
			multipath_partition_scan_work);
	mutex_init(&mpath_disk->lock);
	kref_init(&mpath_disk->ref);

	mpath_disk->disk = blk_alloc_disk(lim, numa_node);
	if (IS_ERR(mpath_disk->disk)) {
		kfree(mpath_disk);
		return NULL;
	}

	mpath_disk->disk->private_data = mpath_disk;
	mpath_disk->disk->fops = &mpath_ops;

	set_bit(GD_SUPPRESS_PART_SCAN, &mpath_disk->disk->state);

	return mpath_disk;
}
EXPORT_SYMBOL_GPL(mpath_alloc_head_disk);

void mpath_device_set_live(struct mpath_disk *mpath_disk,
			struct mpath_device *mpath_device)
{
	struct mpath_head *mpath_head = mpath_disk->mpath_head;
	int ret;

	if (!mpath_disk)
		return;

	if (!test_and_set_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags)) {
		dev_set_drvdata(disk_to_dev(mpath_disk->disk), mpath_disk);
		ret = device_add_disk(mpath_disk->parent, mpath_disk->disk,
				mpath_head->mpdt->device_groups);
		if (ret) {
			clear_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags);
			return;
		}
		queue_work(mpath_wq, &mpath_disk->partition_scan_work);
	}
}
EXPORT_SYMBOL_GPL(mpath_device_set_live);

struct mpath_head *mpath_alloc_head(void)
{
	struct mpath_head *mpath_head;
	int ret;

	mpath_head = kzalloc(sizeof(*mpath_head), GFP_KERNEL);
	if (!mpath_head)
		return ERR_PTR(-ENOMEM);
	INIT_LIST_HEAD(&mpath_head->dev_list);
	mutex_init(&mpath_head->lock);
	kref_init(&mpath_head->ref);

	ret = init_srcu_struct(&mpath_head->srcu);
	if (ret) {
		kfree(mpath_head);
		return ERR_PTR(ret);
	}

	return mpath_head;
}
EXPORT_SYMBOL_GPL(mpath_alloc_head);

static int __init mpath_init(void)
{
	mpath_wq = alloc_workqueue("mpath-wq",
			WQ_UNBOUND | WQ_MEM_RECLAIM | WQ_SYSFS, 0);
	if (!mpath_wq)
		return -ENOMEM;
	return 0;
}

static void __exit mpath_exit(void)
{
	destroy_workqueue(mpath_wq);
}

module_init(mpath_init);
module_exit(mpath_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("libmultipath");
