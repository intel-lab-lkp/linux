/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (c) 2025, Broadcom Corporation
 *
 */

#ifndef _UAPI_FWCTL_BNXT_H_
#define _UAPI_FWCTL_BNXT_H_

#include <linux/types.h>

#define MAX_DMA_MEM_SIZE		0x10000 /*64K*/
#define DFLT_HWRM_CMD_TIMEOUT		500

enum fwctl_bnxt_commands {
	FWCTL_BNXT_QUERY_COMMANDS = 0,
	FWCTL_BNXT_SEND_COMMAND,
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

#define MAX_NUM_DMA_INDICATIONS 10

/**
 * struct fwctl_dma_info_bnxt - describe the buffer that should be DMAed
 * @data: DMA-intended buffer
 * @len: length of the @data
 * @offset: offset at which FW (HWRM) input structure needs DMA address
 * @read_from_device: DMA direction, 0 or 1
 * @unused: pad
 */
struct fwctl_dma_info_bnxt {
	__aligned_u64 data;
	__u32 len;
	__u16 offset;
	__u8 read_from_device;
	__u8 unused;
};

/**
 * struct fwctl_rpc_bnxt - describe the fwctl message for bnxt
 * @req: FW (HWRM) command input structure
 * @req_len: length of @req
 * @timeout: if the user wants to override the driver's default, 0 otherwise
 * @num_dma: number of DMA buffers to be added to @req
 * @payload: DMA buffer details in struct fwctl_dma_info_bnxt format
 */
struct fwctl_rpc_bnxt {
	__aligned_u64 req;
	__u32 req_len;
	__u32 timeout;
	__u32 num_dma;
	__aligned_u64 payload;
};
#endif
