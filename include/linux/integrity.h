/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2009 IBM Corporation
 * Author: Mimi Zohar <zohar@us.ibm.com>
 */

#ifndef _LINUX_INTEGRITY_H
#define _LINUX_INTEGRITY_H

#include <linux/fs.h>
#include <linux/iversion.h>
#include <linux/kernel.h>

enum integrity_status {
	INTEGRITY_PASS = 0,
	INTEGRITY_PASS_IMMUTABLE,
	INTEGRITY_FAIL,
	INTEGRITY_FAIL_IMMUTABLE,
	INTEGRITY_NOLABEL,
	INTEGRITY_NOXATTRS,
	INTEGRITY_UNKNOWN,
};

#ifdef CONFIG_INTEGRITY
extern void __init integrity_load_keys(void);

#else
static inline void integrity_load_keys(void)
{
}
#endif /* CONFIG_INTEGRITY */

/* An inode's attributes for detection of changes */
struct integrity_inode_attributes {
	u64 version;		/* track inode changes */
	unsigned long ino;
	dev_t dev;
};

/*
 * On stacked filesystems the i_version alone is not enough to detect file data
 * or metadata change. Additional metadata is required.
 */
static inline void
integrity_inode_attrs_store(struct integrity_inode_attributes *attrs,
			    u64 i_version, const struct inode *inode)
{
	attrs->version = i_version;
	attrs->dev = inode->i_sb->s_dev;
	attrs->ino = inode->i_ino;
}

/* Compares stat attributes for change detection. */
static inline bool
integrity_inode_attrs_stat_changed
(const struct integrity_inode_attributes *attrs, const struct kstat *stat)
{
	if (stat->result_mask & STATX_CHANGE_COOKIE)
		return stat->change_cookie != attrs->version;

	return true;
}

/*
 * On stacked filesystems detect whether the inode or its content has changed.
 *
 * Must be called in process context.
 */
static inline bool
integrity_inode_attrs_changed(const struct integrity_inode_attributes *attrs,
			      struct file *file, struct inode *inode)
{
	struct kstat stat;

	might_sleep();

	if (inode->i_sb->s_dev != attrs->dev || inode->i_ino != attrs->ino)
		return true;

	/*
	 * EVM currently relies on backing inode i_version. While IS_I_VERSION
	 * is not a good indicator of i_version support, this still retains
	 * the logic such that a re-evaluation should still occur for EVM, and
	 * only for IMA if vfs_getattr_nosec() fails.
	 */
	if (!file || vfs_getattr_nosec(&file->f_path, &stat,
				       STATX_CHANGE_COOKIE,
				       AT_STATX_SYNC_AS_STAT))
		return !IS_I_VERSION(inode) ||
		       !inode_eq_iversion(inode, attrs->version);

	return integrity_inode_attrs_stat_changed(attrs, &stat);
}


#endif /* _LINUX_INTEGRITY_H */
