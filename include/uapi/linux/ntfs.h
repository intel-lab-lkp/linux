/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */

#ifndef _UAPI_LINUX_NTFS_H
#define _UAPI_LINUX_NTFS_H
#include <linux/types.h>
#include <linux/ioctl.h>

#define NTFS_IOCTL_MAGIC	0xEF			/* shared with exfat */

/*
 * cifx, btrfs, exfat, ext4, f2fs use this constant.
 * Hope this value will become common to all fs.
 */
#define NTFS3_IOC_SHUTDOWN	_IOR('X', 125, __u32)

#endif /* _UAPI_LINUX_NTFS_H */
