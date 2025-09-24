/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/* SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION. All rights reserved. */

#ifndef __MLXBF_PKA_DEV_H__
#define __MLXBF_PKA_DEV_H__

/*
 * @file
 *
 * API to handle the PKA EIP-154 I/O block (shim). It provides functions and
 * data structures to initialize and configure the PKA shim. It's the "southband
 * interface" for communication with PKA hardware resources.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/types.h>
#include <linux/units.h>

#include <uapi/linux/mlxbf-pka.h>

#define MASTER_CONTROLLER_OUT_OF_RESET 0

/* PKA address related definitions. */

/*
 * Global Control Space CSR addresses/offsets. These are accessed from the ARM
 * as 8 byte reads/writes. However only the bottom 32 bits are implemented.
 */
#define MLXBF_PKA_CLK_FORCE_ADDR 0x11C80

/*
 * Advanced Interrupt Controller CSR addresses/offsets. These are accessed from
 * the ARM as 8 byte reads/writes. However only the bottom 32 bits are
 * implemented.
 */
#define MLXBF_PKA_AIC_POL_CTRL_ADDR 0x11E00

/*
 * Control register address/offset. This is accessed from the ARM using 8 byte
 * reads/writes. However only the bottom 32 bits are implemented.
 */
#define MLXBF_PKA_MASTER_SEQ_CTRL_ADDR 0x27F90

/* PKA buffer RAM */
#define MLXBF_PKA_BUFFER_RAM_BASE 0x00000
#define MLXBF_PKA_BUFFER_RAM_SIZE SZ_16K

/*
 * PKA Buffer RAM offsets. These are NOT real CSR's but instead are specific
 * offset/addresses within the EIP154 MLXBF_PKA_BUFFER_RAM.
 */

/* Alternate Window RAM size. */
#define MLXBF_PKA_WINDOW_RAM_REGION_SIZE SZ_16K

/* PKA configuration related definitions. */

/* The maximum number of PKA shims referred to as IO blocks. */
#define MLXBF_PKA_MAX_NUM_IO_BLOCKS 24

/* The maximum number of rings supported by the IO block (shim). */
#define MLXBF_PKA_MAX_NUM_IO_BLOCK_RINGS 4

#define MLXBF_PKA_MAX_NUM_RINGS (MLXBF_PKA_MAX_NUM_IO_BLOCK_RINGS * MLXBF_PKA_MAX_NUM_IO_BLOCKS)

/*
 * PKA Window RAM parameters.
 * Define whether to split window RAM during PKA device creation phase.
 */
#define MLXBF_PKA_SPLIT_WINDOW_RAM_MODE 0

/* Defines for window RAM partition, valid for 16K memory. */
#define MLXBF_PKA_WINDOW_RAM_DATA_MEM_SIZE	0x3800 /* 14KB. */

/* Window RAM/Alternate window RAM offset mask for BF1 and BF2. */
#define MLXBF_PKA_WINDOW_RAM_OFFSET_BF1_BF2_MASK (GENMASK(17, 16) | GENMASK(22, 20))

/* Window RAM/Alternate window RAM offset mask for BF3. */
#define MLXBF_PKA_WINDOW_RAM_OFFSET_BF3_MASK GENMASK(18, 16)

/*
 * PKA Master Sequencer Control/Status Register.
 * Writing '1' to bit [31] puts the Master controller Sequencer in a reset
 * state. Resetting the Sequencer (in order to load other firmware) should
 * only be done when the EIP-154 is not performing any operations.
 */
#define MLXBF_PKA_MASTER_SEQ_CTRL_RESET BIT(31)
/*
 * Writing '1' to bit [30] will reset all Command and Result counters. This bit
 * is write-only and self clearing and can only be set if the 'Reset' bit [31]
 * is '1'.
 */
#define MLXBF_PKA_MASTER_SEQ_CTRL_CLEAR_COUNTERS BIT(30)

/**
 * struct mlxbf_pka_dev_res_t - Device resource structure
 * @ioaddr: The (iore)mapped version of addr, driver internal use
 * @base: Base address of the device's resource
 * @size: Size of IO
 * @type: Type of resource addr points to
 * @status: Status of the resource
 * @name: Name of the resource
 */
struct mlxbf_pka_dev_res_t {
	void __iomem *ioaddr;
	u64 base;
	u64 size;
	u8 type;
	s8 status;
	char *name;
};

/* Defines for mlxbf_pka_dev_res->type. */
#define MLXBF_PKA_DEV_RES_TYPE_MEM	1 /* Resource type is memory. */
#define MLXBF_PKA_DEV_RES_TYPE_REG	2 /* Resource type is register. */

/* Defines for mlxbf_pka_dev_res->status. */
#define MLXBF_PKA_DEV_RES_STATUS_MAPPED		1 /* The resource is (iore)mapped. */
#define MLXBF_PKA_DEV_RES_STATUS_UNMAPPED	-1 /* The resource is unmapped. */

/**
 * struct mlxbf_pka_dev_shim_res_t - PKA Shim resources structure
 * @buffer_ram: Buffer RAM
 * @master_seq_ctrl: Master sequencer controller CSR
 * @aic_csr: Interrupt controller CSRs
 * @trng_csr: TRNG module CSRs
 */
struct mlxbf_pka_dev_shim_res_t {
	struct mlxbf_pka_dev_res_t buffer_ram;
	struct mlxbf_pka_dev_res_t master_seq_ctrl;
	struct mlxbf_pka_dev_res_t aic_csr;
	struct mlxbf_pka_dev_res_t trng_csr;
};

/* Number of PKA device resources. */
#define MLXBF_PKA_DEV_SHIM_RES_CNT 6

/* Platform global shim resource information. */
struct mlxbf_pka_dev_gbl_shim_res_info_t {
	struct mlxbf_pka_dev_res_t *res_tbl[MLXBF_PKA_DEV_SHIM_RES_CNT];
	u8 res_cnt;
};

/**
 * struct mlxbf_pka_dev_mem_res - PKA device memory resources
 * @eip154_base: Base address for EIP154 mmio registers
 * @eip154_size: EIP154 mmio register region size
 * @wndw_ram_off_mask: Common offset mask for alt window RAM and window RAM
 * @wndw_ram_base: Base address for window RAM
 * @wndw_ram_size: Window RAM region size
 * @alt_wndw_ram_0_base: Base address for alternate window RAM 0
 * @alt_wndw_ram_1_base: Base address for alternate window RAM 1
 * @alt_wndw_ram_2_base: Base address for alternate window RAM 2
 * @alt_wndw_ram_3_base: Base address for alternate window RAM 3
 * @alt_wndw_ram_size: Alternate window RAM regions size
 * @csr_base: Base address for CSR registers
 * @csr_size: CSR area size
 */
struct mlxbf_pka_dev_mem_res {
	u64 eip154_base;
	u64 eip154_size;

	u64 wndw_ram_off_mask;
	u64 wndw_ram_base;
	u64 wndw_ram_size;

	u64 alt_wndw_ram_0_base;
	u64 alt_wndw_ram_1_base;
	u64 alt_wndw_ram_2_base;
	u64 alt_wndw_ram_3_base;
	u64 alt_wndw_ram_size;

	u64 csr_base;
	u64 csr_size;
};

/**
 * struct mlxbf_pka_dev_shim_s - PKA Shim structure
 * @mem_res: Memory resources
 * @trng_err_cycle: TRNG error cycle
 * @shim_id: Shim identifier
 * @rings_num: Number of supported rings (hardware specific)
 * @rings: Pointer to rings which belong to the shim
 * @ring_priority: Specify the priority in which rings are handled
 * @ring_type: Indicates whether the result ring delivers results strictly in-order
 * @resources: Shim resources
 * @window_ram_split: If non-zero, the split window RAM scheme is used
 * @busy_ring_num: Number of active rings (rings in busy state)
 * @trng_enabled: Whether the TRNG engine is enabled
 * @status: Status of the shim
 * @mutex: Mutex lock for sharing shim
 */
struct mlxbf_pka_dev_shim_s {
	struct mlxbf_pka_dev_mem_res mem_res;
	u64 trng_err_cycle;
	u32 shim_id;
	u32 rings_num;
	struct mlxbf_pka_dev_ring_t **rings;
	u8 ring_priority;
	u8 ring_type;
	struct mlxbf_pka_dev_shim_res_t resources;
	u8 window_ram_split;
	u32 busy_ring_num;
	u8 trng_enabled;
	s8 status;
	struct mutex mutex;
};

/* Defines for mlxbf_pka_dev_shim->status. */
#define MLXBF_PKA_SHIM_STATUS_UNDEFINED		-1
#define MLXBF_PKA_SHIM_STATUS_CREATED		1
#define MLXBF_PKA_SHIM_STATUS_INITIALIZED	2
#define MLXBF_PKA_SHIM_STATUS_RUNNING		3
#define MLXBF_PKA_SHIM_STATUS_STOPPED		4
#define MLXBF_PKA_SHIM_STATUS_FINALIZED		5

/* Defines for mlxbf_pka_dev_shim->window_ram_split. */

/* Window RAM is split into 4 * 16KB blocks. */
#define MLXBF_PKA_SHIM_WINDOW_RAM_SPLIT_ENABLED 1
/* Window RAM is not split and occupies 64KB. */
#define MLXBF_PKA_SHIM_WINDOW_RAM_SPLIT_DISABLED 2

/**
 * struct mlxbf_pka_dev_gbl_config_t - Platform global configuration structure
 * @dev_shims_cnt: Number of registered PKA shims
 * @dev_rings_cnt: Number of registered Rings
 * @dev_shims: Table of registered PKA shims
 * @dev_rings: Table of registered Rings
 */
struct mlxbf_pka_dev_gbl_config_t {
	u32 dev_shims_cnt;
	u32 dev_rings_cnt;
	struct mlxbf_pka_dev_shim_s *dev_shims[MLXBF_PKA_MAX_NUM_IO_BLOCKS];
	struct mlxbf_pka_dev_ring_t *dev_rings[MLXBF_PKA_MAX_NUM_RINGS];
};

extern struct mlxbf_pka_dev_gbl_config_t mlxbf_pka_gbl_config;

/*
 * Processor speed in hertz, used in routines which might be called very early
 * in boot.
 */
static inline u64 mlxbf_pka_early_cpu_speed(void)
{
	/*
	 * Initial guess at our CPU speed.  We set this to be larger than any
	 * possible real speed, so that any calculated delays will be too long,
	 * rather than too short.
	 *
	 * CPU Freq for High/Bin Chip - 1.255GHz.
	 */
	return 1255 * HZ_PER_MHZ;
}

/* Start a PKA device timer. */
static inline u64 mlxbf_pka_dev_timer_start_msec(u32 msec)
{
	u64 cur_time = get_cycles();

	return cur_time + mlxbf_pka_early_cpu_speed() * msec / MSEC_PER_SEC;
}

/* Test a PKA device timer for completion. */
static inline bool mlxbf_pka_dev_timer_done(u64 timer)
{
	return get_cycles() >= timer;
}

/* Return register base address. */
static inline u64 mlxbf_pka_dev_get_register_base(u64 base, u64 reg_addr)
{
	return (base + reg_addr) & PAGE_MASK;
}

/* Return register offset. */
static inline u64 mlxbf_pka_dev_get_register_offset(u64 base, u64 reg_addr)
{
	return (base + reg_addr) & ~PAGE_MASK;
}

/* Return word offset within io memory. */
static inline u64 mlxbf_pka_dev_get_word_offset(u64 mem_base, u64 word_addr, u64 mem_size)
{
	return (mem_base + word_addr) & (mem_size - 1);
}

static inline u64 mlxbf_pka_dev_io_read(void __iomem *mem_ptr, u64 mem_off)
{
	return readq_relaxed(mem_ptr + mem_off);
}

static inline void mlxbf_pka_dev_io_write(void __iomem *mem_ptr, u64 mem_off, u64 value)
{
	writeq_relaxed(value, mem_ptr + mem_off);
}

/*
 * Shim getter for mlxbf_pka_dev_gbl_config_t structure which holds all system
 * global configuration. This configuration is shared and common to kernel
 * device driver associated with PKA hardware.
 */
struct mlxbf_pka_dev_shim_s *mlxbf_pka_dev_get_shim(u32 shim_id);

/* Unset PKA device resource config - unmap io memory if needed. */
void mlxbf_pka_dev_unset_resource_config(struct device *dev,
					 struct mlxbf_pka_dev_shim_s *shim,
					 struct mlxbf_pka_dev_res_t *res_ptr);

/*
 * Register PKA IO block. This function initializes a shim and configures its
 * related resources, and returns the error code.
 */
int mlxbf_pka_dev_register_shim(struct device *dev,
				u32 shim_id,
				struct mlxbf_pka_dev_mem_res *mem_res,
				struct mlxbf_pka_dev_shim_s **shim);

/* Unregister PKA IO block. */
int mlxbf_pka_dev_unregister_shim(struct device *dev, struct mlxbf_pka_dev_shim_s *shim);

#endif /* __MLXBF_PKA_DEV_H__ */
