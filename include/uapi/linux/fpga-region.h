/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Header File for FPGA Region User API
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc.
 *
 * Author: Manne, Nava kishore <nava.kishore.manne@amd.com>
 */

#ifndef _UAPI_LINUX_FPGA_REGION_H
#define _UAPI_LINUX_FPGA_REGION_H

#include <linux/ioctl.h>
#include <linux/limits.h>
#include <linux/types.h>

/* IOCTLs for fpga region file descriptor */
#define FPGA_REGION_MAGIC_NUMBER	'f'
#define FPGA_REGION_BASE		0

/**
 * FPGA_REGION_IOCTL_LOAD - _IOW(FPGA_REGION_MAGIC, 0,
 *                               struct fpga_region_config_info)
 *
 * FPGA_REGION_IOCTL_REMOVE - _IOW(FPGA_REGION_MAGIC, 1,
 *                                 struct fpga_region_config_info)
 *
 * Driver does Configuration/Reconfiguration based on Region ID and
 * Buffer (Image) provided by caller.
 * Return: 0 on success, -errno on failure.
 */
struct fpga_region_config_info {	/* Input */
	char firmware_name[NAME_MAX];   /* Firmware file name */
};

/*
 * FPGA Region Control IOCTLs.
 */
#define FPGA_REGION_MAGIC	'f'
#define FPGA_IOW(num, dtype)	_IOW(FPGA_REGION_MAGIC, num, dtype)
#define FPGA_IOR(num, dtype)	_IOR(FPGA_REGION_MAGIC, num, dtype)

#define FPGA_REGION_IOCTL_LOAD		FPGA_IOW(0, __u32)
#define FPGA_REGION_IOCTL_REMOVE        FPGA_IOW(1, __u32)
#define FPGA_REGION_IOCTL_STATUS        FPGA_IOR(2, __u32)

/* Region status possibilities returned by FPGA_REGION_IOCTL_STATUS ioctl */
#define FPGA_REGION_HAS_PL	0	/* if the region has PL logic */
#define FPGA_REGION_EMPTY	1	/* If the region is empty */

#endif /* _UAPI_LINUX_FPGA_REGION_H */
