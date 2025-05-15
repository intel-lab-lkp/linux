/* SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause */
/* SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION. All rights reserved. */

#ifndef __MLXBF_PKA_DEV_H__
#define __MLXBF_PKA_DEV_H__

/*
 * @file
 *
 * API to handle the PKA EIP-154 I/O block (shim). It provides functions and data structures to
 * initialize and configure the PKA shim. It's the "southband interface" for communication with PKA
 * hardware resources.
 */

#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/ioctl.h>

/* PKA ring device related definitions. */
#define CMD_DESC_SIZE 64

/*
 * Describes the PKA command/result ring as used by the hardware. A pair of command and result rings
 * in PKA window memory, and the data memory used by the commands.
 */
struct mlxbf_pka_ring_desc_t {
	u32 num_descs; /* Total number of descriptors in the ring. */

	u32 cmd_ring_base; /* Base address of the command ring. */
	u32 cmd_idx; /* Index of the command in a ring. */

	u32 rslt_ring_base; /* Base address of the result ring. */
	u32 rslt_idx; /* Index of the result in a ring. */

	u32 operands_base; /* Operands memory base address. */
	u32 operands_end; /* End address of operands memory. */

	u32 desc_size; /* Size of each element in the ring. */

	u64 cmd_desc_mask; /* Bitmask of free(0)/in_use(1) cmd descriptors. */
	u32 cmd_desc_cnt; /* Number of command descriptors currently in use. */
	u32 rslt_desc_cnt; /* Number of result descriptors currently ready. */
};

/* This structure declares ring parameters which can be used by user interface. */
struct mlxbf_pka_ring_info_t {
	int fd; /* File descriptor. */
	int group; /* Iommu group. */
	int container; /* VFIO cointainer. */

	u32 idx; /* Ring index. */
	u32 ring_id; /* Hardware ring identifier. */

	u64 mem_off; /* Offset specific to window RAM region. */
	u64 mem_addr; /* Window RAM region address. */
	u64 mem_size; /* Window RAM region size. */

	u64 reg_off; /* Offset specific to count registers region. */
	u64 reg_addr; /* Count registers region address. */
	u64 reg_size; /* Count registers region size. */

	void *mem_ptr; /* Pointer to mapped memory region. */
	void *reg_ptr; /* Pointer to mapped counters region. */

	struct mlxbf_pka_ring_desc_t ring_desc; /* Ring descriptor. */

	u8 big_endian; /* Big endian byte order when enabled. */
};

/* PKA IOCTL related definitions. */
#define MLXBF_PKA_IOC_TYPE 0xB7

/*
 * MLXBF_PKA_RING_GET_REGION_INFO - _IORW(MLXBF_PKA_IOC_TYPE, 0x0, mlxbf_pka_dev_region_info_t).
 *
 * Retrieve information about a device region. This is intended to describe MMIO, I/O port, as well
 * as bus specific regions (ex. PCI config space). Zero sized regions may be used to describe
 * unimplemented regions.
 *
 * Return: 0 on success, -errno on failure.
 */
struct mlxbf_pka_dev_region_info_t {
	u32 reg_index; /* Registers region index. */
	u64 reg_size; /* Registers region size (bytes). */
	u64 reg_offset; /* Registers region offset from start of device fd. */

	u32 mem_index; /* Memory region index. */
	u64 mem_size; /* Memory region size (bytes). */
	u64 mem_offset; /* Memory region offset from start of device fd. */
};

#define MLXBF_PKA_RING_GET_REGION_INFO \
	_IOWR(MLXBF_PKA_IOC_TYPE, 0x0, struct mlxbf_pka_dev_region_info_t)

/*
 * MLXBF_PKA_GET_RING_INFO - _IORW(MLXBF_PKA_IOC_TYPE, 0x1, mlxbf_pka_dev_ring_info_t).
 *
 * Retrieve information about a ring. This is intended to describe ring information words located in
 * MLXBF_PKA_BUFFER_RAM. Ring information includes base addresses, size and statistics.
 *
 * Return: 0 on success, -errno on failure.
 */

/* Bluefield specific ring information. */
struct mlxbf_pka_dev_hw_ring_info_t {
	/* Base address of the command descriptor ring. */
	u64 cmmd_base;

	/* Base address of the result descriptor ring. */
	u64 rslt_base;

	/*
	 * Size of a command ring in number of descriptors, minus 1. Minimum value is 0 (for 1
	 * descriptor); maximum value is 65535 (for 64K descriptors).
	 */
	u16 size;

	/*
	 * This field specifies the size (in 32-bit words) of the space that PKI command and result
	 * descriptor occupies on the Host.
	 */
	u16 host_desc_size : 10;

	/*
	 * Indicates whether the result ring delivers results strictly in-order ('1') or that result
	 * descriptors are written to the result ring as soon as they become available, or out-of-
	 * order ('0').
	 */
	u8 in_order : 1;

	/* Read pointer of the command descriptor ring. */
	u16 cmmd_rd_ptr;

	/* Write pointer of the result descriptor ring. */
	u16 rslt_wr_ptr;

	/* Read statistics of the command descriptor ring. */
	u16 cmmd_rd_stats;

	/* Write statistics of the result descriptor ring. */
	u16 rslt_wr_stats;
};

/* ring_info related definitions. */
#define MLXBF_PKA_RING_INFO_IN_ORDER_MASK 0x0001
#define MLXBF_PKA_RING_INFO_IN_ORDER_OFFSET 31
#define MLXBF_PKA_RING_INFO_HOST_DESC_SIZE_MASK 0x03FF
#define MLXBF_PKA_RING_INFO_HOST_DESC_SIZE_OFFSET 18
#define MLXBF_PKA_RING_NUM_CMD_DESC_MASK 0xFFFF

#define MLXBF_PKA_GET_RING_INFO _IOWR(MLXBF_PKA_IOC_TYPE, 0x1, struct mlxbf_pka_dev_hw_ring_info_t)

/* Ring option related definitions. */
#define MLXBF_PKA_RING_OPTIONS_RING_PRIORITY_MASK 0xFF
#define MLXBF_PKA_RING_OPTIONS_RING_NUM_OFFSET 8
#define MLXBF_PKA_RING_OPTIONS_RING_NUM_MASK 0xFF00
#define MLXBF_PKA_RING_OPTIONS_SIGNATURE_BYTE_OFFSET 24
#define MLXBF_PKA_RING_OPTIONS_SIGNATURE_BYTE_MASK 0xFF000000

/*
 * MLXBF_PKA_CLEAR_RING_COUNTERS - _IO(MLXBF_PKA_IOC_TYPE, 0x2).
 *
 * Clear counters. This is intended to reset all command and result counters.
 *
 * Return: 0 on success, -errno on failure.
 */
#define MLXBF_PKA_CLEAR_RING_COUNTERS _IO(MLXBF_PKA_IOC_TYPE, 0x2)

/*
 * MLXBF_PKA_GET_RANDOM_BYTES - _IOWR(MLXBF_PKA_IOC_TYPE, 0x3, mlxbf_pka_dev_trng_info_t).
 *
 * Get random bytes from True Random Number Generator(TRNG).
 *
 * Return: 0 on success, -errno on failure.
 */

/* TRNG information. */
struct mlxbf_pka_dev_trng_info_t {
	/* Number of random bytes in the buffer or length of the buffer. */
	u32 count;

	/* Data buffer to hold the random bytes. */
	u8 *data;
};

#define MLXBF_PKA_GET_RANDOM_BYTES _IOWR(MLXBF_PKA_IOC_TYPE, 0x3, struct mlxbf_pka_dev_trng_info_t)

/* PKA address related definitions. */

/*
 * Global Control Space CSR addresses/offsets. These are accessed from the ARM as 8 byte reads/
 * writes. However only the bottom 32 bits are implemented.
 */
#define MLXBF_PKA_CLK_FORCE_ADDR 0x11C80

/*
 * Advanced Interrupt Controller CSR addresses/offsets. These are accessed from the ARM as 8 byte
 * reads/writes. However only the bottom 32 bits are implemented.
 */
#define MLXBF_PKA_AIC_POL_CTRL_ADDR 0x11E00

/*
 * The True Random Number Generator CSR addresses/offsets. These are accessed from the ARM as 8 byte
 * reads/writes. However only the bottom 32 bits are implemented.
 */
#define MLXBF_PKA_TRNG_OUTPUT_0_ADDR 0x12000
#define MLXBF_PKA_TRNG_STATUS_ADDR 0x12020
#define MLXBF_PKA_TRNG_INTACK_ADDR 0x12020
#define MLXBF_PKA_TRNG_CONTROL_ADDR 0x12028
#define MLXBF_PKA_TRNG_CONFIG_ADDR 0x12030
#define MLXBF_PKA_TRNG_ALARMCNT_ADDR 0x12038
#define MLXBF_PKA_TRNG_FROENABLE_ADDR 0x12040
#define MLXBF_PKA_TRNG_FRODETUNE_ADDR 0x12048
#define MLXBF_PKA_TRNG_ALARMMASK_ADDR 0x12050
#define MLXBF_PKA_TRNG_ALARMSTOP_ADDR 0x12058
#define MLXBF_PKA_TRNG_TEST_ADDR 0x120E0
#define MLXBF_PKA_TRNG_RAW_L_ADDR 0x12060
#define MLXBF_PKA_TRNG_RAW_H_ADDR 0x12068
#define MLXBF_PKA_TRNG_MONOBITCNT_ADDR 0x120B8
#define MLXBF_PKA_TRNG_POKER_3_0_ADDR 0x120C0
#define MLXBF_PKA_TRNG_PS_AI_0_ADDR 0x12080

/*
 * Control register address/offset. This is accessed from the ARM using 8 byte reads/writes. However
 * only the bottom 32 bits are implemented.
 */
#define MLXBF_PKA_MASTER_SEQ_CTRL_ADDR 0x27F90

/*
 * Ring CSRs: these are all accessed from the ARM using 8 byte reads/writes. However only the bottom
 * 32 bits are implemented.
 */
/* Ring 0 CSRS. */
#define MLXBF_PKA_COMMAND_COUNT_0_ADDR 0x80080

/* MLXBF_PKA_BUFFER_RAM: 1024 x 64 - 8K bytes. */
#define MLXBF_PKA_BUFFER_RAM_BASE 0x00000
#define MLXBF_PKA_BUFFER_RAM_SIZE SZ_16K /* 0x00000...0x03FFF. */

/*
 * PKA Buffer RAM offsets. These are NOT real CSR's but instead are specific offset/addresses within
 * the EIP154 MLXBF_PKA_BUFFER_RAM.
 */

/* Ring 0. */
#define MLXBF_PKA_RING_CMMD_BASE_0_ADDR 0x00000
#define MLXBF_PKA_RING_RSLT_BASE_0_ADDR 0x00010
#define MLXBF_PKA_RING_SIZE_TYPE_0_ADDR 0x00020
#define MLXBF_PKA_RING_RW_PTRS_0_ADDR 0x00028
#define MLXBF_PKA_RING_RW_STAT_0_ADDR 0x00030

/* Ring Options. */
#define MLXBF_PKA_RING_OPTIONS_ADDR 0x07FF8

/* Alternate Window RAM size. */
#define MLXBF_PKA_WINDOW_RAM_REGION_SIZE SZ_16K

/* PKA configuration related definitions. */

/* The maximum number of PKA shims referred to as IO blocks. */
#define MLXBF_PKA_MAX_NUM_IO_BLOCKS 24
/* The maximum number of rings supported by the IO block (shim). */
#define MLXBF_PKA_MAX_NUM_IO_BLOCK_RINGS 4

#define MLXBF_PKA_MAX_NUM_RINGS (MLXBF_PKA_MAX_NUM_IO_BLOCK_RINGS * MLXBF_PKA_MAX_NUM_IO_BLOCKS)
/*
 * Resources are regions which include info control/status words, count registers and host window
 * RAM.
 */
#define MLXBF_PKA_MAX_NUM_RING_RESOURCES 3

/*
 * PKA Ring resources.
 * Define Ring resources parameters including base address, size (in bytes) and ring spacing.
 */
#define MLXBF_PKA_RING_WORDS_ADDR MLXBF_PKA_BUFFER_RAM_BASE
#define MLXBF_PKA_RING_CNTRS_ADDR MLXBF_PKA_COMMAND_COUNT_0_ADDR

#define MLXBF_PKA_RING_WORDS_SIZE SZ_64
#define MLXBF_PKA_RING_CNTRS_SIZE SZ_32
#define MLXBF_PKA_RING_MEM_SIZE SZ_16K

#define MLXBF_PKA_RING_WORDS_SPACING SZ_64
#define MLXBF_PKA_RING_CNTRS_SPACING SZ_64K
#define MLXBF_PKA_RING_MEM_0_SPACING SZ_16K
#define MLXBF_PKA_RING_MEM_1_SPACING SZ_64K

/*
 * PKA Window RAM parameters.
 * Define whether to split window RAM during PKA device creation phase.
 */
#define MLXBF_PKA_SPLIT_WINDOW_RAM_MODE 0

/* Defines for window RAM partition, valid for 16K memory. */
#define MLXBF_PKA_WINDOW_RAM_RING_MEM_SIZE SZ_2K
#define MLXBF_PKA_WINDOW_RAM_RING_MEM_DIV 2 /* Divide into halves. */
#define MLXBF_PKA_WINDOW_RAM_DATA_MEM_SIZE 0x3800 /* 14KB. */
#define MLXBF_PKA_WINDOW_RAM_RING_ADDR_MASK 0xFFFF
#define MLXBF_PKA_WINDOW_RAM_RING_SIZE_MASK 0xF0000
#define MLXBF_PKA_WINDOW_RAM_RING_SIZE_SHIFT 2

/* Window RAM/Alternate window RAM offset mask for BF1 and BF2. */
#define MLXBF_PKA_WINDOW_RAM_OFFSET_MASK1 0x730000

/* Window RAM/Alternate window RAM offset mask for BF3. */
#define MLXBF_PKA_WINDOW_RAM_OFFSET_MASK2 0x70000

/*
 * PKA Master Sequencer Control/Status Register.
 * Writing '1' to bit [31] puts the Master controller Sequencer in a reset state. Resetting the
 * Sequencer (in order to load other firmware) should only be done when the EIP-154 is not
 * performing any operations.
 */
#define MLXBF_PKA_MASTER_SEQ_CTRL_RESET_VAL BIT(31)
/*
 * Writing '1' to bit [30] will reset all Command and Result counters. This bit is write-only and
 * self clearing and can only be set if the 'Reset' bit [31] is '1'.
 */
#define MLXBF_PKA_MASTER_SEQ_CTRL_CLEAR_COUNTERS_VAL BIT(30)
/*
 * MLXBF_PKA_RING_OPTIONS field to specify the priority in which rings are handled:
 *  '00' = full rotating priority,
 *  '01' = fixed priority (ring 0 lowest),
 *  '10' = ring 0 has the highest priority and the remaining rings have rotating priority,
 *  '11' = reserved, do not use.
 */
#define MLXBF_PKA_RING_OPTIONS_PRIORITY	0x0

/*
 * 'Signature' byte used because the ring options are transferred through RAM which does not have a
 * defined reset value. The EIP-154 master controller keeps reading the MLXBF_PKA_RING_OPTIONS word
 * at start-up until the 'Signature' byte contains 0x46 and the 'Reserved' field contains zero.
 */
#define MLXBF_PKA_RING_OPTIONS_SIGNATURE_BYTE 0x46

/*
 * Order of the result reporting: two schemas are available:
 *  InOrder    - the results will be reported in the same order as the commands were provided.
 *  OutOfOrder - the results are reported as soon as they are available.
 * Note: only the OutOfOrder schema is used in this implementation.
 */
#define MLXBF_PKA_RING_TYPE_OUT_OF_ORDER_BIT 0
#define MLXBF_PKA_RING_TYPE_IN_ORDER MLXBF_PKA_RING_TYPE_OUT_OF_ORDER_BIT

/*
 * Byte order of the data written/read to/from Rings.
 *  Little Endian (LE) - the least significant bytes have the lowest address.
 *  Big    Endian (BE) - the most significant bytes come first.
 * Note: only the little endian is used in this implementation.
 */
#define MLXBF_PKA_RING_BYTE_ORDER_LE 0
#define MLXBF_PKA_RING_BYTE_ORDER MLXBF_PKA_RING_BYTE_ORDER_LE

/*
 * 'trng_clk_on' mask for PKA Clock Switch Forcing Register. Turn on the TRNG clock. When the TRNG
 * is controlled via the host slave interface, this engine needs to be turned on by setting bit 11.
 */
#define MLXBF_PKA_CLK_FORCE_TRNG_ON 0x800

/* Number of TRNG output registers. */
#define MLXBF_PKA_TRNG_OUTPUT_CNT 4

/* Number of TRNG poker test counts. */
#define MLXBF_PKA_TRNG_POKER_TEST_CNT 4

/* TRNG configuration. */
#define MLXBF_PKA_TRNG_CONFIG_REG_VAL 0x00020008
/* TRNG Alarm Counter Register value. */
#define MLXBF_PKA_TRNG_ALARMCNT_REG_VAL 0x000200FF
/* TRNG FRO Enable Register value. */
#define MLXBF_PKA_TRNG_FROENABLE_REG_VAL 0x00FFFFFF
/*
 * TRNG Control Register value. Set bit 10 to start the EIP-76 (i.e. TRNG engine), gathering entropy
 * from the FROs.
 */
#define MLXBF_PKA_TRNG_CONTROL_REG_VAL 0x00000400

/* TRNG Control bit. */
#define MLXBF_PKA_TRNG_CONTROL_TEST_MODE 0x100

/*
 * TRNG Control Register value. Set bit 10 and 12 to start the EIP-76 (i.e. TRNG engine) with DRBG
 * enabled, gathering entropy from the FROs.
 */
#define MLXBF_PKA_TRNG_CONTROL_DRBG_REG_VAL 0x00001400

/*
 * DRBG enabled TRNG 'request_data' value. REQ_DATA_VAL (in accordance with DATA_BLOCK_MASK)
 * requests 256 blocks of 128-bit random output. 4095 blocks is the maximum number that can be
 * requested for the TRNG (with DRBG) configuration on Bluefield platforms.
 */
#define MLXBF_PKA_TRNG_CONTROL_REQ_DATA_VAL 0x10010000

/* Mask for 'Data Block' in TRNG Control Register. */
#define MLXBF_PKA_TRNG_DRBG_DATA_BLOCK_MASK 0xfff00000

/* Set bit 12 of TRNG Control Register to enable DRBG functionality. */
#define MLXBF_PKA_TRNG_CONTROL_DRBG_ENABLE_VAL BIT(12)

/* Set bit 7 (i.e. 'test_sp_800_90 DRBG' bit) in the TRNG Test Register. */
#define MLXBF_PKA_TRNG_TEST_DRBG_VAL BIT(7)

/* Number of Personalization String/Additional Input Registers. */
#define MLXBF_PKA_TRNG_PS_AI_REG_COUNT 12

/* Offset bytes of Personalization String/Additional Input Registers. */
#define MLXBF_PKA_TRNG_OUTPUT_REG_OFFSET 0x8

/* Maximum TRNG test error cycle, about one second. */
#define MLXBF_PKA_TRNG_TEST_ERR_CYCLE_MAX (1000 * 1000 * 1000)

/* DRBG Reseed enable. */
#define MLXBF_PKA_TRNG_CONTROL_DRBG_RESEED BIT(15)

/* TRNG Status bits. */
#define MLXBF_PKA_TRNG_STATUS_READY BIT(0)
#define MLXBF_PKA_TRNG_STATUS_SHUTDOWN_OFLO BIT(1)
#define MLXBF_PKA_TRNG_STATUS_TEST_READY BIT(8)
#define MLXBF_PKA_TRNG_STATUS_MONOBIT_FAIL BIT(7)
#define MLXBF_PKA_TRNG_STATUS_RUN_FAIL BIT(4)
#define MLXBF_PKA_TRNG_STATUS_POKER_FAIL BIT(6)

/* TRNG Alarm Counter bits. */
#define MLXBF_PKA_TRNG_ALARMCNT_STALL_RUN_POKER BIT(15)

/* TRNG Test bits. */
#define MLXBF_PKA_TRNG_TEST_KNOWN_NOISE BIT(5)
#define MLXBF_PKA_TRNG_TEST_NOISE BIT(13)

/* TRNG Test constants*/
#define MLXBF_PKA_TRNG_MONOBITCNT_SUM 9978

#define MLXBF_PKA_TRNG_TEST_HALF_ADD 1
#define MLXBF_PKA_TRNG_TEST_HALF_NO 0

#define MLXBF_PKA_TRNG_TEST_DATAL_BASIC_1 0x11111333
#define MLXBF_PKA_TRNG_TEST_DATAH_BASIC_1 0x3555779f
#define MLXBF_PKA_TRNG_TEST_COUNT_BASIC_1 11

#define MLXBF_PKA_TRNG_TEST_DATAL_BASIC_2 0x01234567
#define MLXBF_PKA_TRNG_TEST_DATAH_BASIC_2 0x89abcdef
#define MLXBF_PKA_TRNG_TEST_COUNT_BASIC_2 302

#define MLXBF_PKA_TRNG_TEST_DATAL_POKER 0xffffffff
#define MLXBF_PKA_TRNG_TEST_DATAH_POKER 0xffffffff
#define MLXBF_PKA_TRNG_TEST_COUNT_POKER 11

#define MLXBF_PKA_TRNG_NUM_OF_FOUR_WORD 128

/* PKA device related definitions. */
#define MLXBF_PKA_DEVFS_RING_DEVICES "mlxbf_pka/%d"

/* Device resource structure. */
struct mlxbf_pka_dev_res_t {
	void __iomem *ioaddr; /* The (iore)mapped version of addr, driver internal use. */
	u64 base; /* Base address of the device's resource. */
	u64 size; /* Size of IO. */
	u8 type; /* Type of resource addr points to. */
	s8 status; /* Status of the resource. */
	char *name; /* Name of the resource. */
};

/* Defines for mlxbf_pka_dev_res->type. */
#define MLXBF_PKA_DEV_RES_TYPE_MEM 1 /* Resource type is memory. */
#define MLXBF_PKA_DEV_RES_TYPE_REG 2 /* Resource type is register. */

/* Defines for mlxbf_pka_dev_res->status. */
#define MLXBF_PKA_DEV_RES_STATUS_MAPPED 1 /* The resource is (iore)mapped. */
#define MLXBF_PKA_DEV_RES_STATUS_UNMAPPED -1 /* The resource is unmapped. */

/* PKA Ring resources structure. */
struct mlxbf_pka_dev_ring_res_t {
	struct mlxbf_pka_dev_res_t info_words; /* Ring information words. */
	struct mlxbf_pka_dev_res_t counters; /* Ring counters. */
	struct mlxbf_pka_dev_res_t window_ram; /* Window RAM. */
};

/* PKA Ring structure. */
struct mlxbf_pka_dev_ring_t {
	u32 ring_id; /* Ring identifier. */
	struct mlxbf_pka_dev_shim_s *shim; /* Pointer to the shim associated to the ring. */
	u32 resources_num; /* Number of ring resources. */
	struct mlxbf_pka_dev_ring_res_t resources; /* Ring resources. */
	struct mlxbf_pka_dev_hw_ring_info_t *ring_info; /* Ring information. */
	u32 num_cmd_desc; /* Number of command descriptors. */
	s8 status; /* Status of the ring. */
	struct mutex mutex; /* Mutex lock for sharing ring device. */
};

/* Defines for mlxbf_pka_dev_ring->status. */
#define MLXBF_PKA_DEV_RING_STATUS_UNDEFINED -1
#define MLXBF_PKA_DEV_RING_STATUS_INITIALIZED 1
#define MLXBF_PKA_DEV_RING_STATUS_READY 2
#define MLXBF_PKA_DEV_RING_STATUS_BUSY 3

/* PKA Shim resources structure. */
struct mlxbf_pka_dev_shim_res_t {
	struct mlxbf_pka_dev_res_t buffer_ram; /* Buffer RAM. */
	struct mlxbf_pka_dev_res_t master_seq_ctrl; /* Master sequencer controller CSR. */
	struct mlxbf_pka_dev_res_t aic_csr; /* Interrupt controller CSRs. */
	struct mlxbf_pka_dev_res_t trng_csr; /* TRNG module CSRs. */
};

/* Number of PKA device resources. */
#define MLXBF_PKA_DEV_SHIM_RES_CNT 6

/* Platform global shim resource information. */
struct mlxbf_pka_dev_gbl_shim_res_info_t {
	struct mlxbf_pka_dev_res_t *res_tbl[MLXBF_PKA_DEV_SHIM_RES_CNT];
	u8 res_cnt;
};

struct mlxbf_pka_dev_mem_res {
	u64 eip154_base; /* Base address for EIP154 mmio registers. */
	u64 eip154_size; /* EIP154 mmio register region size. */

	u64 wndw_ram_off_mask; /* Common offset mask for alt window RAM and window RAM. */
	u64 wndw_ram_base; /* Base address for window RAM. */
	u64 wndw_ram_size; /* Window RAM region size. */

	u64 alt_wndw_ram_0_base; /* Base address for alternate window RAM 0. */
	u64 alt_wndw_ram_1_base; /* Base address for alternate window RAM 1. */
	u64 alt_wndw_ram_2_base; /* Base address for alternate window RAM 2. */
	u64 alt_wndw_ram_3_base; /* Base address for alternate window RAM 3. */
	u64 alt_wndw_ram_size; /* Alternate window RAM regions size. */

	u64 csr_base; /* Base address for CSR registers. */
	u64 csr_size; /* CSR area size. */
};

/* PKA Shim structure. */
struct mlxbf_pka_dev_shim_s {
	struct	mlxbf_pka_dev_mem_res mem_res;
	u64 trng_err_cycle; /* TRNG error cycle. */
	u32 shim_id; /* Shim identifier. */
	u32 rings_num; /* Number of supported rings (hardware specific). */
	struct mlxbf_pka_dev_ring_t **rings; /* Pointer to rings which belong to the shim. */
	u8 ring_priority; /* Specify the priority in which rings are handled. */
	u8 ring_type; /*Indicates whether the result ring delivers results strictly in-order. */
	struct mlxbf_pka_dev_shim_res_t resources; /* Shim resources. */
	u8 window_ram_split; /* If non-zero, the split window RAM scheme is used. */
	u32 busy_ring_num; /* Number of active rings (rings in busy state). */
	u8 trng_enabled; /* Whether the TRNG engine is enabled. */
	s8 status; /* Status of the shim. */
	struct mutex mutex; /* Mutex lock for sharing shim. */
};

/* Defines for mlxbf_pka_dev_shim->status. */
#define MLXBF_PKA_SHIM_STATUS_UNDEFINED -1
#define MLXBF_PKA_SHIM_STATUS_CREATED 1
#define MLXBF_PKA_SHIM_STATUS_INITIALIZED 2
#define MLXBF_PKA_SHIM_STATUS_RUNNING 3
#define MLXBF_PKA_SHIM_STATUS_STOPPED 4
#define MLXBF_PKA_SHIM_STATUS_FINALIZED 5

/* Defines for mlxbf_pka_dev_shim->window_ram_split. */

/* Window RAM is split into 4 * 16KB blocks. */
#define MLXBF_PKA_SHIM_WINDOW_RAM_SPLIT_ENABLED 1
/* Window RAM is not split and occupies 64KB. */
#define MLXBF_PKA_SHIM_WINDOW_RAM_SPLIT_DISABLED 2

/* Defines for mlxbf_pka_dev_shim->trng_enabled. */
#define MLXBF_PKA_SHIM_TRNG_ENABLED 1
#define MLXBF_PKA_SHIM_TRNG_DISABLED 0

/* Platform global configuration structure. */
struct mlxbf_pka_dev_gbl_config_t {
	u32 dev_shims_cnt; /* Number of registered PKA shims. */
	u32 dev_rings_cnt; /* Number of registered Rings. */

	/* Table of registered PKA shims. */
	struct mlxbf_pka_dev_shim_s *dev_shims[MLXBF_PKA_MAX_NUM_IO_BLOCKS];

	/* Table of registered Rings. */
	struct mlxbf_pka_dev_ring_t *dev_rings[MLXBF_PKA_MAX_NUM_RINGS];
};

extern struct mlxbf_pka_dev_gbl_config_t mlxbf_pka_gbl_config;

/* Processor speed in hertz, used in routines which might be called very early in boot. */
static inline u64 mlxbf_pka_early_cpu_speed(void)
{
	/*
	 * Initial guess at our CPU speed.  We set this to be larger than any possible real speed,
	 * so that any calculated delays will be too long, rather than too short.
	 *
	 * CPU Freq for High/Bin Chip - 1.255GHz.
	 */
	return 1255 * 1000 * 1000;
}

/*
 * Ring getter for mlxbf_pka_dev_gbl_config_t structure which holds all system global configuration.
 * This configuration is shared and common to kernel device driver associated with PKA hardware.
 */
struct mlxbf_pka_dev_ring_t *mlxbf_pka_dev_get_ring(u32 ring_id);

/*
 * Shim getter for mlxbf_pka_dev_gbl_config_t structure which holds all system global configuration.
 * This configuration is shared and common to kernel device driver associated with PKA hardware.
 */
struct mlxbf_pka_dev_shim_s *mlxbf_pka_dev_get_shim(u32 shim_id);

/*
 * Register a ring. This function initializes a Ring and configures its related resources, and
 * returns a pointer to that ring.
 */
struct mlxbf_pka_dev_ring_t *mlxbf_pka_dev_register_ring(struct device *dev,
							 u32 ring_id,
							 u32 shim_id);

/* Unregister a Ring. */
int mlxbf_pka_dev_unregister_ring(struct mlxbf_pka_dev_ring_t *ring);

/*
 * Register PKA IO block. This function initializes a shim and configures its related resources, and
 * returns a pointer to that ring.
 */
struct mlxbf_pka_dev_shim_s *mlxbf_pka_dev_register_shim(struct device *dev,
							 u32 shim_id,
							 struct mlxbf_pka_dev_mem_res *mem_res);

/* Unregister PKA IO block. */
int mlxbf_pka_dev_unregister_shim(struct mlxbf_pka_dev_shim_s *shim);

/* Reset a Ring. */
int mlxbf_pka_dev_reset_ring(struct mlxbf_pka_dev_ring_t *ring);

/*
 * Clear ring counters. This function resets the master sequencer controller to clear the command
 * and result counters.
 */
int mlxbf_pka_dev_clear_ring_counters(struct mlxbf_pka_dev_ring_t *ring);

/*
 * Read data from the TRNG. Drivers can fill up to 'cnt' bytes of data into the buffer 'data'. The
 * buffer 'data' is aligned for any type and 'cnt' is a multiple of 4.
 */
int mlxbf_pka_dev_trng_read(struct mlxbf_pka_dev_shim_s *shim, u32 *data, u32 cnt);

/* Return true if the TRNG engine is enabled, false if not. */
bool mlxbf_pka_dev_has_trng(struct mlxbf_pka_dev_shim_s *shim);

/*
 * Open the file descriptor associated with ring. It returns an integer value, which is used to
 * refer to the file. If not successful, it returns a negative error.
 */
int mlxbf_pka_dev_open_ring(struct mlxbf_pka_ring_info_t *ring_info);

/*
 * Close the file descriptor associated with ring. The function returns 0 if successful, negative
 * value to indicate an error.
 */
int mlxbf_pka_dev_close_ring(struct mlxbf_pka_ring_info_t *ring_info);

#endif /* __MLXBF_PKA_DEV_H__ */
