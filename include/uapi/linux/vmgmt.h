/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Header file for Versal PCIe device user API
 *
 * Copyright (C) 2024 AMD Corporation, Inc.
 */

#ifndef _UAPI_LINUX_VMGMT_H
#define _UAPI_LINUX_VMGMT_H

#include <linux/ioctl.h>

#define VERSAL_MGMT_MAGIC	0xB7
#define VERSAL_MGMT_BASE	0

/**
 * VERSAL_MGMT_LOAD_XCLBIN_IOCTL - Download XCLBIN to the device
 *
 * This IOCTL is used to download XCLBIN down to the device.
 * Return: 0 on success, -errno on failure.
 */
#define VERSAL_MGMT_LOAD_XCLBIN_IOCTL	_IOW(VERSAL_MGMT_MAGIC,		\
					     VERSAL_MGMT_BASE + 0, void *)

#endif /* _UAPI_LINUX_VMGMT_H */
