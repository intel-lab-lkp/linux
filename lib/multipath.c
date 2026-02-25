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


void mpath_synchronize(struct mpath_head *mpath_head)
{
	synchronize_srcu(&mpath_head->srcu);
}
EXPORT_SYMBOL_GPL(mpath_synchronize);

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

	return false;
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

const struct block_device_operations mpath_ops = {
	.owner          = THIS_MODULE,
	.open		= mpath_bdev_open,
	.release	= mpath_bdev_release,
	.submit_bio	= mpath_bdev_submit_bio,
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
		queue_work(mpath_wq, &mpath_disk->partition_scan_work);
	}

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
