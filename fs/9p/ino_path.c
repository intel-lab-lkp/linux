// SPDX-License-Identifier: GPL-2.0-only
/*
 * Specific operations on the v9fs_ino_path structure.
 *
 * Copyright (C) 2025 by Tingmao Wang <m@maowtm.org>
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/fs.h>
#include <linux/string.h>
#include <linux/dcache.h>

#include <linux/posix_acl.h>
#include <net/9p/9p.h>
#include <net/9p/client.h>
#include "v9fs.h"

/*
 * Must hold rename_sem due to traversing parents.  Caller must hold
 * reference to dentry.
 */
struct v9fs_ino_path *make_ino_path(struct dentry *dentry)
{
	struct v9fs_ino_path *path;
	size_t path_components = 0;
	struct dentry *curr = dentry;
	ssize_t i;

	/* Either read or write lock held is ok */
	lockdep_assert_held(&v9fs_dentry2v9ses(dentry)->rename_sem);
	might_sleep(); /* Allocation below might block */

	rcu_read_lock();

	/* Don't include the root dentry */
	while (curr->d_parent != curr) {
		if (WARN_ON_ONCE(path_components >= SSIZE_MAX)) {
			rcu_read_unlock();
			return NULL;
		}
		path_components++;
		curr = curr->d_parent;
	}

	/*
	 * Allocation can block so don't do it in RCU (and because the
	 * allocation might be large, since name_snapshot leaves space for
	 * inline str, not worth trying GFP_ATOMIC)
	 */
	rcu_read_unlock();

	path = kmalloc(struct_size(path, names, path_components), GFP_KERNEL);
	if (!path)
		return NULL;

	path->nr_components = path_components;
	curr = dentry;

	rcu_read_lock();
	for (i = path_components - 1; i >= 0; i--) {
		take_dentry_name_snapshot(&path->names[i], curr);
		curr = curr->d_parent;
	}
	WARN_ON(curr != curr->d_parent);
	rcu_read_unlock();
	return path;
}

void free_ino_path(struct v9fs_ino_path *path)
{
	if (path) {
		for (size_t i = 0; i < path->nr_components; i++)
			release_dentry_name_snapshot(&path->names[i]);
		kfree(path);
	}
}

/*
 * Must hold rename_sem due to traversing parents.  Returns whether
 * ino_path matches with the path of a v9fs dentry.  This function does
 * not sleep.
 */
bool ino_path_compare(struct v9fs_ino_path *ino_path, struct dentry *dentry)
{
	struct dentry *curr = dentry;
	struct name_snapshot *compare;
	ssize_t i;
	bool ret;

	lockdep_assert_held_read(&v9fs_dentry2v9ses(dentry)->rename_sem);

	rcu_read_lock();
	for (i = ino_path->nr_components - 1; i >= 0; i--) {
		if (curr->d_parent == curr) {
			/* We're supposed to have more components to walk */
			rcu_read_unlock();
			return false;
		}
		compare = &ino_path->names[i];
		if (!d_same_name(curr, curr->d_parent, &compare->name)) {
			rcu_read_unlock();
			return false;
		}
		curr = curr->d_parent;
	}
	/* Comparison fails if dentry is deeper than ino_path */
	ret = (curr == curr->d_parent);
	rcu_read_unlock();
	return ret;
}
