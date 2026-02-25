// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2017-2018 Christoph Hellwig.
 * Copyright (c) 2026 Oracle and/or its affiliates.
 */
#include <linux/module.h>
#include <linux/multipath.h>
#include <trace/events/block.h>

static struct mpath_device *mpath_find_path(struct mpath_head *mpath_head);

static struct workqueue_struct *mpath_wq;

static const char *mpath_iopolicy_names[] = {
	[MPATH_IOPOLICY_NUMA]	= "numa",
	[MPATH_IOPOLICY_RR]	= "round-robin",
	[MPATH_IOPOLICY_QD]	= "queue-depth",
};

int mpath_set_iopolicy(const char *val, int *iopolicy)
{
	if (!val)
		return -EINVAL;
	if (!strncmp(val, "numa", 4))
		*iopolicy = MPATH_IOPOLICY_NUMA;
	else if (!strncmp(val, "round-robin", 11))
		*iopolicy = MPATH_IOPOLICY_RR;
	else if (!strncmp(val, "queue-depth", 11))
		*iopolicy = MPATH_IOPOLICY_QD;
	else
		return -EINVAL;

	return 0;
}
EXPORT_SYMBOL_GPL(mpath_set_iopolicy);

int mpath_get_iopolicy(char *buf, int iopolicy)
{
	return sprintf(buf, "%s\n", mpath_iopolicy_names[iopolicy]);
}
EXPORT_SYMBOL_GPL(mpath_get_iopolicy);

#ifdef CONFIG_BLK_DEV_ZONED
static int mpath_bdev_report_zones(struct gendisk *disk, sector_t sector,
		unsigned int nr_zones, struct blk_report_zones_args *args)
{
	struct mpath_disk *mpath_disk = mpath_gendisk_to_disk(disk);
	struct mpath_head *mpath_head = mpath_disk->mpath_head;
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	if (!mpath_head->mpdt->report_zones)
		return -EOPNOTSUPP;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device)
		ret = mpath_head->mpdt->report_zones(mpath_device, sector,
			nr_zones, args);
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	return ret;
}
#else
#define mpath_bdev_report_zones	NULL
#endif /* CONFIG_BLK_DEV_ZONED */

void mpath_synchronize(struct mpath_head *mpath_head)
{
	synchronize_srcu(&mpath_head->srcu);
}
EXPORT_SYMBOL_GPL(mpath_synchronize);

void mpath_add_device(struct mpath_head *mpath_head,
			struct mpath_device *mpath_device)
{
	mutex_lock(&mpath_head->lock);
	list_add_tail_rcu(&mpath_device->siblings, &mpath_head->dev_list);
	mutex_unlock(&mpath_head->lock);
	cancel_delayed_work(&mpath_head->remove_work);
}
EXPORT_SYMBOL_GPL(mpath_add_device);

void mpath_delete_device(struct mpath_head *mpath_head,
			struct mpath_device *mpath_device)
{
	mutex_lock(&mpath_head->lock);
	list_del_rcu(&mpath_device->siblings);
	mutex_unlock(&mpath_head->lock);
}
EXPORT_SYMBOL_GPL(mpath_delete_device);

int mpath_call_for_device(struct mpath_head *mpath_head,
			int (*cb)(struct mpath_device *mpath_device))
{
	struct mpath_device *mpath_device;
	int ret = -EWOULDBLOCK, srcu_idx;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device)
		ret = cb(mpath_device);
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}
EXPORT_SYMBOL_GPL(mpath_call_for_device);

bool mpath_clear_current_path(struct mpath_head *mpath_head,
			struct mpath_device *mpath_device)
{
	bool changed = false;
	int node;

	if (!mpath_head)
		goto out;

	for_each_node(node) {
		if (mpath_device ==
			rcu_access_pointer(mpath_head->current_path[node])) {
			rcu_assign_pointer(mpath_head->current_path[node],
				NULL);
			changed = true;
		}
	}
out:
	return changed;
}
EXPORT_SYMBOL_GPL(mpath_clear_current_path);

static void mpath_revalidate_paths_iter(struct mpath_disk *mpath_disk,
	void (*cb)(struct mpath_device *mpath_device, sector_t capacity))
{
	struct mpath_head *mpath_head = mpath_disk->mpath_head;
	sector_t capacity = get_capacity(mpath_disk->disk);
	struct mpath_device *mpath_device;
	int srcu_idx;

	if (!cb)
		return;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {
		cb(mpath_device, capacity);
	}
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
}

void mpath_clear_paths(struct mpath_head *mpath_head)
{
	int node;

	for_each_node(node)
		rcu_assign_pointer(mpath_head->current_path[node], NULL);
}
EXPORT_SYMBOL_GPL(mpath_clear_paths);

void mpath_revalidate_paths(struct mpath_disk *mpath_disk,
	void (*cb)(struct mpath_device *mpath_device, sector_t capacity))
{
	struct mpath_head *mpath_head = mpath_disk->mpath_head;

	mpath_revalidate_paths_iter(mpath_disk, cb);
	mpath_clear_paths(mpath_head);

	kblockd_schedule_work(&mpath_head->requeue_work);
}
EXPORT_SYMBOL_GPL(mpath_revalidate_paths);

static bool mpath_path_is_disabled(struct mpath_head *mpath_head,
				struct mpath_device *mpath_device)
{
	return mpath_head->mpdt->is_disabled(mpath_device);
}

static struct mpath_device *__mpath_find_path(struct mpath_head *mpath_head,
			enum mpath_iopolicy_e iopolicy, int node)
{
	int found_distance = INT_MAX, fallback_distance = INT_MAX, distance;
	struct mpath_device *mpath_dev_found, *mpath_dev_fallback,
			*mpath_device;

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
		srcu_read_lock_held(&mpath_head->srcu)) {
		if (mpath_path_is_disabled(mpath_head, mpath_device))
			continue;

		if (mpath_device->numa_node != NUMA_NO_NODE &&
		    (iopolicy == MPATH_IOPOLICY_NUMA))
			distance = node_distance(node, mpath_device->numa_node);
		else
			distance = LOCAL_DISTANCE;

		switch(mpath_head->mpdt->get_access_state(mpath_device)) {
		case MPATH_STATE_OPTIMIZED:
		    if (distance < found_distance) {
			    found_distance = distance;
			    mpath_dev_found = mpath_device;
		    }
		    break;
		case MPATH_STATE_ACTIVE:
		    if (distance < fallback_distance) {
			    fallback_distance = distance;
			    mpath_dev_fallback = mpath_device;
		    }
		    break;
		default:
		    break;
		}
	}

	if (!mpath_dev_found)
		mpath_dev_found = mpath_dev_fallback;

	if (mpath_dev_found)
		rcu_assign_pointer(mpath_head->current_path[node],
			mpath_dev_found);

	return mpath_dev_found;
}

static struct mpath_device *mpath_next_dev(struct mpath_head *mpath_head,
			struct mpath_device *mpath_dev)
{
	mpath_dev = list_next_or_null_rcu(&mpath_head->dev_list,
			&mpath_dev->siblings, struct mpath_device,
			siblings);

	if (mpath_dev)
		return mpath_dev;
	return list_first_or_null_rcu(&mpath_head->dev_list,
				struct mpath_device, siblings);
}

static struct mpath_device *mpath_round_robin_path(
				struct mpath_head *mpath_head,
				enum mpath_iopolicy_e iopolicy)
{
	struct mpath_device *mpath_device, *found = NULL;
	int node = numa_node_id();
	enum mpath_access_state access_state_old;
	struct mpath_device *old =
			srcu_dereference(mpath_head->current_path[node],
				&mpath_head->srcu);

	if (unlikely(!old))
		return __mpath_find_path(mpath_head, iopolicy, node);

	if (list_is_singular(&mpath_head->dev_list)) {
		if (mpath_path_is_disabled(mpath_head, old))
			return NULL;
		return old;
	}

	for (mpath_device = mpath_next_dev(mpath_head, old);
	    mpath_device && mpath_device != old;
	    mpath_device = mpath_next_dev(mpath_head, mpath_device)) {
		enum mpath_access_state access_state;

		if (mpath_path_is_disabled(mpath_head, mpath_device))
			continue;
		access_state = mpath_head->mpdt->get_access_state(mpath_device);
		if (access_state == MPATH_STATE_OPTIMIZED) {
			found = mpath_device;
			goto out;
		}
		if (access_state == MPATH_STATE_ACTIVE)
			found = mpath_device;
	}

	/*
	 * The loop above skips the current path for round-robin semantics.
	 * Fall back to the current path if either:
	 *  - no other optimized path found and current is optimized,
	 *  - no other usable path found and current is usable.
	 */
	access_state_old = mpath_head->mpdt->get_access_state(old);
	if (!mpath_path_is_disabled(mpath_head, old) &&
	    (access_state_old == MPATH_STATE_OPTIMIZED ||
	    (!found && access_state_old == MPATH_STATE_ACTIVE)))
		return old;

	if (!found)
		return NULL;
out:
	rcu_assign_pointer(mpath_head->current_path[node], found);

	return found;
}

static struct mpath_device *mpath_queue_depth_path(struct mpath_head *mpath_head)
{
	struct mpath_device *best_opt = NULL, *mpath_device;
	struct mpath_device *best_nonopt = NULL;
	unsigned int min_depth_opt = UINT_MAX, min_depth_nonopt = UINT_MAX;
	unsigned int depth;

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {

		if (mpath_path_is_disabled(mpath_head, mpath_device))
			continue;

		depth = atomic_read(&mpath_device->nr_active);

		switch (mpath_head->mpdt->get_access_state(mpath_device)) {
		case MPATH_STATE_OPTIMIZED:
			if (depth < min_depth_opt) {
				min_depth_opt = depth;
				best_opt = mpath_device;
			}
			break;
		case MPATH_STATE_ACTIVE:
			if (depth < min_depth_nonopt) {
				min_depth_nonopt = depth;
				best_nonopt = mpath_device;
			}
			break;
		default:
			break;
		}

		if (min_depth_opt == 0)
			return best_opt;
	}

	return best_opt ? best_opt : best_nonopt;
}

static inline bool mpath_path_is_optimized(struct mpath_head *mpath_head,
					struct mpath_device *mpath_device)
{
	return mpath_head->mpdt->is_optimized(mpath_device);
}

static struct mpath_device *mpath_numa_path(struct mpath_head *mpath_head,
					enum mpath_iopolicy_e iopolicy)
{
	int node = numa_node_id();
	struct mpath_device *mpath_device;

	mpath_device = srcu_dereference(mpath_head->current_path[node],
					&mpath_head->srcu);
	if (unlikely(!mpath_device))
		return __mpath_find_path(mpath_head, iopolicy, node);
	if (unlikely(!mpath_path_is_optimized(mpath_head, mpath_device)))
		return __mpath_find_path(mpath_head, iopolicy, node);
	return mpath_device;
}

static struct mpath_device *mpath_find_path(struct mpath_head *mpath_head)
{
	enum mpath_iopolicy_e iopolicy =
			mpath_head->mpdt->get_iopolicy(mpath_head);

	switch (iopolicy) {
	case MPATH_IOPOLICY_QD:
		return mpath_queue_depth_path(mpath_head);
	case MPATH_IOPOLICY_RR:
		return mpath_round_robin_path(mpath_head, iopolicy);
	default:
		return mpath_numa_path(mpath_head, iopolicy);
	}
}

static bool mpath_available_path(struct mpath_head *mpath_head)
{
	struct mpath_device *mpath_device;

	if (!test_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags))
		return false;

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {
		bool available = false;

		if (!mpath_head->mpdt->available_path(mpath_device,
				&available))
			continue;
		if (available)
			return true;
	}

	/*
	 * If "mpahead->delayed_removal_secs" is configured (i.e., non-zero), do
	 * not immediately fail I/O. Instead, requeue the I/O for the configured
	 * duration, anticipating that if there's a transient link failure then
	 * it may recover within this time window. This parameter is exported to
	 * userspace via sysfs, and its default value is zero. It is internally
	 * mapped to MPATH_HEAD_QUEUE_IF_NO_PATH. When delayed_removal_secs is
	 * non-zero, this flag is set to true. When zero, the flag is cleared.
	 */
	return mpath_head_queue_if_no_path(mpath_head);

}

static void mpath_bdev_submit_bio(struct bio *bio)
{
	struct mpath_disk *mpath_disk = bio->bi_bdev->bd_disk->private_data;
	struct mpath_head *mpath_head = mpath_disk->mpath_head;
	struct device *dev = mpath_disk->parent;
	struct mpath_device *mpath_device;
	int srcu_idx;

	bio = bio_split_to_limits(bio);
	if (!bio)
		return;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);

	if (likely(mpath_device)) {
		bio->bi_opf |= REQ_MPATH;
		if (mpath_head->mpdt->clone_bio)
			bio = mpath_head->mpdt->clone_bio(bio);
		trace_block_bio_remap(bio, disk_devt(mpath_device->disk),
				      bio->bi_iter.bi_sector);
		bio_set_dev(bio, mpath_device->disk->part0);

		submit_bio_noacct(bio);
	} else if (mpath_available_path(mpath_head)) {
		dev_warn_ratelimited(dev, "no usable path - requeuing I/O\n");

		spin_lock_irq(&mpath_head->requeue_lock);
		bio_list_add(&mpath_head->requeue_list, bio);
		spin_unlock_irq(&mpath_head->requeue_lock);
	} else {
		dev_warn_ratelimited(dev, "no available path - failing I/O\n");

		bio_io_error(bio);
	}

	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
}

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

static int mpath_bdev_ioctl(struct block_device *bdev, blk_mode_t mode,
		    unsigned int cmd, unsigned long arg)
{
	struct gendisk *disk = bdev->bd_disk;
	struct mpath_disk *mpath_disk = mpath_gendisk_to_disk(disk);
	struct mpath_head *mpath_head = mpath_disk->mpath_head;
	struct mpath_device *mpath_device;
	int srcu_idx, err;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);

	if (!mpath_device) {
		err = -EWOULDBLOCK;
		goto out_unlock;
	}

	if (bdev_is_partition(bdev) && !capable(CAP_SYS_RAWIO)) {
		err = -ENOIOCTLCMD;
		goto out_unlock;
	}

	/* ->ioctl must always unlock */
	err = mpath_head->mpdt->bdev_ioctl(bdev, mpath_device, mode, cmd,
				arg, srcu_idx);
	lockdep_assert_not_held(&mpath_head->srcu);
	return err;

out_unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	return err;
}

void mpath_head_read_unlock(struct mpath_head *mpath_head, int srcu_idx)
__releases(&mpath_head->srcu)
{
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
}
EXPORT_SYMBOL_GPL(mpath_head_read_unlock);

static int mpath_pr_register(struct block_device *bdev, u64 old_key,
			u64 new_key, unsigned int flags)
{
	struct mpath_disk *mpath_disk = dev_get_drvdata(&bdev->bd_device);
	struct mpath_head *mpath_head = mpath_disk->mpath_head;
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (mpath_device)
		ret = mpath_head->mpdt->pr_ops->pr_register(mpath_device,
				old_key, new_key, flags);
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_pr_reserve(struct block_device *bdev, u64 key,
		enum pr_type type, unsigned flags)
{
	struct mpath_disk *mpath_disk = dev_get_drvdata(&bdev->bd_device);
	struct mpath_head *mpath_head = mpath_disk->mpath_head;
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);

	if (mpath_device)
		ret = mpath_head->mpdt->pr_ops->pr_reserve(mpath_device, key,
				type, flags);

	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_pr_release(struct block_device *bdev, u64 key, enum pr_type type)
{
	struct mpath_disk *mpath_disk = dev_get_drvdata(&bdev->bd_device);
	struct mpath_head *mpath_head = mpath_disk->mpath_head;
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);

	if (mpath_device)
		ret = mpath_head->mpdt->pr_ops->pr_release(mpath_device, key,
				type);

	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_pr_preempt(struct block_device *bdev, u64 old, u64 new,
		enum pr_type type, bool abort)
{
	struct mpath_disk *mpath_disk = dev_get_drvdata(&bdev->bd_device);
	struct mpath_head *mpath_head = mpath_disk->mpath_head;
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);

	if (mpath_device)
		ret = mpath_head->mpdt->pr_ops->pr_preempt(mpath_device, old,
				new, type, abort);

	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_pr_clear(struct block_device *bdev, u64 key)
{
	struct mpath_disk *mpath_disk = dev_get_drvdata(&bdev->bd_device);
	struct mpath_head *mpath_head = mpath_disk->mpath_head;
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);

	if (mpath_device)
		ret = mpath_head->mpdt->pr_ops->pr_clear(mpath_device, key);

	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_pr_read_keys(struct block_device *bdev,
		struct pr_keys *keys_info)
{
	struct mpath_disk *mpath_disk = dev_get_drvdata(&bdev->bd_device);
	struct mpath_head *mpath_head = mpath_disk->mpath_head;
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);

	if (mpath_device)
		ret = mpath_head->mpdt->pr_ops->pr_read_keys(mpath_device,
				keys_info);

	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static int mpath_pr_read_reservation(struct block_device *bdev,
		struct pr_held_reservation *resv)
{
	struct mpath_disk *mpath_disk = dev_get_drvdata(&bdev->bd_device);
	struct mpath_head *mpath_head = mpath_disk->mpath_head;
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);

	if (mpath_device)
		ret = mpath_head->mpdt->pr_ops->pr_read_reservation(
				mpath_device, resv);

	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return ret;
}

static const struct pr_ops mpath_pr_ops = {
	.pr_register	= mpath_pr_register,
	.pr_reserve	= mpath_pr_reserve,
	.pr_release	= mpath_pr_release,
	.pr_preempt	= mpath_pr_preempt,
	.pr_clear	= mpath_pr_clear,
	.pr_read_keys	= mpath_pr_read_keys,
	.pr_read_reservation = mpath_pr_read_reservation,
};

const struct block_device_operations mpath_ops = {
	.owner          = THIS_MODULE,
	.open		= mpath_bdev_open,
	.release	= mpath_bdev_release,
	.submit_bio	= mpath_bdev_submit_bio,
	.ioctl		= mpath_bdev_ioctl,
	.compat_ioctl	= blkdev_compat_ptr_ioctl,
	.report_zones	= mpath_bdev_report_zones,
	.pr_ops		= &mpath_pr_ops,
};
EXPORT_SYMBOL_GPL(mpath_ops);

static int mpath_generic_chr_open(struct inode *inode, struct file *file)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct mpath_head *mpath_head =
			container_of(cdev, struct mpath_head, cdev);

	return mpath_get_head(mpath_head);
}

static int mpath_generic_chr_release(struct inode *inode, struct file *file)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct mpath_head *mpath_head =
			container_of(cdev, struct mpath_head, cdev);

	mpath_put_head(mpath_head);
	return 0;
}

static long mpath_generic_chr_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct cdev *cdev = file_inode(file)->i_cdev;
	struct mpath_head *mpath_head =
			container_of(cdev, struct mpath_head, cdev);
	struct mpath_device *mpath_device;
	fmode_t mode = file->f_mode;
	int srcu_idx, err = -EWOULDBLOCK;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);
	if (!mpath_device)
		goto out_unlock;

	/*
	 * If we are in the middle of error recovery, don't let anyone
	 * else try and use this device.  Also, if error recovery fails, it
	 * may try and take the device offline, in which case all further
	 * access to the device is prohibited.
	 */
	err = mpath_head->mpdt->cdev_ioctl(mpath_head, mpath_device,
			mode, cmd, arg, srcu_idx);
	lockdep_assert_not_held(&mpath_head->srcu);
	return err;// ioctl must unlock

out_unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	return err;
}

static int mpath_generic_chr_uring_cmd(struct io_uring_cmd *ioucmd,
		unsigned int issue_flags)
{
	struct cdev *cdev = file_inode(ioucmd->file)->i_cdev;
	struct mpath_head *mpath_head =
			container_of(cdev, struct mpath_head, cdev);
	struct mpath_device *mpath_device;
	int srcu_idx, ret = -EWOULDBLOCK;

	if (!mpath_head->mpdt->chr_uring_cmd)
		return -EOPNOTSUPP;

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	mpath_device = mpath_find_path(mpath_head);

	if (!mpath_device)
		goto out_unlock;

	ret = mpath_head->mpdt->chr_uring_cmd(mpath_device, ioucmd,
			issue_flags);
out_unlock:
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	return ret;
}

static int mpath_generic_chr_uring_cmd_iopoll(struct io_uring_cmd *ioucmd,
				 struct io_comp_batch *iob,
				 unsigned int poll_flags)
{
	struct cdev *cdev = file_inode(ioucmd->file)->i_cdev;
	struct mpath_head *mpath_head =
			container_of(cdev, struct mpath_head, cdev);

	if (!mpath_head->mpdt->chr_uring_cmd_iopoll)
		return -EOPNOTSUPP;

	return mpath_head->mpdt->chr_uring_cmd_iopoll(ioucmd, iob, poll_flags);
}

const struct file_operations mpath_generic_chr_fops = {
	.owner		= THIS_MODULE,
	.open		= mpath_generic_chr_open,
	.release	= mpath_generic_chr_release,
	.unlocked_ioctl	= mpath_generic_chr_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
	.uring_cmd	= mpath_generic_chr_uring_cmd,
	.uring_cmd_iopoll = mpath_generic_chr_uring_cmd_iopoll,
};
EXPORT_SYMBOL_GPL(mpath_generic_chr_fops);

static int mpath_head_add_cdev(struct mpath_head *mpath_head)
{
	if (mpath_head->mpdt->add_cdev)
		return mpath_head->mpdt->add_cdev(mpath_head);
	return 0;
}

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

void mpath_requeue_work(struct work_struct *work)
{
	struct mpath_head *mpath_head =
	    container_of(work, struct mpath_head, requeue_work);
	struct bio *bio, *next;

	spin_lock_irq(&mpath_head->requeue_lock);
	next = bio_list_get(&mpath_head->requeue_list);
	spin_unlock_irq(&mpath_head->requeue_lock);

	while ((bio = next) != NULL) {
		next = bio->bi_next;
		bio->bi_next = NULL;
		submit_bio_noacct(bio);
	}
}
EXPORT_SYMBOL_GPL(mpath_requeue_work);

static void mpath_head_del_cdev(struct mpath_head *mpath_head)
{
	if (mpath_head->mpdt->del_cdev)
		mpath_head->mpdt->del_cdev(mpath_head);
}

bool mpath_can_remove_head(struct mpath_head *mpath_head)
{
	bool remove = false;

	mutex_lock(&mpath_head->lock);
	/*
	 * Ensure that no one could remove this module while the head
	 * remove work is pending.
	 */
	if (mpath_head_queue_if_no_path(mpath_head) &&
		try_module_get(mpath_head->drv_module)) {

		mod_delayed_work(mpath_wq, &mpath_head->remove_work,
				mpath_head->delayed_removal_secs * HZ);
	} else {
		remove = true;
	}

	mutex_unlock(&mpath_head->lock);
	return remove;
}
EXPORT_SYMBOL_GPL(mpath_can_remove_head);

void mpath_remove_disk(struct mpath_disk *mpath_disk)
{
	struct mpath_head *mpath_head = mpath_disk->mpath_head;

	if (test_and_clear_bit(MPATH_HEAD_DISK_LIVE, &mpath_head->flags)) {
		struct gendisk *disk = mpath_disk->disk;

		/*
		 * requeue I/O after MPATH_HEAD_DISK_LIVE has been cleared
		 * to allow multipath to fail all I/O.
		 */
		kblockd_schedule_work(&mpath_head->requeue_work);

		mpath_head_del_cdev(mpath_head);
		mpath_synchronize(mpath_head);
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

		mpath_head_add_cdev(mpath_head);
		queue_work(mpath_wq, &mpath_disk->partition_scan_work);
	}

	mpath_add_sysfs_link(mpath_disk);

	mutex_lock(&mpath_head->lock);
	if (mpath_path_is_optimized(mpath_head, mpath_device)) {
		int node, srcu_idx;

		srcu_idx = srcu_read_lock(&mpath_head->srcu);
		for_each_online_node(node)
			__mpath_find_path(mpath_head,
				mpath_head->mpdt->get_iopolicy(mpath_head),
				node);
		srcu_read_unlock(&mpath_head->srcu, srcu_idx);
	}
	mutex_unlock(&mpath_head->lock);

	mpath_synchronize(mpath_head);
	kblockd_schedule_work(&mpath_head->requeue_work);
}
EXPORT_SYMBOL_GPL(mpath_device_set_live);

static struct attribute dummy_attr = {
	.name = "dummy",
};

static struct attribute *mpath_attrs[] = {
	&dummy_attr,
	NULL
};

static bool multipath_sysfs_group_visible(struct kobject *kobj)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct gendisk *disk = dev_to_disk(dev);

	return is_mpath_head(disk);
}

static bool multipath_sysfs_attr_visible(struct kobject *kobj,
		struct attribute *attr, int n)
{
	return false;
}

DEFINE_SYSFS_GROUP_VISIBLE(multipath_sysfs)

const struct attribute_group mpath_attr_group = {
	.name           = "multipath",
	.attrs		= mpath_attrs,
	.is_visible     = SYSFS_GROUP_VISIBLE(multipath_sysfs),
};
EXPORT_SYMBOL_GPL(mpath_attr_group);

const struct attribute_group *mpath_device_groups[] = {
	&mpath_attr_group,
	NULL
};
EXPORT_SYMBOL_GPL(mpath_device_groups);

ssize_t mpath_iopolicy_show(struct mpath_iopolicy *mpath_iopolicy, char *buf)
{
	return sysfs_emit(buf, "%s\n",
		mpath_iopolicy_names[mpath_read_iopolicy(mpath_iopolicy)]);
}
EXPORT_SYMBOL_GPL(mpath_iopolicy_show);

static void mpath_iopolicy_update(struct mpath_iopolicy *mpath_iopolicy,
		int iopolicy, void (*update)(void *), void *data)
{
	int old_iopolicy = READ_ONCE(mpath_iopolicy->iopolicy);

	if (old_iopolicy == iopolicy)
		return;

	WRITE_ONCE(mpath_iopolicy->iopolicy, iopolicy);

	/*
	 * iopolicy changes clear the mpath by design, which @update
	 * must do.
	 */
	update(data);

	pr_err("iopolicy changed from %s to %s\n",
		mpath_iopolicy_names[old_iopolicy],
		mpath_iopolicy_names[iopolicy]);
}

ssize_t mpath_iopolicy_store(struct mpath_iopolicy *mpath_iopolicy,
				const char *buf, size_t count,
				void (*update)(void *), void *data)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mpath_iopolicy_names); i++) {
		if (sysfs_streq(buf, mpath_iopolicy_names[i])) {
			mpath_iopolicy_update(mpath_iopolicy, i, update, data);
			return count;
		}
	}

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(mpath_iopolicy_store);

ssize_t mpath_numa_nodes_show(struct mpath_head *mpath_head,
			struct mpath_device *mpath_device,
			struct mpath_iopolicy *mpath_iopolicy, char *buf)
{
	int node, srcu_idx;
	nodemask_t numa_nodes;
	struct mpath_device *current_mpath_dev;

	if (mpath_read_iopolicy(mpath_iopolicy) != MPATH_IOPOLICY_NUMA)
		return 0;

	nodes_clear(numa_nodes);

	srcu_idx = srcu_read_lock(&mpath_head->srcu);
	for_each_node(node) {
		current_mpath_dev =
			srcu_dereference(mpath_head->current_path[node],
				&mpath_head->srcu);
		if (current_mpath_dev == mpath_device)
			node_set(node, numa_nodes);
	}
	srcu_read_unlock(&mpath_head->srcu, srcu_idx);

	return sysfs_emit(buf, "%*pbl\n", nodemask_pr_args(&numa_nodes));
}
EXPORT_SYMBOL_GPL(mpath_numa_nodes_show);

ssize_t mpath_delayed_removal_secs_show(struct mpath_head *mpath_head,
					char *buf)
{
	int ret;

	mutex_lock(&mpath_head->lock);
	ret = sysfs_emit(buf, "%u\n", mpath_head->delayed_removal_secs);
	mutex_unlock(&mpath_head->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(mpath_delayed_removal_secs_show);

ssize_t mpath_delayed_removal_secs_store(struct mpath_head *mpath_head,
			const char *buf, size_t count)
{
	ssize_t ret;
	int sec;

	ret = kstrtouint(buf, 0, &sec);
	if (ret < 0)
		return ret;

	mutex_lock(&mpath_head->lock);
	mpath_head->delayed_removal_secs = sec;
	if (sec)
		set_bit(MPATH_HEAD_QUEUE_IF_NO_PATH, &mpath_head->flags);
	else
		clear_bit(MPATH_HEAD_QUEUE_IF_NO_PATH, &mpath_head->flags);
	mutex_unlock(&mpath_head->lock);

	/*
	 * Ensure that update to MPATH_HEAD_QUEUE_IF_NO_PATH is seen
	 * by its reader.
	 */
	mpath_synchronize(mpath_head);

	return count;
}
EXPORT_SYMBOL_GPL(mpath_delayed_removal_secs_store);

void mpath_add_sysfs_link(struct mpath_disk *mpath_disk)
{
	struct mpath_head *mpath_head = mpath_disk->mpath_head;
	struct device *target;
	struct device *source;
	int rc, srcu_idx;
	struct kobject *mpath_gd_kobj;
	struct mpath_device *mpath_device;

	/*
	 * Ensure head disk node is already added otherwise we may get invalid
	 * kobj for head disk node
	 */
	if (!test_bit(GD_ADDED, &mpath_disk->disk->state))
		return;

	mpath_gd_kobj = &disk_to_dev(mpath_disk->disk)->kobj;
	srcu_idx = srcu_read_lock(&mpath_head->srcu);

	list_for_each_entry_srcu(mpath_device, &mpath_head->dev_list, siblings,
				 srcu_read_lock_held(&mpath_head->srcu)) {
		if (!test_bit(GD_ADDED, &mpath_device->disk->state))
			continue;

		if (test_and_set_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags))
			continue;

		target = disk_to_dev(mpath_device->disk);
		source = disk_to_dev(mpath_disk->disk);
		/*
		 * Create sysfs link from head gendisk kobject @kobj to the
		 * ns path gendisk kobject @target->kobj.
		 */
		rc = sysfs_add_link_to_group(mpath_gd_kobj, "multipath",
				&target->kobj, dev_name(target));

		if (unlikely(rc)) {
			dev_err(disk_to_dev(mpath_disk->disk),
					"failed to create link to %s rc=%d\n",
					dev_name(target), rc);
			clear_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags);
		} else {
			dev_info(source, "Created multipath sysfs link to %s\n",
					mpath_device->disk->disk_name);
		}
	}

	srcu_read_unlock(&mpath_head->srcu, srcu_idx);
}
EXPORT_SYMBOL_GPL(mpath_add_sysfs_link);

void mpath_remove_sysfs_link(struct mpath_disk *mpath_disk,
				struct mpath_device *mpath_device)
{
	struct device *target;
	struct kobject *mpath_gd_kobj;

	if (!test_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags))
		return;

	target = disk_to_dev(mpath_device->disk);
	mpath_gd_kobj = &disk_to_dev(mpath_disk->disk)->kobj;

	sysfs_remove_link_from_group(mpath_gd_kobj, "multipath",
			dev_name(target));

	clear_bit(MPATH_DEVICE_SYSFS_ATTR_LINK, &mpath_device->flags);
}
EXPORT_SYMBOL_GPL(mpath_remove_sysfs_link);

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

	mpath_head->delayed_removal_secs = 0;

	INIT_WORK(&mpath_head->requeue_work, mpath_requeue_work);
	spin_lock_init(&mpath_head->requeue_lock);
	bio_list_init(&mpath_head->requeue_list);

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
