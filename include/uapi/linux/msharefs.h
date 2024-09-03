/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * msharefs defines a memory region that is shared across processes.
 * ioctl is used on files created under msharefs to set various
 * attributes on these shared memory regions
 *
 *
 * Copyright (C) 2024 Oracle Corp. All rights reserved.
 * Author:	Khalid Aziz <khalid@kernel.org>
 */

#ifndef _UAPI_LINUX_MSHAREFS_H
#define _UAPI_LINUX_MSHAREFS_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * msharefs specific ioctl commands
 */
#define MSHAREFS_GET_SIZE	_IOR('x', 0,  struct mshare_info)
#define MSHAREFS_SET_SIZE	_IOW('x', 1,  struct mshare_info)

struct mshare_info {
	__u64 start;
	__u64 size;
};

#endif
