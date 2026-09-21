/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2020 Synopsys, Inc. and/or its affiliates.
 * Copyright (c) 2026 Google LLC
 *
 * Author: Vitor Soares <vitor.soares@synopsys.com>
 */

#ifndef _UAPI_I3C_DEV_H_
#define _UAPI_I3C_DEV_H_

#include <linux/types.h>
#include <linux/ioctl.h>

/* IOCTL commands */
#define I3C_DEV_IOC_MAGIC	0x07

#define I3C_DEV_DIR_WRITE	0
#define I3C_DEV_DIR_READ	1

#define I3C_XFER_MODE_HDR_DDR	0
#define I3C_XFER_MODE_HDR_TSP	1
#define I3C_XFER_MODE_HDR_TSL	2
#define I3C_XFER_MODE_SDR	31

/**
 * struct i3c_ioc_xfer - I3C ioctl transfer
 * @data: Holds pointer to userspace buffer with transmit/receive data.
 * @len: Length of data buffer, in bytes.
 * @actual_len: Actual length of data transferred on read, in bytes (output).
 * @rnw: Transfer direction for SDR mode (I3C_DEV_DIR_WRITE or I3C_DEV_DIR_READ).
 * @cmd: Command byte for HDR mode (0x00-0x7f write, 0x80-0xff read).
 * @mode: Transfer mode (I3C_XFER_MODE_SDR, I3C_XFER_MODE_HDR_DDR, etc.).
 * @pad: Reserved for future extensions; must be zeroed.
 */
struct i3c_ioc_xfer {
	__u64 data;
	__u16 len;
	__u16 actual_len;
	union {
		__u8 rnw;
		__u8 cmd;
	};
	__u8 mode;
	__u8 pad[2];
};

#define __I3C_XFER_SIZE(type)	\
	((((sizeof(struct i3c_ioc_xfer)) * (type)) < (1 << _IOC_SIZEBITS)) \
	? ((sizeof(struct i3c_ioc_xfer)) * (type)) : 0)

#define I3C_XFER_SIZE(N)	__I3C_XFER_SIZE(N)

#define I3C_IOC_XFER(N)	\
	_IOC(_IOC_READ | _IOC_WRITE, I3C_DEV_IOC_MAGIC, 30, I3C_XFER_SIZE(N))

#endif
