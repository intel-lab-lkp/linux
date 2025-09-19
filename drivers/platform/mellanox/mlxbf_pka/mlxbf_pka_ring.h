/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/* SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION. All rights reserved. */

#ifndef __MLXBF_PKA_RING_H__
#define __MLXBF_PKA_RING_H__

/* PKA ring device related definitions. */
#define CMD_DESC_SIZE 64

/**
 * struct mlxbf_pka_ring_desc_t - PKA command/result ring descriptor
 * @num_descs: Total number of descriptors in the ring
 * @cmd_ring_base: Base address of the command ring
 * @cmd_idx: Index of the command in a ring
 * @rslt_ring_base: Base address of the result ring
 * @rslt_idx: Index of the result in a ring
 * @operands_base: Operands memory base address
 * @operands_end: End address of operands memory
 * @desc_size: Size of each element in the ring
 * @cmd_desc_mask: Bitmask of free(0)/in_use(1) cmd descriptors
 * @cmd_desc_cnt: Number of command descriptors currently in use
 * @rslt_desc_cnt: Number of result descriptors currently ready
 *
 * Describes the PKA command/result ring as used by the hardware. A pair of
 * command and result rings in PKA window memory, and the data memory used by
 * the commands.
 */
struct mlxbf_pka_ring_desc_t {
	u32 num_descs;
	u32 cmd_ring_base;
	u32 cmd_idx;
	u32 rslt_ring_base;
	u32 rslt_idx;
	u32 operands_base;
	u32 operands_end;
	u32 desc_size;
	u64 cmd_desc_mask;
	u32 cmd_desc_cnt;
	u32 rslt_desc_cnt;
};

/**
 * struct mlxbf_pka_ring_info_t - Ring parameters for user interface
 * @fd: File descriptor
 * @group: Iommu group
 * @container: VFIO container
 * @idx: Ring index
 * @ring_id: Hardware ring identifier
 * @mem_off: Offset specific to window RAM region
 * @mem_addr: Window RAM region address
 * @mem_size: Window RAM region size
 * @reg_off: Offset specific to count registers region
 * @reg_addr: Count registers region address
 * @reg_size: Count registers region size
 * @mem_ptr: Pointer to mapped memory region
 * @reg_ptr: Pointer to mapped counters region
 * @ring_desc: Ring descriptor
 * @big_endian: Big endian byte order when enabled
 *
 * This structure declares ring parameters which can be used by user interface.
 */
struct mlxbf_pka_ring_info_t {
	int fd;
	int group;
	int container;
	u32 idx;
	u32 ring_id;
	u64 mem_off;
	u64 mem_addr;
	u64 mem_size;
	u64 reg_off;
	u64 reg_addr;
	u64 reg_size;
	void *mem_ptr;
	void *reg_ptr;
	struct mlxbf_pka_ring_desc_t ring_desc;
	u8 big_endian;
};

/* ring_info related definitions. */
#define MLXBF_PKA_RING_INFO_IN_ORDER_MASK      GENMASK(31, 31)
#define MLXBF_PKA_RING_INFO_HOST_DESC_SIZE_MASK	GENMASK(27, 18)
#define MLXBF_PKA_RING_NUM_CMD_DESC_MASK	GENMASK(15, 0)

/* Ring option related definitions. */
#define MLXBF_PKA_RING_OPTIONS_RING_PRIORITY_MASK      GENMASK(7, 0)
#define MLXBF_PKA_RING_OPTIONS_RING_NUM_MASK	   GENMASK(15, 8)
#define MLXBF_PKA_RING_OPTIONS_SIGNATURE_BYTE_MASK     GENMASK(31, 24)

/*
 * Ring CSRs: these are all accessed from the ARM using 8 byte reads/writes.
 * However only the bottom 32 bits are implemented.
 */
/* Ring 0 CSRS. */
#define MLXBF_PKA_COMMAND_COUNT_0_ADDR 0x80080

/* Ring 0. */
#define MLXBF_PKA_RING_CMD_BASE_0_ADDR 0x00000
#define MLXBF_PKA_RING_RSLT_BASE_0_ADDR 0x00010
#define MLXBF_PKA_RING_SIZE_TYPE_0_ADDR 0x00020
#define MLXBF_PKA_RING_RW_PTRS_0_ADDR  0x00028
#define MLXBF_PKA_RING_RW_STAT_0_ADDR  0x00030

/* Ring Options. */
#define MLXBF_PKA_RING_OPTIONS_ADDR    0x07FF8

/*
 * Resources are regions which include info control/status words, count
 * registers and host window RAM.
 */
#define MLXBF_PKA_MAX_NUM_RING_RESOURCES 3

/*
 * PKA Ring resources.
 * Define Ring resources parameters including base address, size (in bytes)
 * and ring spacing.
 */
#define MLXBF_PKA_RING_WORDS_ADDR MLXBF_PKA_BUFFER_RAM_BASE
#define MLXBF_PKA_RING_CNTRS_ADDR MLXBF_PKA_COMMAND_COUNT_0_ADDR

#define MLXBF_PKA_RING_WORDS_SIZE      SZ_64
#define MLXBF_PKA_RING_CNTRS_SIZE      SZ_32
#define MLXBF_PKA_RING_MEM_SIZE		SZ_16K

#define MLXBF_PKA_RING_WORDS_SPACING   SZ_64
#define MLXBF_PKA_RING_CNTRS_SPACING   SZ_64K
#define MLXBF_PKA_RING_MEM_0_SPACING   SZ_16K
#define MLXBF_PKA_RING_MEM_1_SPACING   SZ_64K

/* Defines for window RAM partition, valid for 16K memory. */
#define MLXBF_PKA_WINDOW_RAM_RING_MEM_SIZE     SZ_2K
#define MLXBF_PKA_WINDOW_RAM_RING_MEM_DIV      2 /* Divide into halves. */
#define MLXBF_PKA_WINDOW_RAM_RING_ADDR_MASK    GENMASK(15, 0)
#define MLXBF_PKA_WINDOW_RAM_RING_SIZE_MASK    GENMASK(19, 16)
#define MLXBF_PKA_WINDOW_RAM_RING_SIZE_SHIFT   2

/*
 * MLXBF_PKA_RING_OPTIONS_PRIORITY field to specify the priority in which rings
 * are handled. In this implementation, the priority is full rotating priority,
 * with the value of 0x0.
 */
#define MLXBF_PKA_RING_OPTIONS_PRIORITY	0x0

/*
 * 'Signature' byte used because the ring options are transferred through RAM
 * which does not have a defined reset value. The EIP-154 master controller
 * keeps reading the MLXBF_PKA_RING_OPTIONS word at start-up until the
 * 'Signature' byte contains 0x46 and the 'Reserved' field contains zero.
 */
#define MLXBF_PKA_RING_OPTIONS_SIGNATURE_BYTE 0x46

/*
 * Order of the result reporting: two schemas are available:
 *  InOrder - the results will be reported in the same order as the commands
 *	    were provided.
 *  OutOfOrder - the results are reported as soon as they are available.
 * Note: only the OutOfOrder schema is used in this implementation.
 */
#define MLXBF_PKA_RING_TYPE_OUT_OF_ORDER_BIT   0
#define MLXBF_PKA_RING_TYPE_IN_ORDER	   MLXBF_PKA_RING_TYPE_OUT_OF_ORDER_BIT

/* PKA device related definitions. */
#define MLXBF_PKA_DEVFS_RING_DEVICES "mlxbf_pka/%d"

/**
 * struct mlxbf_pka_dev_ring_res_t - PKA Ring resources structure
 * @info_words: Ring information words
 * @counters: Ring counters
 * @window_ram: Window RAM
 */
struct mlxbf_pka_dev_ring_res_t {
	struct mlxbf_pka_dev_res_t info_words;
	struct mlxbf_pka_dev_res_t counters;
	struct mlxbf_pka_dev_res_t window_ram;
};

/**
 * struct mlxbf_pka_dev_ring_t - PKA Ring structure
 * @ring_id: Ring identifier
 * @shim: Pointer to the shim associated to the ring
 * @resources_num: Number of ring resources
 * @resources: Ring resources
 * @ring_info: Ring information
 * @num_cmd_desc: Number of command descriptors
 * @status: Status of the ring
 * @mutex: Mutex lock for sharing ring device
 */
struct mlxbf_pka_dev_ring_t {
	u32 ring_id;
	struct mlxbf_pka_dev_shim_s *shim;
	u32 resources_num;
	struct mlxbf_pka_dev_ring_res_t resources;
	struct mlxbf_pka_dev_hw_ring_info *ring_info;
	u32 num_cmd_desc;
	s8 status;
	struct mutex mutex;
};

/* Defines for mlxbf_pka_dev_ring->status. */
#define MLXBF_PKA_DEV_RING_STATUS_UNDEFINED    -1
#define MLXBF_PKA_DEV_RING_STATUS_INITIALIZED  1
#define MLXBF_PKA_DEV_RING_STATUS_READY		2
#define MLXBF_PKA_DEV_RING_STATUS_BUSY	 3

/*
 * Ring getter for mlxbf_pka_dev_gbl_config_t structure which holds all system
 * global configuration. This configuration is shared and common to kernel
 * device driver associated with PKA hardware.
 */
struct mlxbf_pka_dev_ring_t *mlxbf_pka_dev_get_ring(u32 ring_id);

/* Configure ring options. */
int mlxbf_pka_dev_config_ring_options(struct device *dev,
				      struct mlxbf_pka_dev_res_t *buffer_ram_ptr,
				      u32 rings_num,
				      u8 ring_priority);

				      /* Release a given Ring. */
int mlxbf_pka_dev_release_ring(struct device *dev, struct mlxbf_pka_dev_ring_t *ring);

/*
 * Register a ring. This function initializes a Ring and configures its related
 * resources, and returns the error code.
 */
int mlxbf_pka_dev_register_ring(struct device *dev,
				u32 ring_id,
				u32 shim_id,
				struct mlxbf_pka_dev_ring_t **ring);

/* Unregister a Ring. */
int mlxbf_pka_dev_unregister_ring(struct device *dev, struct mlxbf_pka_dev_ring_t *ring);

/* Reset a Ring. */
int mlxbf_pka_dev_reset_ring(struct mlxbf_pka_dev_ring_t *ring);

/*
 * Clear ring counters. This function resets the master sequencer controller to
 * clear the command and result counters.
 */
int mlxbf_pka_dev_clear_ring_counters(struct mlxbf_pka_dev_ring_t *ring);

/*
 * Open the file descriptor associated with ring. It returns an integer value,
 * which is used to refer to the file. If not successful, it returns a negative
 * error.
 */
int mlxbf_pka_dev_open_ring(struct device *dev, struct mlxbf_pka_ring_info_t *ring_info);

/*
 * Close the file descriptor associated with ring. The function returns 0 if
 * successful, negative value to indicate an error.
 */
int mlxbf_pka_dev_close_ring(struct mlxbf_pka_ring_info_t *ring_info);

#endif /* __MLXBF_PKA_RING_H__ */
