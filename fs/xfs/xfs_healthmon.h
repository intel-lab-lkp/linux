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
};

enum xfs_healthmon_domain {
	XFS_HEALTHMON_MOUNT,	/* affects the whole fs */
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
		/* mount */
		struct {
			unsigned int	flags;
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
