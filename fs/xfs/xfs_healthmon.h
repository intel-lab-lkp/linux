/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2024-2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_HEALTHMON_H__
#define __XFS_HEALTHMON_H__

enum xfs_healthmon_type {
	XFS_HEALTHMON_RUNNING,	/* monitor running */
	XFS_HEALTHMON_LOST,	/* message lost */
	XFS_HEALTHMON_UNMOUNT,	/* filesystem is unmounting */

	/* filesystem shutdown */
	XFS_HEALTHMON_SHUTDOWN,

	/* metadata health events */
	XFS_HEALTHMON_SICK,	/* runtime corruption observed */
	XFS_HEALTHMON_CORRUPT,	/* fsck reported corruption */
	XFS_HEALTHMON_HEALTHY,	/* fsck reported healthy structure */

	/* media errors */
	XFS_HEALTHMON_MEDIA_ERROR,
};

enum xfs_healthmon_domain {
	XFS_HEALTHMON_MOUNT,	/* affects the whole fs */

	/* metadata health events */
	XFS_HEALTHMON_FS,	/* main filesystem metadata */
	XFS_HEALTHMON_AG,	/* allocation group metadata */
	XFS_HEALTHMON_INODE,	/* inode metadata */
	XFS_HEALTHMON_RTGROUP,	/* realtime group metadata */

	/* media errors */
	XFS_HEALTHMON_DATADEV,
	XFS_HEALTHMON_RTDEV,
	XFS_HEALTHMON_LOGDEV,
};

struct xfs_healthmon_event {
	struct xfs_healthmon_event	*next;

	enum xfs_healthmon_type		type;
	enum xfs_healthmon_domain	domain;

	uint64_t			time_ns;

	union {
		/* lost events */
		struct {
			uint64_t	lostcount;
		};
		/* shutdown */
		struct {
			unsigned int	flags;
		};
		/* fs/rt metadata */
		struct {
			/* XFS_SICK_* flags */
			unsigned int	fsmask;
		};
		/* ag/rtgroup metadata */
		struct {
			/* XFS_SICK_* flags */
			unsigned int	grpmask;
			unsigned int	group;
		};
		/* inode metadata */
		struct {
			/* XFS_SICK_INO_* flags */
			unsigned int	imask;
			uint32_t	gen;
			xfs_ino_t	ino;
		};
		/* media errors */
		struct {
			xfs_daddr_t	daddr;
			uint64_t	bbcount;
		};
	};
};

#ifdef CONFIG_XFS_HEALTH_MONITOR
long xfs_ioc_health_monitor(struct xfs_mount *mp,
		struct xfs_health_monitor __user *arg);
#else
# define xfs_ioc_health_monitor(mp, hmo)	(-ENOTTY)
#endif /* CONFIG_XFS_HEALTH_MONITOR */

#endif /* __XFS_HEALTHMON_H__ */
