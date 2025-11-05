/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_NOTIFY_FAILURE_H__
#define __XFS_NOTIFY_FAILURE_H__

extern const struct dax_holder_operations xfs_dax_holder_operations;

enum xfs_failed_device {
	XFS_FAILED_DATADEV,
	XFS_FAILED_LOGDEV,
	XFS_FAILED_RTDEV,
};

#if defined(CONFIG_XFS_LIVE_HOOKS) && defined(CONFIG_MEMORY_FAILURE) && defined(CONFIG_FS_DAX)
struct xfs_media_error_params {
	struct xfs_mount		*mp;
	enum xfs_failed_device		fdev;
	xfs_daddr_t			daddr;
	uint64_t			bbcount;
	bool				pre_remove;
};

struct xfs_media_error_hook {
	struct xfs_hook			error_hook;
};

int xfs_media_error_hook_add(struct xfs_mount *mp,
		struct xfs_media_error_hook *hook);
void xfs_media_error_hook_del(struct xfs_mount *mp,
		struct xfs_media_error_hook *hook);
void xfs_media_error_hook_setup(struct xfs_media_error_hook *hook,
		notifier_fn_t mod_fn);
#else
struct xfs_media_error_params { };
struct xfs_media_error_hook { };
# define xfs_media_error_hook_add(...)		(0)
# define xfs_media_error_hook_del(...)		((void)0)
# define xfs_media_error_hook_setup(...)	((void)0)
#endif /* CONFIG_XFS_LIVE_HOOKS */

#endif /* __XFS_NOTIFY_FAILURE_H__ */
