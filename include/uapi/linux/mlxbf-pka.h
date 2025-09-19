/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION. All rights reserved. */

#ifndef _UAPI_LINUX_MLXBF_PKA_H
#define _UAPI_LINUX_MLXBF_PKA_H

#include <linux/types.h>
#include <linux/ioctl.h>

/* PKA IOCTL related definitions. */
#define MLXBF_PKA_IOC_TYPE 0xBF

/**
 * struct mlxbf_pka_dev_region_info - Device region information
 * @reg_index: Registers region index
 * @reg_size: Registers region size (bytes)
 * @reg_offset: Registers region offset from start of device fd
 * @mem_index: Memory region index
 * @mem_size: Memory region size (bytes)
 * @mem_offset: Memory region offset from start of device fd
 *
 * MLXBF_PKA_RING_GET_REGION_INFO:
 * _IOWR(MLXBF_PKA_IOC_TYPE, 0x0, mlxbf_pka_dev_region_info).
 *
 * Retrieve information about a device region. This is intended to describe
 * MMIO, I/O port, as well as bus specific regions (ex. PCI config space). Zero
 * sized regions may be used to describe unimplemented regions.
 *
 * Return: 0 on success, -errno on failure.
 */
struct mlxbf_pka_dev_region_info {
	__u32 reg_index;
	__u64 reg_size;
	__u64 reg_offset;
	__u32 mem_index;
	__u64 mem_size;
	__u64 mem_offset;
};

#define MLXBF_PKA_RING_GET_REGION_INFO \
	_IOWR(MLXBF_PKA_IOC_TYPE, 0x0, struct mlxbf_pka_dev_region_info)

/**
 * struct mlxbf_pka_dev_hw_ring_info - Bluefield specific ring information
 * @cmd_base: Base address of the command descriptor ring
 * @rslt_base: Base address of the result descriptor ring
 * @size: Size of a command ring in number of descriptors, minus 1. Minimum
 *	value is 0 (for 1 descriptor); maximum value is 65535 (for 64K
 *	descriptors)
 * @host_desc_size: This field specifies the size (in 32-bit words) of the space
 *		  that PKI command and result descriptor occupies on the Host
 * @in_order: Indicates whether the result ring delivers results strictly
 *	    in-order ('1') or that result descriptors are written to the
 *	    result ring as soon as they become available, or out-of-order ('0')
 * @cmd_rd_ptr: Read pointer of the command descriptor ring
 * @rslt_wr_ptr: Write pointer of the result descriptor ring
 * @cmd_rd_stats: Read statistics of the command descriptor ring
 * @rslt_wr_stats: Write statistics of the result descriptor ring
 *
 * MLXBF_PKA_GET_RING_INFO:
 * _IOWR(MLXBF_PKA_IOC_TYPE, 0x1, mlxbf_pka_dev_hw_ring_info).
 *
 * Retrieve information about a ring. This is intended to describe ring
 * information words located in MLXBF_PKA_BUFFER_RAM. Ring information
 * includes base addresses, size and statistics.
 *
 * Return: 0 on success, -errno on failure.
 */
struct mlxbf_pka_dev_hw_ring_info {
	__u64 cmd_base;
	__u64 rslt_base;
	__u16 size;
	__u16 host_desc_size : 10;
	__u8 in_order : 1;
	__u16 cmd_rd_ptr;
	__u16 rslt_wr_ptr;
	__u16 cmd_rd_stats;
	__u16 rslt_wr_stats;
};

#define MLXBF_PKA_GET_RING_INFO _IOWR(MLXBF_PKA_IOC_TYPE, 0x1, struct mlxbf_pka_dev_hw_ring_info)

/**
 * MLXBF_PKA_CLEAR_RING_COUNTERS:
 * _IO(MLXBF_PKA_IOC_TYPE, 0x2).
 *
 * Clear counters. This is intended to reset all command and result counters.
 *
 * Return: 0 on success, -errno on failure.
 */
#define MLXBF_PKA_CLEAR_RING_COUNTERS _IO(MLXBF_PKA_IOC_TYPE, 0x2)

#endif /* _UAPI_LINUX_MLXBF_PKA_H */
