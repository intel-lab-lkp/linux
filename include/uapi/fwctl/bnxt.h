/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2026, Broadcom Inc
 */

#ifndef _UAPI_FWCTL_BNXT_H_
#define _UAPI_FWCTL_BNXT_H_

#include <linux/sizes.h>
#include <linux/types.h>

enum fwctl_bnxt_commands {
	FWCTL_BNXT_INLINE_COMMANDS = 0,
	FWCTL_BNXT_QUERY_COMMANDS,
	FWCTL_BNXT_SEND_COMMANDS,
	FWCTL_BNXT_DMA_COMMANDS,
};

/**
 * struct fwctl_info_bnxt - ioctl(FWCTL_INFO) out_device_data
 * @uctx_caps: The command capabilities driver accepts.
 *
 * Return basic information about the FW interface available.
 */
struct fwctl_info_bnxt {
	__u32 uctx_caps;
};

enum fwctl_bnxt_buf_dir {
	FWCTL_BNXT_BUF_TO_DEVICE   = 0,
	FWCTL_BNXT_BUF_FROM_DEVICE = 1,
};

/**
 * struct fwctl_bnxt_buf - one indirect DMA buffer descriptor
 * @addr: Userspace pointer to the payload data.
 * @len:  Byte length of the buffer.
 * @dir:  One of enum fwctl_bnxt_buf_dir.
 * @rsvd: Must be zero.
 */
struct fwctl_bnxt_buf {
	__aligned_u64 addr;
	__u32         len;
	__u32         dir;
	__u32         rsvd[2];
};

#define FWCTL_BNXT_MAX_BUFS	4
#define FWCTL_BNXT_MAX_DMABUF	SZ_4M

/**
 * struct fwctl_bnxt_driver_data - pointed to by fwctl_rpc::driver_data for bnxt
 * @num_bufs: Number of valid entries in @bufs. Must be non-zero and no greater
 *   than the number of DMA address fields the specific HWRM command supports
 * @rsvd:     Must be zero.
 * @bufs:     Array of buffer descriptors.
 */
struct fwctl_bnxt_driver_data {
	__u32                 num_bufs;
	__u32                 rsvd;
	struct fwctl_bnxt_buf bufs[FWCTL_BNXT_MAX_BUFS];
};

#endif
