// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#ifndef __XFS_FILE_H__
#define __XFS_FILE_H__

extern const struct file_operations xfs_file_operations;
extern const struct file_operations xfs_dir_file_operations;

bool xfs_is_falloc_aligned(struct xfs_inode *ip, loff_t pos,
		long long int len);

enum xfs_file_ioerror_type {
	XFS_FILE_IOERROR_BUFFERED_READ,
	XFS_FILE_IOERROR_BUFFERED_WRITE,
	XFS_FILE_IOERROR_DIRECT_READ,
	XFS_FILE_IOERROR_DIRECT_WRITE,
};

struct xfs_file_ioerror_params {
	xfs_ino_t		ino;
	loff_t			pos;
	u64			len;
	u32			gen;
	int			error;
};

#ifdef CONFIG_XFS_LIVE_HOOKS
struct xfs_file_ioerror_hook {
	struct xfs_hook			ioerror_hook;
};

void xfs_file_ioerror_hook_disable(void);
void xfs_file_ioerror_hook_enable(void);

int xfs_file_ioerror_hook_add(struct xfs_mount *mp,
		struct xfs_file_ioerror_hook *hook);
void xfs_file_ioerror_hook_del(struct xfs_mount *mp,
		struct xfs_file_ioerror_hook *hook);
void xfs_file_ioerror_hook_setup(struct xfs_file_ioerror_hook *hook,
		notifier_fn_t mod_fn);

void xfs_vm_ioerror(struct address_space *mapping, int direction, loff_t pos,
		u64 len, int error);
#else
# define xfs_vm_ioerror			NULL
#endif /* CONFIG_XFS_LIVE_HOOKS */

#endif /* __XFS_FILE_H__ */
