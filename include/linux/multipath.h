
#ifndef _LIBMULTIPATH_H
#define _LIBMULTIPATH_H

#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/cdev.h>
#include <linux/pr.h>
#include <linux/srcu.h>
#include <linux/io_uring/cmd.h>

extern const struct file_operations mpath_generic_chr_fops;
extern const struct block_device_operations mpath_ops;
extern const struct attribute_group mpath_attr_group;
extern const struct attribute_group *mpath_device_groups[];

enum mpath_iopolicy_e {
	MPATH_IOPOLICY_NUMA,
	MPATH_IOPOLICY_RR,
	MPATH_IOPOLICY_QD,
};

struct mpath_iopolicy {
	enum mpath_iopolicy_e	iopolicy;
};

enum mpath_access_state {
	MPATH_STATE_OPTIMIZED,
	MPATH_STATE_ACTIVE,
	MPATH_STATE_INVALID	= 0xFF
};

struct mpath_disk {
	struct gendisk		*disk;
	struct kref		ref;
	struct work_struct	partition_scan_work;
	struct mutex		lock;
	struct mpath_head	*mpath_head;
	struct device		*parent;
};

#define MPATH_DEVICE_SYSFS_ATTR_LINK      0

struct mpath_device {
	struct list_head	siblings;
	atomic_t		nr_active;
	struct gendisk		*disk;
	unsigned long		flags;
	int			numa_node;
};

struct mpath_pr_ops {
	int (*pr_register)(struct mpath_device *mpath_device, u64 old_key,
			u64 new_key, u32 flags);
	int (*pr_reserve)(struct mpath_device *mpath_device, u64 key,
			enum pr_type type, u32 flags);
	int (*pr_release)(struct mpath_device *mpath_device, u64 key,
			enum pr_type type);
	int (*pr_preempt)(struct mpath_device *mpath_device, u64 old_key,
			u64 new_key, enum pr_type type, bool abort);
	int (*pr_clear)(struct mpath_device *mpath_device, u64 key);
	int (*pr_read_keys)(struct mpath_device *mpath_device,
			struct pr_keys *keys_info);
	int (*pr_read_reservation)(struct mpath_device *mpath_device,
			struct pr_held_reservation *rsv);
};

struct mpath_head_template {
	bool (*available_path)(struct mpath_device *, bool *);
	int (*add_cdev)(struct mpath_head *);
	void (*del_cdev)(struct mpath_head *);
	bool (*is_disabled)(struct mpath_device *);
	bool (*is_optimized)(struct mpath_device *);
	enum mpath_access_state (*get_access_state)(struct mpath_device *);
	int (*bdev_ioctl)(struct block_device *bdev, struct mpath_device *,
			blk_mode_t mode, unsigned int cmd, unsigned long arg,
			int srcu_idx);
	int (*cdev_ioctl)(struct mpath_head *, struct mpath_device *,
			blk_mode_t mode, unsigned int cmd, unsigned long arg, int srcu_idx);
	int (*report_zones)(struct mpath_device *, sector_t sector,
		unsigned int nr_zones, struct blk_report_zones_args *args);
	int (*chr_uring_cmd)(struct mpath_device *, struct io_uring_cmd *ioucmd,
		unsigned int issue_flags);
	int (*chr_uring_cmd_iopoll)(struct io_uring_cmd *ioucmd,
				 struct io_comp_batch *iob,
				 unsigned int poll_flags);
	enum mpath_iopolicy_e (*get_iopolicy)(struct mpath_head *);
	struct bio *(*clone_bio)(struct bio *);
	const struct mpath_pr_ops *pr_ops;
	const struct attribute_group **device_groups;
};

#define MPATH_HEAD_DISK_LIVE 			0
#define MPATH_HEAD_QUEUE_IF_NO_PATH		1

struct mpath_head {
	struct srcu_struct	srcu;
	struct list_head	dev_list;	/* list of all mpath_devs */
	struct mutex		lock;

	struct kref		ref;

	struct bio_list		requeue_list; /* list for requeing bio */
	spinlock_t		requeue_lock;
	struct work_struct	requeue_work; /* work struct for requeue */

	struct cdev		cdev;
	struct device		cdev_device;

	struct delayed_work	remove_work;
	unsigned int		delayed_removal_secs;
	struct module		*drv_module;

	unsigned long		flags;
	struct mpath_device __rcu 		*current_path[MAX_NUMNODES];
	const struct mpath_head_template	*mpdt;
	void			*drvdata;
};

#define REQ_MPATH		REQ_DRV

static inline bool is_mpath_request(struct request *req)
{
	return req->cmd_flags & REQ_MPATH;
}

static inline struct mpath_disk *mpath_bd_device_to_disk(struct device *dev)
{
	return dev_get_drvdata(dev);
}

static inline struct mpath_disk *mpath_gendisk_to_disk(struct gendisk *disk)
{
	return mpath_bd_device_to_disk(disk_to_dev(disk));
}

static inline enum mpath_iopolicy_e mpath_read_iopolicy(
			struct mpath_iopolicy *mpath_iopolicy)
{
	return READ_ONCE(mpath_iopolicy->iopolicy);
}
void mpath_synchronize(struct mpath_head *mpath_head);
int mpath_set_iopolicy(const char *val, int *iopolicy);
int mpath_get_iopolicy(char *buf, int iopolicy);
bool mpath_clear_current_path(struct mpath_head *mpath_head,
			struct mpath_device *mpath_device);
void mpath_synchronize(struct mpath_head *mpath_head);
void mpath_add_device(struct mpath_head *mpath_head,
			struct mpath_device *mpath_device);
void mpath_delete_device(struct mpath_head *mpath_head,
			struct mpath_device *mpath_device);
int mpath_call_for_device(struct mpath_head *mpath_head,
			int (*cb)(struct mpath_device *mpath_device));
void mpath_clear_paths(struct mpath_head *mpath_head);
void mpath_revalidate_paths(struct mpath_disk *mpath_disk,
	void (*cb)(struct mpath_device *mpath_device, sector_t capacity));
void mpath_add_sysfs_link(struct mpath_disk *mpath_disk);
void mpath_remove_sysfs_link(struct mpath_disk *mpath_disk,
				struct mpath_device *mpath_device);
void mpath_head_read_unlock(struct mpath_head *mpath_head, int srcu_idx);
int mpath_get_head(struct mpath_head *mpath_head);
void mpath_put_head(struct mpath_head *mpath_head);
void mpath_requeue_work(struct work_struct *work);
struct mpath_head *mpath_alloc_head(void);
void mpath_put_disk(struct mpath_disk *mpath_disk);
bool mpath_can_remove_head(struct mpath_head *mpath_head);
void mpath_remove_disk(struct mpath_disk *mpath_disk);
void mpath_unregister_disk(struct mpath_disk *mpath_disk);
struct mpath_disk *mpath_alloc_head_disk(struct queue_limits *lim,
			int numa_node);
void mpath_device_set_live(struct mpath_disk *mpath_disk,
			struct mpath_device *mpath_device);
void mpath_unregister_disk(struct mpath_disk *mpath_disk);
ssize_t mpath_numa_nodes_show(struct mpath_head *mpath_head,
			struct mpath_device *mpath_device,
			struct mpath_iopolicy *iopolicy, char *buf);
ssize_t mpath_iopolicy_show(struct mpath_iopolicy *mpath_iopolicy, char *buf);
ssize_t mpath_iopolicy_store(struct mpath_iopolicy *mpath_iopolicy,
			const char *buf, size_t count,
			void (*update)(void *data), void *);
ssize_t mpath_delayed_removal_secs_show(struct mpath_head *mpath_head,
			char *buf);
ssize_t mpath_delayed_removal_secs_store(struct mpath_head *mpath_head,
			const char *buf, size_t count);

static inline bool is_mpath_head(struct gendisk *disk)
{
	return disk->fops == &mpath_ops;
}

static inline bool mpath_qd_iopolicy(struct mpath_iopolicy *mpath_iopolicy)
{
	return mpath_read_iopolicy(mpath_iopolicy) == MPATH_IOPOLICY_QD;
}

static inline bool mpath_head_queue_if_no_path(struct mpath_head *mpath_head)
{
	if (test_bit(MPATH_HEAD_QUEUE_IF_NO_PATH, &mpath_head->flags))
		return true;
	return false;
}

#endif // _LIBMULTIPATH_H
