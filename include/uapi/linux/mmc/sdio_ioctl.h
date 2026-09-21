/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_MMC_SDIO_IOCTL_H
#define _UAPI_LINUX_MMC_SDIO_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

/*
 * SDIO userspace passthrough interface, issued on the character device
 * exposed by each SDIO function (/dev/mmcX:YYYY:FN). Requires
 * CAP_SYS_ADMIN. Raw access while a function driver is bound can
 * disturb the state the driver maintains.
 */

/*
 * CMD52: single byte read/write. fn 0 addresses the card common area
 * (CCCR/FBR), functions 1-7 address the per-function registers. With
 * raw set, the register is read back after being written and data
 * carries the value actually read.
 */
struct sdio_ioc_rw {
	__u8 write;		/* 1 = write, 0 = read */
	__u8 fn;		/* function number, 0..7 */
	__u8 raw;		/* read back after write */
	__u8 data;		/* byte to write / value read */
	__u32 addr;		/* SDIO register address */
};

/*
 * CMD53: extended (multiple byte) read/write in byte mode, 1..512
 * bytes. Function 0 is not valid for CMD53.
 */
struct sdio_ioc_rw_ext {
	__u8 write;		/* 1 = write, 0 = read */
	__u8 fn;		/* function number, 1..7 */
	__u8 incr_addr;		/* 1 = incrementing address, 0 = fixed */
	__u8 reserved1;
	__u32 addr;		/* SDIO register address */
	__u16 count;		/* number of bytes, 1..512 */
	__u16 reserved2;
	__u8 data[512];		/* data to write / buffer for read */
};

#define SDIO_IOC_MAGIC		'S'
#define SDIO_IOC_RW		_IOWR(SDIO_IOC_MAGIC, 0x52, struct sdio_ioc_rw)
#define SDIO_IOC_RW_EXT		_IOWR(SDIO_IOC_MAGIC, 0x53, struct sdio_ioc_rw_ext)

#endif /* _UAPI_LINUX_MMC_SDIO_IOCTL_H */
