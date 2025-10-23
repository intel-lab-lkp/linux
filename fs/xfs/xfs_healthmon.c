// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (c) 2024-2025 Oracle.  All Rights Reserved.
 * Author: Darrick J. Wong <djwong@kernel.org>
 */
#include "xfs.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_inode.h"
#include "xfs_trace.h"
#include "xfs_ag.h"
#include "xfs_btree.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_quota_defs.h"
#include "xfs_rtgroup.h"
#include "xfs_healthmon.h"

#include <linux/anon_inodes.h>
#include <linux/eventpoll.h>
#include <linux/poll.h>

/*
 * Live Health Monitoring
 * ======================
 *
 * Autonomous self-healing of XFS filesystems requires a means for the kernel
 * to send filesystem health events to a monitoring daemon in userspace.  To
 * accomplish this, we establish a thread_with_file kthread object to handle
 * translating internal events about filesystem health into a format that can
 * be parsed easily by userspace.  Then we hook various parts of the filesystem
 * to supply those internal events to the kthread.  Userspace reads events
 * from the file descriptor returned by the ioctl.
 *
 * The healthmon abstraction has a weak reference to the host filesystem mount
 * so that the queueing and processing of the events do not pin the mount and
 * cannot slow down the main filesystem.  The healthmon object can exist past
 * the end of the filesystem mount.
 */

struct xfs_healthmon {
	struct xfs_mount		*mp;
};

/*
 * Convey queued event data to userspace.  First copy any remaining bytes in
 * the outbuf, then format the oldest event into the outbuf and copy that too.
 */
STATIC ssize_t
xfs_healthmon_read_iter(
	struct kiocb		*iocb,
	struct iov_iter		*to)
{
	return -EIO;
}

/* Free the health monitoring information. */
STATIC int
xfs_healthmon_release(
	struct inode		*inode,
	struct file		*file)
{
	struct xfs_healthmon	*hm = file->private_data;

	kfree(hm);

	return 0;
}

/* Validate ioctl parameters. */
static inline bool
xfs_healthmon_validate(
	const struct xfs_health_monitor	*hmo)
{
	if (hmo->flags)
		return false;
	if (hmo->format)
		return false;
	if (memchr_inv(&hmo->pad1, 0, sizeof(hmo->pad1)))
		return false;
	if (memchr_inv(&hmo->pad2, 0, sizeof(hmo->pad2)))
		return false;
	return true;
}

/* Emit some data about the health monitoring fd. */
#ifdef CONFIG_PROC_FS
static void
xfs_healthmon_show_fdinfo(
	struct seq_file		*m,
	struct file		*file)
{
	struct xfs_healthmon	*hm = file->private_data;

	seq_printf(m, "state:\talive\ndev:\t%s\n",
			hm->mp->m_super->s_id);
}
#endif

static const struct file_operations xfs_healthmon_fops = {
	.owner		= THIS_MODULE,
#ifdef CONFIG_PROC_FS
	.show_fdinfo	= xfs_healthmon_show_fdinfo,
#endif
	.read_iter	= xfs_healthmon_read_iter,
	.release	= xfs_healthmon_release,
};

/*
 * Create a health monitoring file.  Returns an index to the fd table or a
 * negative errno.
 */
long
xfs_ioc_health_monitor(
	struct xfs_mount		*mp,
	struct xfs_health_monitor __user *arg)
{
	struct xfs_health_monitor	hmo;
	struct xfs_healthmon		*hm;
	int				fd;
	int				ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (copy_from_user(&hmo, arg, sizeof(hmo)))
		return -EFAULT;

	if (!xfs_healthmon_validate(&hmo))
		return -EINVAL;

	hm = kzalloc(sizeof(*hm), GFP_KERNEL);
	if (!hm)
		return -ENOMEM;
	hm->mp = mp;

	/*
	 * Create the anonymous file.  If it succeeds, the file owns hm and
	 * can go away at any time, so we must not access it again.
	 */
	fd = anon_inode_getfd("xfs_healthmon", &xfs_healthmon_fops, hm,
			O_CLOEXEC | O_RDONLY);
	if (fd < 0) {
		ret = fd;
		goto out_hm;
	}

	return fd;

out_hm:
	kfree(hm);
	return ret;
}
