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
