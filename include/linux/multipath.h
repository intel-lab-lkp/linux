
#ifndef _LIBMULTIPATH_H
#define _LIBMULTIPATH_H

#include <linux/blkdev.h>
#include <linux/srcu.h>

struct mpath_device {
	struct list_head	siblings;
	struct gendisk		*disk;
};

struct mpath_head {
	struct srcu_struct	srcu;
	struct list_head	dev_list;	/* list of all mpath_devs */
	struct mutex		lock;

	struct kref		ref;

	struct mpath_device __rcu 		*current_path[MAX_NUMNODES];
	void			*drvdata;
};

int mpath_get_head(struct mpath_head *mpath_head);
void mpath_put_head(struct mpath_head *mpath_head);
struct mpath_head *mpath_alloc_head(void);

#endif // _LIBMULTIPATH_H
