/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2009 IBM Corporation
 * Author: Mimi Zohar <zohar@us.ibm.com>
 */

#ifndef _LINUX_INTEGRITY_H
#define _LINUX_INTEGRITY_H

#include <linux/fs.h>
#include <linux/iversion.h>

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
	u64 ctime_guard;
	u64 version;		/* track inode changes */
	unsigned long ino;
	dev_t dev;
};

static inline u64 integrity_ctime_guard(struct kstat stat)
{
	return stat.ctime.tv_sec ^ stat.ctime.tv_nsec;
}

/*
 * On stacked filesystems the i_version alone is not enough to detect file data
 * or metadata change. Additional metadata is required.
 */
static inline void
integrity_inode_attrs_store(struct integrity_inode_attributes *attrs,
			    u64 i_version, u64 ctime_guard,
			    const struct inode *inode)
{
	attrs->ctime_guard = ctime_guard;
	attrs->version = i_version;
	attrs->dev = inode->i_sb->s_dev;
	attrs->ino = inode->i_ino;
}

/*
 * On stacked filesystems detect whether the inode or its content has changed.
 */
static inline bool
integrity_inode_attrs_changed(const struct integrity_inode_attributes *attrs,
			      struct file *file, struct inode *inode)
{
	struct kstat stat;

	if (inode->i_sb->s_dev != attrs->dev ||
	    inode->i_ino != attrs->ino)
		return true;

	if (inode_eq_iversion(inode, attrs->version))
		return false;

	if (!file || vfs_getattr_nosec(&file->f_path, &stat, STATX_CTIME,
				       AT_STATX_SYNC_AS_STAT))
		return true;

	return attrs->ctime_guard != integrity_ctime_guard(stat);
}


#endif /* _LINUX_INTEGRITY_H */
