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
#define MSHAREFS_CREATE_MAPPING	_IOW('x', 0,  struct mshare_create)
#define MSHAREFS_UNMAP		_IOW('x', 1,  struct mshare_unmap)

struct mshare_create {
	__u64 region_offset;
	__u64 size;
	__u64 offset;
	__u32 prot;
	__u32 flags;
	__u32 fd;
};

struct mshare_unmap {
	__u64 region_offset;
	__u64 size;
};

#endif
