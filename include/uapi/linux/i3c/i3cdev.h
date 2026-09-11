/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2026 Microsoft Corporation
 *
 * Author: Meagan Lloyd <meaganlloyd@linux.microsoft.com>
 *
 * Based on code from:
 *     Copyright (c) 2020 Synopsys, Inc. and/or its affiliates.
 *     Author: Vitor Soares <vitor.soares@synopsys.com>
 *
 *     Author: Boris Brezillon <boris.brezillon@bootlin.com>
 */

#ifndef _UAPI_I3C_DEV_H_
#define _UAPI_I3C_DEV_H_

#include <linux/types.h>
#include <linux/ioctl.h>

/**
 * Reserved ioctl encoding for i3cdev
 * Code 0x7, Seq# 0x0-0x9E
 */
#define I3CDEV_IOCTL_ID	0x07

/**
 * struct i3cdev_xfer - I3C transfer (read/write)
 *
 * This struct more-or-less mirrors the kernel's 'struct i3c_xfer'.
 * Currently, only Single Data Rate (SDR) transfers are supported.
 *
 * @data: Pointer to userspace buffer. For writes, this will be the
 * bytes to send to the Target. For reads, this buffer will be
 * populated with the read response from the Target.
 * @actual_len: Where the kernel will report the number of processed
 * bytes. For reads, this reflects the number of response bytes in the
 * @data buffer. For SDR writes, the user shouldn't use this member
 * as it's neither supported nor useful.
 * @len: Length of input @data buffer in bytes.
 * @rnw: Transfer direction. 1 for a read, 0 for a write
 * @pad: Used to eliminate implicit, undefined-value padding. Zero
 * these bytes without referencing this field (for compatibility).
 */
struct i3cdev_xfer {
	__u64 data;
	__u16 actual_len; /* output */
	__u16 len;
	union {
		__u8 rnw; /* SDR */
		__u8 cmd; /* Not currently supported (HDR) */
	};
	__u8 pad[3];
};

/**
 * struct i3cdev_xfers - I3C transfers
 * @nxfers: Number of i3cdev_xfer objs in @xfers
 * @xfers: Pointer to an array of i3cdev_xfer objs
 * @xfer_size: sizeof(struct i3cdev_xfer)
 */
struct i3cdev_xfers {
	__u64 nxfers;
	__u64 xfers;
	__u64 xfer_size;
};

#define I3CDEV_XFER \
	_IOWR(I3CDEV_IOCTL_ID, 0, struct i3cdev_xfers)

#endif
