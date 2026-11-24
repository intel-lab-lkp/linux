/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_FPC1540_TEE_H
#define _UAPI_LINUX_FPC1540_TEE_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct fpc_tee_cmd {
	__u64 buf;
	__u32 len;
	__s32 status;
};

#define FPC_TEE_IOC_MAGIC	0xBB
#define FPC_TEE_IOC_POWER	_IOW(FPC_TEE_IOC_MAGIC, 0, __u32)
#define FPC_TEE_IOC_RESET	_IO(FPC_TEE_IOC_MAGIC, 1)
#define FPC_TEE_IOC_CMD		_IOWR(FPC_TEE_IOC_MAGIC, 2, struct fpc_tee_cmd)
#define FPC_TEE_IOC_IRQ_CLEAR	_IO(FPC_TEE_IOC_MAGIC, 3)

#endif
