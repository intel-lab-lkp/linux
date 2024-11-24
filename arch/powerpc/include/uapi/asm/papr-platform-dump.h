/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_PAPR_PLATFORM_DUMP_H_
#define _UAPI_PAPR_PLATFORM_DUMP_H_

#include <asm/ioctl.h>
#include <asm/papr-miscdev.h>

/*
 * ioctl for /dev/papr-platform-dump. Returns a VPD handle fd corresponding to
 * the location code.
 */
#define PAPR_PLATFORM_DUMP_IOC_CREATE_HANDLE _IOW(PAPR_MISCDEV_IOC_ID, 3, __u64)

#endif /* _UAPI_PAPR_PLATFORM_DUMP_H_ */
