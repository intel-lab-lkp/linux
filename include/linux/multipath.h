
#ifndef _LIBMULTIPATH_H
#define _LIBMULTIPATH_H

#include <linux/blkdev.h>
#include <linux/srcu.h>

extern const struct block_device_operations mpath_ops;

struct mpath_disk {
	struct gendisk		*disk;
	struct kref		ref;
	struct work_struct	partition_scan_work;
	struct mutex		lock;
	struct mpath_head	*mpath_head;
	struct device		*parent;
};

struct mpath_device {
	struct list_head	siblings;
	struct gendisk		*disk;
};

struct mpath_head_template {
	const struct attribute_group **device_groups;
};

#define MPATH_HEAD_DISK_LIVE 			0

struct mpath_head {
	struct srcu_struct	srcu;
	struct list_head	dev_list;	/* list of all mpath_devs */
	struct mutex		lock;

	struct kref		ref;

	unsigned long		flags;
	struct mpath_device __rcu 		*current_path[MAX_NUMNODES];
	const struct mpath_head_template	*mpdt;
	void			*drvdata;
};

static inline struct mpath_disk *mpath_bd_device_to_disk(struct device *dev)
{
	return dev_get_drvdata(dev);
}

static inline struct mpath_disk *mpath_gendisk_to_disk(struct gendisk *disk)
{
	return mpath_bd_device_to_disk(disk_to_dev(disk));
}

int mpath_get_head(struct mpath_head *mpath_head);
void mpath_put_head(struct mpath_head *mpath_head);
struct mpath_head *mpath_alloc_head(void);
void mpath_put_disk(struct mpath_disk *mpath_disk);
void mpath_remove_disk(struct mpath_disk *mpath_disk);
void mpath_unregister_disk(struct mpath_disk *mpath_disk);
struct mpath_disk *mpath_alloc_head_disk(struct queue_limits *lim,
			int numa_node);
void mpath_device_set_live(struct mpath_disk *mpath_disk,
			struct mpath_device *mpath_device);
void mpath_unregister_disk(struct mpath_disk *mpath_disk);

static inline bool is_mpath_head(struct gendisk *disk)
{
	return disk->fops == &mpath_ops;
}
#endif // _LIBMULTIPATH_H
