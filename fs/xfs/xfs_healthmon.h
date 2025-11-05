/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (c) 2024-2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#ifndef __XFS_HEALTHMON_H__
#define __XFS_HEALTHMON_H__

#ifdef CONFIG_XFS_HEALTH_MONITOR
long xfs_ioc_health_monitor(struct xfs_mount *mp,
		struct xfs_health_monitor __user *arg);
#else
# define xfs_ioc_health_monitor(mp, hmo)	(-ENOTTY)
#endif /* CONFIG_XFS_HEALTH_MONITOR */

#endif /* __XFS_HEALTHMON_H__ */
