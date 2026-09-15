/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Description:
 * This driver is developed for the SDM SoCFPGA Crypto Service (FCS). It
 * provides an ioctl interface for the SDOS (Secure Data Object Service)
 * encrypt/decrypt operation. The crypto session and the per-request context
 * ID are managed by the kernel internally, so neither is part of the user
 * ABI.
 */
#ifndef __SOCFPGA_FCS_CRYPTO_H
#define __SOCFPGA_FCS_CRYPTO_H

#include <linux/types.h>
#include <linux/ioctl.h>

struct fcs_ioc_sdos {
	__u64 error_code;	/* __user ptr to __s32 (out)              */
	__u64 src;		/* __user ptr to input buffer (in)        */
	__u64 dst;		/* __user ptr to output buffer (out)      */
	__u64 dst_size;		/* __user ptr to __u32 capacity/len (in/out) */
	__u32 op_mode;		/* (in)  */
	__u32 src_size;		/* (in)  */
};

#define FCS_IOC_MAGIC		0xA6
#define FCS_IOC_SDOS		_IOWR(FCS_IOC_MAGIC, 1, struct fcs_ioc_sdos)

#endif /* __SOCFPGA_FCS_CRYPTO_H */
