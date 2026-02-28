/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2024 Unisoc Technologies Co., Ltd.
 */

#ifndef _UAPI_LINUX_EXFAT_H
#define _UAPI_LINUX_EXFAT_H
#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * exfat-specific ioctl commands
 */

#define EXFAT_IOC_SHUTDOWN		_IOR('X', 125, __u32)
/* Get the current valid data length(VDL) of a file */
#define EXFAT_IOC_GET_VALID_DATA	_IOR('r', 0x14, __u64)
/*
 * Set the valid data length(VDL) of a file. This is equivalent to
 * SetValidData() in WinAPI. Due to security implications, CAP_SYS_ADMIN is
 * required(see capabilities(7)).
 */
#define EXFAT_IOC_SET_VALID_DATA	_IOW('r', 0x15, __u64)

/*
 * Flags used by EXFAT_IOC_SHUTDOWN
 */

#define EXFAT_GOING_DOWN_DEFAULT	0x0	/* default with full sync */
#define EXFAT_GOING_DOWN_FULLSYNC	0x1     /* going down with full sync*/
#define EXFAT_GOING_DOWN_NOSYNC         0x2     /* going down */

#endif /* _UAPI_LINUX_EXFAT_H */
