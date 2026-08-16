/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * FPGA userspace API
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */
#ifndef _UAPI_LINUX_FPGA_H
#define _UAPI_LINUX_FPGA_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define FPGA_IOCTL_LOAD_DMA_BUF	_IOW('J', 1, __s32)

#endif /* _UAPI_LINUX_FPGA_H */
