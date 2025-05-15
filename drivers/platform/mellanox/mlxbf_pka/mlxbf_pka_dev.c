// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION. All rights reserved.

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/ioport.h>
#include <linux/timex.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/io.h>

#include "mlxbf_pka_dev.h"

#define MASTER_CONTROLLER_OUT_OF_RESET 0

/* Personalization string "NVIDIA-MELLANOX-BLUEFIELD-TRUE_RANDOM_NUMBER_GEN". */
static u32 mlxbf_pka_trng_drbg_ps_str[] = {
	0x4e564944, 0x49412d4d, 0x454c4c41, 0x4e4f582d,
	0x424c5545, 0x4649454c, 0x442d5452, 0x55455f52,
	0x414e444f, 0x4d5f4e55, 0x4d424552, 0x5f47454e
};

/* Personalization string for DRBG test. */
static u32 mlxbf_pka_trng_drbg_test_ps_str[] = {
	0x64299d83, 0xc34d7098, 0x5bd1f51d, 0xddccfdc1,
	0xdd0455b7, 0x166279e5, 0x0974cb1b, 0x2f2cd100,
	0x59a5060a, 0xca79940d, 0xd4e29a40, 0x56b7b779
};

/* First Entropy string for DRBG test. */
static u32 mlxbf_pka_trng_drbg_test_etpy_str1[] = {
	0xaa6bbcab, 0xef45e339, 0x136ca1e7, 0xbce1c881,
	0x9fa37b09, 0x63b53667, 0xb36e0053, 0xa202ed81,
	0x4650d90d, 0x8eed6127, 0x666f2402, 0x0dfd3af9
};

/* Second Entropy string for DRBG test. */
static u32 mlxbf_pka_trng_drbg_test_etpy_str2[] = {
	0x35c1b7a1, 0x0154c52b, 0xd5777390, 0x226a4fdb,
	0x5f16080d, 0x06b68369, 0xd0c93d00, 0x3336e27f,
	0x1abf2c37, 0xe6ab006c, 0xa4adc6e1, 0x8e1907a2
};

/* Known answer for DRBG test. */
static u32 mlxbf_pka_trng_drbg_test_output[] = {
	0xb663b9f1, 0x24943e13, 0x80f7dce5, 0xaba1a16f
};

/* Known answer for poker test. */
static u64 poker_test_exp_cnt[] = {
	0x20f42bf4, 0xaf415f4, 0xf4f4fff4, 0xfff4f4f4
};

struct mlxbf_pka_dev_gbl_config_t mlxbf_pka_gbl_config;

/* Global PKA shim resource info table. */
static struct mlxbf_pka_dev_gbl_shim_res_info_t mlxbf_pka_gbl_res_tbl[MLXBF_PKA_MAX_NUM_IO_BLOCKS];

/* Start a PKA device timer. */
static inline u64 mlxbf_pka_dev_timer_start_msec(u32 msec)
{
	u64 cur_time = get_cycles();

	return (cur_time + (mlxbf_pka_early_cpu_speed() * msec) / MSEC_PER_SEC);
}

/* Test a PKA device timer for completion. */
static inline int mlxbf_pka_dev_timer_done(u64 timer)
{
	return (get_cycles() >= timer);
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
 * Mapping PKA Ring address into Window RAM address.
 * It converts the ring address, either physical address or virtual address, to valid address into
 * the Window RAM. This is done assuming the Window RAM base, size and mask. Here, base is the
 * actual physical address of the Window RAM, with the help of mask it is reduced to Window RAM
 * offset within that PKA block. Further, with the help of addr and size, we arrive at the Window
 * RAM offset address for a PKA Ring within the given Window RAM.
 */
static inline u64 mlxbf_pka_ring_mem_addr(u64 base, u64 mask, u64 addr, u64 size)
{
	return ((base) & (mask)) |
	       (((addr) & MLXBF_PKA_WINDOW_RAM_RING_ADDR_MASK) |
	       ((((addr) & ~((size) - 1)) & MLXBF_PKA_WINDOW_RAM_RING_SIZE_MASK) >>
	       MLXBF_PKA_WINDOW_RAM_RING_SIZE_SHIFT));
}

/* Add the resource to the global resource table. */
static int mlxbf_pka_dev_add_resource(struct mlxbf_pka_dev_res_t *res_ptr, u32 shim_idx)
{
	u8 res_cnt;

	res_cnt = mlxbf_pka_gbl_res_tbl[shim_idx].res_cnt;
	if (res_cnt >= MLXBF_PKA_DEV_SHIM_RES_CNT)
		return -ENOMEM;

	mlxbf_pka_gbl_res_tbl[shim_idx].res_tbl[res_cnt] = res_ptr;
	mlxbf_pka_gbl_res_tbl[shim_idx].res_cnt++;

	return 0;
}

/* Remove the resource from the global resource table. */
static int mlxbf_pka_dev_put_resource(struct mlxbf_pka_dev_res_t *res, u32 shim_idx)
{
	struct mlxbf_pka_dev_res_t *res_ptr;
	u8 res_idx;

	for (res_idx = 0; res_idx < MLXBF_PKA_DEV_SHIM_RES_CNT; res_idx++) {
		res_ptr = mlxbf_pka_gbl_res_tbl[shim_idx].res_tbl[res_idx];
		if (res_ptr && strcmp(res_ptr->name, res->name) == 0) {
			mlxbf_pka_gbl_res_tbl[shim_idx].res_tbl[res_idx] = NULL;
			mlxbf_pka_gbl_res_tbl[shim_idx].res_cnt--;
			break;
		}
	}

	/*
	 * Check whether the resource shares the same memory map; If so, the memory map shouldn't
	 * be released.
	 */
	for (res_idx = 0; res_idx < MLXBF_PKA_DEV_SHIM_RES_CNT; res_idx++) {
		res_ptr = mlxbf_pka_gbl_res_tbl[shim_idx].res_tbl[res_idx];
		if (res_ptr && res_ptr->base == res->base)
			return -EBUSY;
	}

	return 0;
}

static void __iomem *mlxbf_pka_dev_get_resource_ioaddr(u64 res_base, u32 shim_idx)
{
	struct mlxbf_pka_dev_res_t *res_ptr;
	u8 res_cnt, res_idx;

	res_cnt = mlxbf_pka_gbl_res_tbl[shim_idx].res_cnt;
	if (!res_cnt)
		return NULL;

	for (res_idx = 0; res_idx < res_cnt; res_idx++) {
		res_ptr = mlxbf_pka_gbl_res_tbl[shim_idx].res_tbl[res_idx];
		if (res_ptr->base == res_base)
			return res_ptr->ioaddr;
	}

	return NULL;
}

/* Set PKA device resource config and map io memory if needed. */
static int mlxbf_pka_dev_set_resource_config(struct device *dev,
					     struct mlxbf_pka_dev_shim_s *shim,
					     struct mlxbf_pka_dev_res_t *res_ptr,
					     u64 res_base,
					     u64 res_size,
					     u64 res_type,
					     char *res_name)
{
	if (res_ptr->status == MLXBF_PKA_DEV_RES_STATUS_MAPPED)
		return -EPERM;

	switch (res_type) {
	case MLXBF_PKA_DEV_RES_TYPE_REG:
		res_ptr->base = res_base;
		break;
	case MLXBF_PKA_DEV_RES_TYPE_MEM:
		res_ptr->base = shim->mem_res.eip154_base + res_base;
		break;
	default:
		return -EINVAL;
	}

	res_ptr->size = res_size;
	res_ptr->type = res_type;
	res_ptr->name = res_name;
	res_ptr->status = MLXBF_PKA_DEV_RES_STATUS_UNMAPPED;
	res_ptr->ioaddr = mlxbf_pka_dev_get_resource_ioaddr(res_ptr->base, shim->shim_id);
	if (!res_ptr->ioaddr) {
		if (!devm_request_mem_region(dev, res_ptr->base, res_ptr->size, res_ptr->name)) {
			dev_err(dev, "failed to get io memory region\n");
			return -EPERM;
		}

		res_ptr->ioaddr = devm_ioremap(dev, res_ptr->base, res_ptr->size);
		if (!res_ptr->ioaddr) {
			dev_err(dev, "unable to map io memory into CPU space\n");
			return -ENOMEM;
		}
	}

	res_ptr->status = MLXBF_PKA_DEV_RES_STATUS_MAPPED;

	if (!res_ptr->ioaddr || mlxbf_pka_dev_add_resource(res_ptr, shim->shim_id)) {
		dev_err(dev, "unable to map io memory\n");
		return -ENOMEM;
	}

	return 0;
}

/* Unset PKA device resource config - unmap io memory if needed. */
static void mlxbf_pka_dev_unset_resource_config(struct mlxbf_pka_dev_shim_s *shim,
						struct mlxbf_pka_dev_res_t *res_ptr)
{
	if (res_ptr->status != MLXBF_PKA_DEV_RES_STATUS_MAPPED)
		return;

	if (res_ptr->ioaddr && -EBUSY != mlxbf_pka_dev_put_resource(res_ptr, shim->shim_id)) {
		pr_debug("mlxbf_pka: PKA device resource released\n");
		res_ptr->status = MLXBF_PKA_DEV_RES_STATUS_UNMAPPED;
	}
}

int mlxbf_pka_dev_clear_ring_counters(struct mlxbf_pka_dev_ring_t *ring)
{
	struct mlxbf_pka_dev_res_t *master_seq_ctrl_ptr;
	u64 master_reg_base, master_reg_off;
	struct mlxbf_pka_dev_shim_s *shim;
	void __iomem *master_reg_ptr;

	shim = ring->shim;
	master_seq_ctrl_ptr = &shim->resources.master_seq_ctrl;
	master_reg_base = master_seq_ctrl_ptr->base;
	master_reg_ptr = master_seq_ctrl_ptr->ioaddr;
	master_reg_off = mlxbf_pka_dev_get_register_offset(master_reg_base,
							   MLXBF_PKA_MASTER_SEQ_CTRL_ADDR);

	/* Push the EIP-154 master controller into reset. */
	mlxbf_pka_dev_io_write(master_reg_ptr, master_reg_off, MLXBF_PKA_MASTER_SEQ_CTRL_RESET_VAL);

	/* Clear counters. */
	mlxbf_pka_dev_io_write(master_reg_ptr, master_reg_off,
			       MLXBF_PKA_MASTER_SEQ_CTRL_CLEAR_COUNTERS_VAL);

	/* Take the EIP-154 master controller out of reset. */
	mlxbf_pka_dev_io_write(master_reg_ptr, master_reg_off, MASTER_CONTROLLER_OUT_OF_RESET);

	return 0;
}

/*
 * Initialize ring. Set ring parameters and configure ring resources. It returns 0 on success, a
 * negative error code on failure.
 */
static int mlxbf_pka_dev_init_ring(struct device *dev,
				   struct mlxbf_pka_dev_ring_t *ring,
				   u32 ring_id,
				   struct mlxbf_pka_dev_shim_s *shim)
{
	struct mlxbf_pka_dev_res_t *ring_window_ram_ptr;
	struct mlxbf_pka_dev_res_t *ring_info_words_ptr;
	struct mlxbf_pka_dev_res_t *ring_counters_ptr;
	u8 window_ram_split;
	u32 ring_words_off;
	u32 ring_cntrs_off;
	u32 ring_mem_base;
	u32 ring_mem_off;
	u32 shim_ring_id;

	if (ring->status != MLXBF_PKA_DEV_RING_STATUS_UNDEFINED) {
		dev_err(dev, "PKA ring must be undefined\n");
		return -EPERM;
	}

	if (ring_id > MLXBF_PKA_MAX_NUM_RINGS - 1) {
		dev_err(dev, "invalid ring identifier\n");
		return -EINVAL;
	}

	ring->ring_id = ring_id;
	ring->shim = shim;
	ring->resources_num = MLXBF_PKA_MAX_NUM_RING_RESOURCES;
	shim_ring_id = ring_id % MLXBF_PKA_MAX_NUM_IO_BLOCK_RINGS;
	shim->rings[shim_ring_id] = ring;

	/* Configure ring information control/status words resource. */
	ring_info_words_ptr = &ring->resources.info_words;
	ring_words_off = shim_ring_id * MLXBF_PKA_RING_WORDS_SPACING;
	ring_info_words_ptr->base = ring_words_off + shim->mem_res.eip154_base +
				    MLXBF_PKA_RING_WORDS_ADDR;
	ring_info_words_ptr->size = MLXBF_PKA_RING_WORDS_SIZE;
	ring_info_words_ptr->type = MLXBF_PKA_DEV_RES_TYPE_MEM;
	ring_info_words_ptr->status = MLXBF_PKA_DEV_RES_STATUS_UNMAPPED;
	ring_info_words_ptr->name = "MLXBF_PKA_RING_INFO";

	/* Configure ring counters registers resource. */
	ring_counters_ptr = &ring->resources.counters;
	ring_cntrs_off = shim_ring_id * MLXBF_PKA_RING_CNTRS_SPACING;
	ring_counters_ptr->base = ring_cntrs_off + shim->mem_res.eip154_base +
				  MLXBF_PKA_RING_CNTRS_ADDR;
	ring_counters_ptr->size = MLXBF_PKA_RING_CNTRS_SIZE;
	ring_counters_ptr->type = MLXBF_PKA_DEV_RES_TYPE_REG;
	ring_counters_ptr->status = MLXBF_PKA_DEV_RES_STATUS_UNMAPPED;
	ring_counters_ptr->name = "MLXBF_PKA_RING_CNTRS";

	/* Configure ring window RAM resource. */
	window_ram_split = shim->window_ram_split;
	if (window_ram_split == MLXBF_PKA_SHIM_WINDOW_RAM_SPLIT_ENABLED) {
		ring_mem_off = shim_ring_id * MLXBF_PKA_RING_MEM_1_SPACING;
		ring_mem_base = ring_mem_off + shim->mem_res.alt_wndw_ram_0_base;
	} else {
		ring_mem_off = shim_ring_id * MLXBF_PKA_RING_MEM_0_SPACING;
		ring_mem_base = ring_mem_off + shim->mem_res.wndw_ram_base;
	}

	ring_window_ram_ptr = &ring->resources.window_ram;
	ring_window_ram_ptr->base = ring_mem_base;
	ring_window_ram_ptr->size = MLXBF_PKA_RING_MEM_SIZE;
	ring_window_ram_ptr->type = MLXBF_PKA_DEV_RES_TYPE_MEM;
	ring_window_ram_ptr->status = MLXBF_PKA_DEV_RES_STATUS_UNMAPPED;
	ring_window_ram_ptr->name = "MLXBF_PKA_RING_WINDOW";

	ring->ring_info = devm_kzalloc(dev, sizeof(*ring->ring_info), GFP_KERNEL);
	if (!ring->ring_info)
		return -ENOMEM;

	mutex_init(&ring->mutex);
	ring->status = MLXBF_PKA_DEV_RING_STATUS_INITIALIZED;

	return 0;
}

/* Release a given Ring. */
static int mlxbf_pka_dev_release_ring(struct mlxbf_pka_dev_ring_t *ring)
{
	struct mlxbf_pka_dev_shim_s *shim;
	u32 shim_ring_id;

	if (ring->status == MLXBF_PKA_DEV_RING_STATUS_UNDEFINED)
		return 0;

	if (ring->status == MLXBF_PKA_DEV_RING_STATUS_BUSY) {
		pr_err("mlxbf_pka error: PKA ring is busy\n");
		return -EBUSY;
	}

	shim = ring->shim;

	if (shim->status == MLXBF_PKA_SHIM_STATUS_RUNNING) {
		pr_err("mlxbf_pka error: PKA shim is running\n");
		return -EPERM;
	}

	mlxbf_pka_dev_unset_resource_config(shim, &ring->resources.info_words);
	mlxbf_pka_dev_unset_resource_config(shim, &ring->resources.counters);
	mlxbf_pka_dev_unset_resource_config(shim, &ring->resources.window_ram);

	ring->status = MLXBF_PKA_DEV_RING_STATUS_UNDEFINED;
	shim_ring_id = ring->ring_id % MLXBF_PKA_MAX_NUM_IO_BLOCK_RINGS;
	shim->rings[shim_ring_id] = NULL;
	shim->rings_num--;

	return 0;
}

/*
 * Partition the window RAM for a given PKA ring.  Here we statically divide the 16K memory region
 * into three partitions:  First partition is reserved for command descriptor ring (1K), second
 * partition is reserved for result descriptor ring (1K), and the remaining 14K are reserved for
 * vector data. Through this memory partition scheme, command/result descriptor rings hold a total
 * of 1KB/64B = 16 descriptors each. The addresses for the rings start at offset 0x3800.  Also note
 * that it is possible to have rings full while the vector data can support more data,  the opposite
 * can also happen, but it is not suitable. For instance ECC point multiplication requires 8 input
 * vectors and 2 output vectors, a total of 10 vectors. If each vector has a length of 24 words
 * (24x4B = 96B), we can process 14KB/960B = 14 operations which is close to 16 the total
 * descriptors supported by rings. On the other hand, using 12K vector data region, allows to
 * process only 12 operations, while rings can hold 32 descriptors (ring usage is significantly
 * low).
 *
 * For ECDSA verify, we have 12 vectors which require 1152B, with 14KB we can handle 12 operations,
 * against 10 operations with 12KB vector data memory. We believe that the aforementioned memory
 * partition help us to leverage the trade-off between supported descriptors and required vectors.
 * Note that these examples give approximative values and does not include buffer word padding
 * across vectors.
 *
 * The function also writes the result descriptor rings base addresses, size and type. And
 * initialize the read and write pointers and statistics. It returns 0 on success, a negative error
 * code on failure.
 *
 * This function must be called once per ring, at initialization before any other functions are
 * called.
 */
static int mlxbf_pka_dev_partition_mem(struct mlxbf_pka_dev_ring_t *ring)
{
	u64 rslt_desc_ring_base;
	u64 cmd_desc_ring_base;
	u32 cmd_desc_ring_size;
	u64 window_ram_base;
	u64 window_ram_size;
	u32 ring_mem_base;

	if (!ring->shim || ring->status != MLXBF_PKA_DEV_RING_STATUS_INITIALIZED)
		return -EPERM;

	window_ram_base = ring->resources.window_ram.base;
	window_ram_size = ring->resources.window_ram.size;
	/*
	 * Partition ring memory.  Give ring pair (cmmd descriptor ring and rslt descriptor ring) an
	 * equal portion of the memory.  The cmmd descriptor ring and result descriptor ring are
	 * used as "non-overlapping" ring. Currently set aside 1/8 of the window RAM for command and
	 * result descriptor rings - giving a total of 1K/64B = 16 descriptors per ring. The
	 * remaining memory is "Data Memory" - i.e. memory to hold the command operands and results
	 * - also called input/output vectors (in all cases these vectors are just single large
	 * integers - often in the range of hundreds to thousands of bits long).
	 */
	ring_mem_base = window_ram_base + MLXBF_PKA_WINDOW_RAM_DATA_MEM_SIZE;
	cmd_desc_ring_size = MLXBF_PKA_WINDOW_RAM_RING_MEM_SIZE /
			     MLXBF_PKA_WINDOW_RAM_RING_MEM_DIV;
	ring->num_cmd_desc = MLXBF_PKA_WINDOW_RAM_RING_MEM_SIZE /
			     MLXBF_PKA_WINDOW_RAM_RING_MEM_DIV / CMD_DESC_SIZE;
	/*
	 * The command and result descriptor rings may be placed at different (non-overlapping)
	 * locations in Window RAM memory space. PKI command interface: Most of the functionality is
	 * defined by the EIP-154 master firmware on the EIP-154 master controller Sequencer.
	 */
	cmd_desc_ring_base = ring_mem_base;
	rslt_desc_ring_base = ring_mem_base + cmd_desc_ring_size;

	cmd_desc_ring_base = mlxbf_pka_ring_mem_addr(window_ram_base,
						     ring->shim->mem_res.wndw_ram_off_mask,
						     cmd_desc_ring_base,
						     window_ram_size);
	rslt_desc_ring_base = mlxbf_pka_ring_mem_addr(window_ram_base,
						      ring->shim->mem_res.wndw_ram_off_mask,
						      rslt_desc_ring_base,
						      window_ram_size);

	/* Fill ring information. */
	memset(ring->ring_info, 0, sizeof(struct mlxbf_pka_dev_hw_ring_info_t));

	ring->ring_info->cmmd_base = cmd_desc_ring_base;
	ring->ring_info->rslt_base = rslt_desc_ring_base;
	ring->ring_info->size = MLXBF_PKA_WINDOW_RAM_RING_MEM_SIZE /
				MLXBF_PKA_WINDOW_RAM_RING_MEM_DIV / CMD_DESC_SIZE - 1;
	ring->ring_info->host_desc_size = CMD_DESC_SIZE / sizeof(u32);
	ring->ring_info->in_order = ring->shim->ring_type;

	return 0;
}

/*
 * Write the ring base address, ring size and type, and initialize (clear) the read and write
 * pointers and statistics.
 */
static int mlxbf_pka_dev_write_ring_info(struct mlxbf_pka_dev_res_t *buffer_ram_ptr,
					 u8 ring_id,
					 u32 ring_cmmd_base_val,
					 u32 ring_rslt_base_val,
					 u32 ring_size_type_val)
{
	u32 ring_spacing;
	u64 word_off;

	if (buffer_ram_ptr->status != MLXBF_PKA_DEV_RES_STATUS_MAPPED ||
	    buffer_ram_ptr->type != MLXBF_PKA_DEV_RES_TYPE_MEM)
		return -EPERM;

	pr_debug("mlxbf_pka: writing ring information control/status words\n");

	ring_spacing = ring_id * MLXBF_PKA_RING_WORDS_SPACING;
	/*
	 * Write the command ring base address that the EIP-154 master firmware uses with the
	 * command ring read pointer to get command descriptors from the Host ring. After the
	 * initialization, although the word is writeable it should be regarded as read-only.
	 */
	word_off = mlxbf_pka_dev_get_word_offset(buffer_ram_ptr->base,
						 MLXBF_PKA_RING_CMMD_BASE_0_ADDR + ring_spacing,
						 MLXBF_PKA_BUFFER_RAM_SIZE);
	mlxbf_pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, ring_cmmd_base_val);

	/*
	 * Write the result ring base address that the EIP-154 master firmware uses with the result
	 * ring write pointer to put the result descriptors in the Host ring. After the
	 * initialization, although the word is writeable it should be regarded as read-only.
	 */
	word_off = mlxbf_pka_dev_get_word_offset(buffer_ram_ptr->base,
						 MLXBF_PKA_RING_RSLT_BASE_0_ADDR + ring_spacing,
						 MLXBF_PKA_BUFFER_RAM_SIZE);
	mlxbf_pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, ring_rslt_base_val);

	/*
	 * Write the ring size (number of descriptors), the size of the descriptor and the result
	 * reporting scheme. After the initialization, although the word is writeable it should be
	 * regarded as read-only.
	 */
	word_off = mlxbf_pka_dev_get_word_offset(buffer_ram_ptr->base,
						 MLXBF_PKA_RING_SIZE_TYPE_0_ADDR + ring_spacing,
						 MLXBF_PKA_BUFFER_RAM_SIZE);
	mlxbf_pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, ring_size_type_val);

	/*
	 * Write the command and result ring indices that the EIP-154 master firmware uses. This
	 * word should be written with zero when the ring information is initialized. After the
	 * initialization, although the word is writeable it should be regarded as read-only.
	 */
	word_off = mlxbf_pka_dev_get_word_offset(buffer_ram_ptr->base,
						 MLXBF_PKA_RING_RW_PTRS_0_ADDR + ring_spacing,
						 MLXBF_PKA_BUFFER_RAM_SIZE);
	mlxbf_pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, 0);

	/*
	 * Write the ring statistics (two 16-bit counters, one for commands and one for results)
	 * from EIP-154 master firmware point of view. This word should be written with zero when
	 * the ring information is initialized. After the initialization, although the word is
	 * writeable it should be regarded as read-only.
	 */
	word_off = mlxbf_pka_dev_get_word_offset(buffer_ram_ptr->base,
						 MLXBF_PKA_RING_RW_STAT_0_ADDR + ring_spacing,
						 MLXBF_PKA_BUFFER_RAM_SIZE);
	mlxbf_pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, 0);

	return 0;
}

/*
 * Set up the control/status words. Upon a PKI command the EIP-154 master firmware will read and
 * partially update the ring information.
 */
static int mlxbf_pka_dev_set_ring_info(struct mlxbf_pka_dev_ring_t *ring)
{
	u32 ring_cmmd_base_val;
	u32 ring_rslt_base_val;
	u32 ring_size_type_val;
	int ret;

	/* Ring info configuration MUST be done when the PKA ring is initialized. */
	if ((ring->shim->status != MLXBF_PKA_SHIM_STATUS_INITIALIZED &&
	     ring->shim->status != MLXBF_PKA_SHIM_STATUS_RUNNING &&
	     ring->shim->status != MLXBF_PKA_SHIM_STATUS_STOPPED) ||
	     ring->status != MLXBF_PKA_DEV_RING_STATUS_INITIALIZED)
		return -EPERM;

	/* Partition ring memory. */
	ret = mlxbf_pka_dev_partition_mem(ring);
	if (ret) {
		pr_err("mlxbf_pka error: failed to initialize ring memory\n");
		return ret;
	}

	/* Fill ring information. */
	ring_cmmd_base_val = ring->ring_info->cmmd_base;
	ring_rslt_base_val = ring->ring_info->rslt_base;
	ring_size_type_val = (ring->ring_info->in_order &
			     MLXBF_PKA_RING_INFO_IN_ORDER_MASK) <<
			     MLXBF_PKA_RING_INFO_IN_ORDER_OFFSET;
	ring_size_type_val |= (ring->ring_info->host_desc_size &
			      MLXBF_PKA_RING_INFO_HOST_DESC_SIZE_MASK) <<
			      MLXBF_PKA_RING_INFO_HOST_DESC_SIZE_OFFSET;
	ring_size_type_val |= (ring->num_cmd_desc - 1) & MLXBF_PKA_RING_NUM_CMD_DESC_MASK;

	/* Write ring information status/control words in the PKA Buffer RAM. */
	ret = mlxbf_pka_dev_write_ring_info(&ring->shim->resources.buffer_ram,
					    ring->ring_id % MLXBF_PKA_MAX_NUM_IO_BLOCK_RINGS,
					    ring_cmmd_base_val,
					    ring_rslt_base_val,
					    ring_size_type_val);
	if (ret) {
		pr_err("mlxbf_pka error: failed to write ring information\n");
		return ret;
	}

	ring->status = MLXBF_PKA_DEV_RING_STATUS_READY;

	return ret;
}

/*
 * Create shim. Set shim parameters and configure shim resources. It returns 0 on success, a
 * negative error code on failure.
 */
static int mlxbf_pka_dev_create_shim(struct device *dev,
				     struct mlxbf_pka_dev_shim_s *shim,
				     u32 shim_id,
				     u8 split,
				     struct mlxbf_pka_dev_mem_res *mem_res)
{
	u64 reg_base;
	u64 reg_size;
	int ret;

	if (shim->status == MLXBF_PKA_SHIM_STATUS_CREATED)
		return 0;

	if (shim->status != MLXBF_PKA_SHIM_STATUS_UNDEFINED) {
		dev_err(dev, "PKA device must be undefined\n");
		return -EPERM;
	}

	if (shim_id > MLXBF_PKA_MAX_NUM_IO_BLOCKS - 1) {
		dev_err(dev, "invalid shim identifier\n");
		return -EINVAL;
	}

	shim->shim_id = shim_id;
	shim->mem_res = *mem_res;

	if (split)
		shim->window_ram_split = MLXBF_PKA_SHIM_WINDOW_RAM_SPLIT_ENABLED;
	else
		shim->window_ram_split = MLXBF_PKA_SHIM_WINDOW_RAM_SPLIT_DISABLED;

	shim->ring_type = MLXBF_PKA_RING_TYPE_IN_ORDER;
	shim->ring_priority = MLXBF_PKA_RING_OPTIONS_PRIORITY;
	shim->rings_num = MLXBF_PKA_MAX_NUM_IO_BLOCK_RINGS;
	shim->rings = devm_kzalloc(dev,
				   shim->rings_num * sizeof(struct mlxbf_pka_dev_ring_t),
				   GFP_KERNEL);
	if (!shim->rings) {
		dev_err(dev, "unable to allocate memory for ring\n");
		return -ENOMEM;
	}

	/* Set PKA device Buffer RAM config. */
	ret = mlxbf_pka_dev_set_resource_config(dev,
						shim,
						&shim->resources.buffer_ram,
						MLXBF_PKA_BUFFER_RAM_BASE,
						MLXBF_PKA_BUFFER_RAM_SIZE,
						MLXBF_PKA_DEV_RES_TYPE_MEM,
						"MLXBF_PKA_BUFFER_RAM");
	if (ret) {
		dev_err(dev, "unable to set Buffer RAM config\n");
		return ret;
	}

	/* Set PKA device Master Controller register. */
	reg_size = PAGE_SIZE;
	reg_base = mlxbf_pka_dev_get_register_base(shim->mem_res.eip154_base,
						   MLXBF_PKA_MASTER_SEQ_CTRL_ADDR);
	ret = mlxbf_pka_dev_set_resource_config(dev,
						shim,
						&shim->resources.master_seq_ctrl,
						reg_base,
						reg_size,
						MLXBF_PKA_DEV_RES_TYPE_REG,
						"MLXBF_PKA_MASTER_SEQ_CTRL");
	if (ret) {
		dev_err(dev, "unable to set Master Controller register config\n");
		return ret;
	}

	/* Set PKA device AIC registers. */
	reg_size = PAGE_SIZE;
	reg_base = mlxbf_pka_dev_get_register_base(shim->mem_res.eip154_base,
						   MLXBF_PKA_AIC_POL_CTRL_ADDR);
	ret = mlxbf_pka_dev_set_resource_config(dev,
						shim,
						&shim->resources.aic_csr,
						reg_base,
						reg_size,
						MLXBF_PKA_DEV_RES_TYPE_REG,
						"MLXBF_PKA_AIC_CSR");
	if (ret) {
		dev_err(dev, "unable to set AIC registers config\n");
		return ret;
	}

	/* Set PKA device TRNG registers. */
	reg_size = PAGE_SIZE;
	reg_base = mlxbf_pka_dev_get_register_base(shim->mem_res.eip154_base,
						   MLXBF_PKA_TRNG_OUTPUT_0_ADDR);
	ret = mlxbf_pka_dev_set_resource_config(dev,
						shim,
						&shim->resources.trng_csr,
						reg_base,
						reg_size,
						MLXBF_PKA_DEV_RES_TYPE_REG,
						"MLXBF_PKA_TRNG_CSR");
	if (ret) {
		dev_err(dev, "unable to setup the TRNG\n");
		return ret;
	}

	shim->status = MLXBF_PKA_SHIM_STATUS_CREATED;

	return ret;
}

/* Delete shim and unset shim resources. */
static int mlxbf_pka_dev_delete_shim(struct mlxbf_pka_dev_shim_s *shim)
{
	struct mlxbf_pka_dev_res_t *res_master_seq_ctrl, *res_aic_csr, *res_trng_csr;
	struct mlxbf_pka_dev_res_t *res_buffer_ram;

	pr_debug("mlxbf_pka: PKA device delete shim\n");

	if (shim->status == MLXBF_PKA_SHIM_STATUS_UNDEFINED)
		return 0;

	if (shim->status != MLXBF_PKA_SHIM_STATUS_FINALIZED &&
	    shim->status != MLXBF_PKA_SHIM_STATUS_CREATED) {
		pr_err("mlxbf_pka error: PKA device status must be finalized\n");
		return -EPERM;
	}

	res_buffer_ram = &shim->resources.buffer_ram;
	res_master_seq_ctrl = &shim->resources.master_seq_ctrl;
	res_aic_csr = &shim->resources.aic_csr;
	res_trng_csr = &shim->resources.trng_csr;

	mlxbf_pka_dev_unset_resource_config(shim, res_buffer_ram);
	mlxbf_pka_dev_unset_resource_config(shim, res_master_seq_ctrl);
	mlxbf_pka_dev_unset_resource_config(shim, res_aic_csr);
	mlxbf_pka_dev_unset_resource_config(shim, res_trng_csr);

	shim->status = MLXBF_PKA_SHIM_STATUS_UNDEFINED;

	return 0;
}

/* Configure ring options. */
static int mlxbf_pka_dev_config_ring_options(struct mlxbf_pka_dev_res_t *buffer_ram_ptr,
					     u32 rings_num,
					     u8 ring_priority)
{
	u64 control_word;
	u64 word_off;

	if (buffer_ram_ptr->status != MLXBF_PKA_DEV_RES_STATUS_MAPPED ||
	    buffer_ram_ptr->type != MLXBF_PKA_DEV_RES_TYPE_MEM)
		return -EPERM;

	if (rings_num > MLXBF_PKA_MAX_NUM_RINGS || rings_num < 1) {
		pr_err("mlxbf_pka error: invalid rings number\n");
		return -EINVAL;
	}

	pr_debug("mlxbf_pka: configure PKA ring options control word\n");

	/*
	 * Write MLXBF_PKA_RING_OPTIONS control word located in the MLXBF_PKA_BUFFER_RAM. The value
	 * of this word is determined by the PKA I/O block (Shim). Set the number of implemented
	 * command/result ring pairs that is available in this EIP-154, encoded as binary value,
	 * which is 4.
	 */
	control_word = (u64)0x0;
	control_word |= ring_priority & MLXBF_PKA_RING_OPTIONS_RING_PRIORITY_MASK;
	control_word |= ((rings_num - 1) << MLXBF_PKA_RING_OPTIONS_RING_NUM_OFFSET) &
			MLXBF_PKA_RING_OPTIONS_RING_NUM_MASK;
	control_word |= (MLXBF_PKA_RING_OPTIONS_SIGNATURE_BYTE <<
			MLXBF_PKA_RING_OPTIONS_SIGNATURE_BYTE_OFFSET) &
			MLXBF_PKA_RING_OPTIONS_SIGNATURE_BYTE_MASK;
	word_off = mlxbf_pka_dev_get_word_offset(buffer_ram_ptr->base,
						 MLXBF_PKA_RING_OPTIONS_ADDR,
						 MLXBF_PKA_BUFFER_RAM_SIZE);
	mlxbf_pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, control_word);

	return 0;
}

static int mlxbf_pka_dev_config_trng_clk(struct mlxbf_pka_dev_res_t *aic_csr_ptr)
{
	u32 trng_clk_en = 0;
	void __iomem *csr_reg_ptr;
	u64 csr_reg_base;
	u64 csr_reg_off;
	u64 timer;

	if (aic_csr_ptr->status != MLXBF_PKA_DEV_RES_STATUS_MAPPED ||
	    aic_csr_ptr->type != MLXBF_PKA_DEV_RES_TYPE_REG)
		return -EPERM;

	pr_debug("mlxbf_pka: turn on TRNG clock\n");

	csr_reg_base = aic_csr_ptr->base;
	csr_reg_ptr = aic_csr_ptr->ioaddr;

	/*
	 * Enable the TRNG clock in MLXBF_PKA_CLK_FORCE. In general, this register should be left in
	 * its default state of all zeroes. Only when the TRNG is directly controlled via the Host
	 * slave interface, the engine needs to be turned on using the 'trng_clk_on' bit in this
	 * register. In case the TRNG is controlled via internal firmware, this is not required.
	 */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_CLK_FORCE_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_CLK_FORCE_TRNG_ON);
	/*
	 * Check whether the system clock for TRNG engine is enabled. The clock MUST be running to
	 * provide access to the TRNG.
	 */
	timer = mlxbf_pka_dev_timer_start_msec(100);
	while (!trng_clk_en) {
		trng_clk_en |= mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off)
						     & MLXBF_PKA_CLK_FORCE_TRNG_ON;
		if (mlxbf_pka_dev_timer_done(timer)) {
			pr_debug("mlxbf_pka: failed to enable TRNG clock\n");
			return -EAGAIN;
		}
	}
	pr_debug("mlxbf_pka: trng_clk_on is enabled\n");

	return 0;
}

static int mlxbf_pka_dev_trng_wait_test_ready(void __iomem *csr_reg_ptr, u64 csr_reg_base)
{
	u64 csr_reg_off, timer, csr_reg_val, test_ready = 0;

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_STATUS_ADDR);
	timer = mlxbf_pka_dev_timer_start_msec(1000);

	while (!test_ready) {
		csr_reg_val = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
		test_ready = csr_reg_val & MLXBF_PKA_TRNG_STATUS_TEST_READY;

		if (mlxbf_pka_dev_timer_done(timer)) {
			pr_debug("mlxbf_pka: TRNG test ready timer done, 0x%llx\n", csr_reg_val);
			return 1;
		}
	}

	return 0;
}

static int mlxbf_pka_dev_trng_enable_test(void __iomem *csr_reg_ptr, u64 csr_reg_base, u32 test)
{
	u64 csr_reg_val, csr_reg_off;

	/*
	 * Set the 'test_mode' bit in the TRNG_CONTROL register and the 'test_known_noise' bit in
	 * the TRNG_TEST register – this will immediately set the 'test_ready' bit (in the
	 * TRNG_STATUS register) to indicate that data can be written. It will also reset the
	 * 'monobit test', 'run test' and 'poker test' circuits to their initial states. Note that
	 * the TRNG need not be enabled for this test.
	 */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	csr_reg_val = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);

	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off,
			       csr_reg_val | MLXBF_PKA_TRNG_CONTROL_TEST_MODE);
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_TEST_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, test);
	/* Wait until the 'test_ready' bit is set. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_STATUS_ADDR);
	do {
		csr_reg_val = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
	} while (!(csr_reg_val & MLXBF_PKA_TRNG_STATUS_TEST_READY));

	/* Check whether the 'monobit test', 'run test' and 'poker test' are reset. */
	if (csr_reg_val &
	    (MLXBF_PKA_TRNG_STATUS_MONOBIT_FAIL |
	    MLXBF_PKA_TRNG_STATUS_RUN_FAIL |
	    MLXBF_PKA_TRNG_STATUS_POKER_FAIL)) {
		pr_err("mlxbf_pka error: test bits aren't reset, TRNG_STATUS:0x%llx\n",
		       csr_reg_val);
		return -EAGAIN;
	}

	/*
	 * Set 'stall_run_poker' bit to allow inspecting the state of the result counters which
	 * would otherwise be reset immediately for the next 20,000 bits block to test.
	 */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_ALARMCNT_ADDR);
	csr_reg_val = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
	mlxbf_pka_dev_io_write(csr_reg_ptr,
			       csr_reg_off,
			       csr_reg_val | MLXBF_PKA_TRNG_ALARMCNT_STALL_RUN_POKER);

	return 0;
}

static int mlxbf_pka_dev_trng_test_circuits(void __iomem *csr_reg_ptr,
					    u64 csr_reg_base,
					    u64 datal, u64 datah,
					    int count, u8 add_half,
					    u64 *monobit_fail_cnt,
					    u64 *run_fail_cnt,
					    u64 *poker_fail_cnt)
{
	u64 status, csr_reg_off;
	int test_idx;

	if (!monobit_fail_cnt || !run_fail_cnt || !poker_fail_cnt)
		return -EINVAL;

	for (test_idx = 0; test_idx < count; test_idx++) {
		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_RAW_L_ADDR);
		mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, datal);

		if (add_half) {
			if (test_idx < count - 1) {
				csr_reg_off =
				mlxbf_pka_dev_get_register_offset(csr_reg_base,
								  MLXBF_PKA_TRNG_RAW_H_ADDR);
				mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, datah);
			}
		} else {
			csr_reg_off =
			mlxbf_pka_dev_get_register_offset(csr_reg_base,
							  MLXBF_PKA_TRNG_RAW_H_ADDR);
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, datah);
		}

		/*
		 * Wait until the 'test_ready' bit in the TRNG_STATUS register becomes '1' again,
		 * signaling readiness for the next 64 bits of test data. At this point, the
		 * previous test data has been handled so the counter states can be inspected.
		 */
		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_STATUS_ADDR);
		do {
			status = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
		} while (!(status & MLXBF_PKA_TRNG_STATUS_TEST_READY));

		/* Check test status bits. */
		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_INTACK_ADDR);
		if (status & MLXBF_PKA_TRNG_STATUS_MONOBIT_FAIL) {
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off,
					       MLXBF_PKA_TRNG_STATUS_MONOBIT_FAIL);
			*monobit_fail_cnt += 1;
		} else if (status & MLXBF_PKA_TRNG_STATUS_RUN_FAIL) {
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off,
					       MLXBF_PKA_TRNG_STATUS_RUN_FAIL);
			*run_fail_cnt += 1;
		} else if (status & MLXBF_PKA_TRNG_STATUS_POKER_FAIL) {
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off,
					       MLXBF_PKA_TRNG_STATUS_POKER_FAIL);
			*poker_fail_cnt += 1;
		}
	}

	return (*monobit_fail_cnt || *poker_fail_cnt || *run_fail_cnt) ? -EIO : 0;
}

static void mlxbf_pka_dev_trng_disable_test(void __iomem *csr_reg_ptr, u64 csr_reg_base)
{
	u64 status, val, csr_reg_off;
	/*
	 * When done, clear the 'test_known_noise' bit in the TRNG_TEST register (will immediately
	 * clear the 'test_ready' bit in the TRNG_STATUS register and reset the 'monobit test',
	 * 'run test' and 'poker test' circuits) and clear the 'test_mode' bit in the TRNG_CONTROL
	 * register.
	 */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_TEST_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_STATUS_ADDR);
	status = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

	if (status & MLXBF_PKA_TRNG_STATUS_TEST_READY)
		pr_info("mlxbf_pka warning: test ready bit is still set\n");

	if (status &
	    (MLXBF_PKA_TRNG_STATUS_MONOBIT_FAIL |
	    MLXBF_PKA_TRNG_STATUS_RUN_FAIL |
	    MLXBF_PKA_TRNG_STATUS_POKER_FAIL))
		pr_info("mlxbf_pka warning: test bits are still set, TRNG_STATUS:0x%llx\n", status);

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	val = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, (val & ~MLXBF_PKA_TRNG_STATUS_TEST_READY));

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_ALARMCNT_ADDR);
	val = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
	mlxbf_pka_dev_io_write(csr_reg_ptr,
			       csr_reg_off,
			       (val & ~MLXBF_PKA_TRNG_ALARMCNT_STALL_RUN_POKER));
}

static int mlxbf_pka_dev_trng_test_known_answer_basic(void __iomem *csr_reg_ptr, u64 csr_reg_base)
{
	u64 poker_cnt[MLXBF_PKA_TRNG_POKER_TEST_CNT];
	u64 monobit_fail_cnt = 0;
	u64 poker_fail_cnt = 0;
	u64 run_fail_cnt = 0;
	u64 monobit_cnt;
	u64 csr_reg_off;
	int cnt_idx;
	int cnt_off;
	int ret;

	pr_debug("mlxbf_pka: run known-answer test circuits\n");

	ret = mlxbf_pka_dev_trng_enable_test(csr_reg_ptr, csr_reg_base,
					     MLXBF_PKA_TRNG_TEST_KNOWN_NOISE);
	if (ret)
		return ret;

	ret = mlxbf_pka_dev_trng_test_circuits(csr_reg_ptr,
					       csr_reg_base,
					       MLXBF_PKA_TRNG_TEST_DATAL_BASIC_1,
					       MLXBF_PKA_TRNG_TEST_DATAH_BASIC_1,
					       MLXBF_PKA_TRNG_TEST_COUNT_BASIC_1,
					       MLXBF_PKA_TRNG_TEST_HALF_NO,
					       &monobit_fail_cnt,
					       &run_fail_cnt,
					       &poker_fail_cnt);

	ret |= mlxbf_pka_dev_trng_test_circuits(csr_reg_ptr,
						csr_reg_base,
						MLXBF_PKA_TRNG_TEST_DATAL_BASIC_2,
						MLXBF_PKA_TRNG_TEST_DATAH_BASIC_2,
						MLXBF_PKA_TRNG_TEST_COUNT_BASIC_2,
						MLXBF_PKA_TRNG_TEST_HALF_ADD,
						&monobit_fail_cnt,
						&run_fail_cnt,
						&poker_fail_cnt);

	pr_debug("mlxbf_pka: monobit_fail_cnt : 0x%llx\n", monobit_fail_cnt);
	pr_debug("mlxbf_pka: poker_fail_cnt   : 0x%llx\n", poker_fail_cnt);
	pr_debug("mlxbf_pka: run_fail_cnt     : 0x%llx\n", run_fail_cnt);

	for (cnt_idx = 0, cnt_off = 0;
	     cnt_idx < MLXBF_PKA_TRNG_POKER_TEST_CNT;
	     cnt_idx++, cnt_off += 8) {
		csr_reg_off =
		mlxbf_pka_dev_get_register_offset(csr_reg_base,
						  (MLXBF_PKA_TRNG_POKER_3_0_ADDR + cnt_off));
		poker_cnt[cnt_idx] = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
	}

	csr_reg_off =
	mlxbf_pka_dev_get_register_offset(csr_reg_base,
					  MLXBF_PKA_TRNG_MONOBITCNT_ADDR);
	monobit_cnt = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

	if (!ret) {
		if (memcmp(poker_cnt,
			   poker_test_exp_cnt,
			   sizeof(poker_test_exp_cnt))) {
			pr_debug("mlxbf_pka: invalid poker counters!\n");
			ret = -EIO;
		}

		if (monobit_cnt != MLXBF_PKA_TRNG_MONOBITCNT_SUM) {
			pr_debug("mlxbf_pka: invalid sum of squares!\n");
			ret = -EIO;
		}
	}

	mlxbf_pka_dev_trng_disable_test(csr_reg_ptr, csr_reg_base);

	return ret;
}

static int mlxbf_pka_dev_trng_test_known_answer_poker_fail(void __iomem *csr_reg_ptr,
							   u64 csr_reg_base)
{
	u64 monobit_fail_cnt = 0;
	u64 poker_fail_cnt = 0;
	u64 run_fail_cnt = 0;

	pr_debug("mlxbf_pka: run known-answer test circuits (poker fail)\n");

	mlxbf_pka_dev_trng_enable_test(csr_reg_ptr, csr_reg_base, MLXBF_PKA_TRNG_TEST_KNOWN_NOISE);

	/*
	 * Ignore the return value here as it is expected that poker test should fail. Check failure
	 * counts thereafter to assert only poker test has failed.
	 */
	mlxbf_pka_dev_trng_test_circuits(csr_reg_ptr,
					 csr_reg_base,
					 MLXBF_PKA_TRNG_TEST_DATAL_POKER,
					 MLXBF_PKA_TRNG_TEST_DATAH_POKER,
					 MLXBF_PKA_TRNG_TEST_COUNT_POKER,
					 MLXBF_PKA_TRNG_TEST_HALF_NO,
					 &monobit_fail_cnt,
					 &run_fail_cnt,
					 &poker_fail_cnt);

	pr_debug("mlxbf_pka: monobit_fail_cnt : 0x%llx\n", monobit_fail_cnt);
	pr_debug("mlxbf_pka: poker_fail_cnt   : 0x%llx\n", poker_fail_cnt);
	pr_debug("mlxbf_pka: run_fail_cnt     : 0x%llx\n", run_fail_cnt);

	mlxbf_pka_dev_trng_disable_test(csr_reg_ptr, csr_reg_base);

	return (poker_fail_cnt && !run_fail_cnt && !monobit_fail_cnt) ? 0 : -EIO;
}

static int mlxbf_pka_dev_trng_test_unknown_answer(void __iomem *csr_reg_ptr, u64 csr_reg_base)
{
	u64 datal = 0, datah = 0, csr_reg_off;
	int ret = 0, test_idx;

	pr_debug("mlxbf_pka: run unknown-answer self test\n");

	/* First reset, the RAW registers. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_RAW_L_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_RAW_H_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	/*
	 * There is a small probability for this test to fail. So run the test 10 times, if it
	 * succeeds once then assume that the test passed.
	 */
	for (test_idx = 0; test_idx < 10; test_idx++) {
		mlxbf_pka_dev_trng_enable_test(csr_reg_ptr, csr_reg_base,
					       MLXBF_PKA_TRNG_TEST_NOISE);

		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_RAW_L_ADDR);
		datal = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_RAW_H_ADDR);
		datah = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

		pr_debug("mlxbf_pka: datal=0x%llx\n", datal);
		pr_debug("mlxbf_pka: datah=0x%llx\n", datah);

		mlxbf_pka_dev_trng_disable_test(csr_reg_ptr, csr_reg_base);

		if (!datah && !datal)
			ret = -EIO;
		else
			return 0;
	}
	return ret;
}

/* Test TRNG. */
static int mlxbf_pka_dev_test_trng(void __iomem *csr_reg_ptr, u64 csr_reg_base)
{
	int ret;

	ret = mlxbf_pka_dev_trng_test_known_answer_basic(csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	ret = mlxbf_pka_dev_trng_test_known_answer_poker_fail(csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	ret = mlxbf_pka_dev_trng_test_unknown_answer(csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	return ret;
}

static void mlxbf_pka_dev_trng_write_ps_ai_str(void __iomem *csr_reg_ptr,
					       u64 csr_reg_base,
					       u32 input_str[])
{
	u64 csr_reg_off;
	int i;

	for (i = 0; i < MLXBF_PKA_TRNG_PS_AI_REG_COUNT; i++) {
		csr_reg_off =
		mlxbf_pka_dev_get_register_offset(csr_reg_base,
						  MLXBF_PKA_TRNG_PS_AI_0_ADDR +
						  (i * MLXBF_PKA_TRNG_OUTPUT_REG_OFFSET));

		mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, input_str[i]);
	}
}

static void mlxbf_pka_dev_trng_drbg_generate(void __iomem *csr_reg_ptr, u64 csr_reg_base)
{
	u64 csr_reg_off;

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_CONTROL_REQ_DATA_VAL);
}

static int mlxbf_pka_dev_test_trng_drbg(void __iomem *csr_reg_ptr, u64 csr_reg_base)
{
	u64 csr_reg_off, csr_reg_val;
	int i, ret;

	/* Make sure the engine is idle. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	/* Enable DRBG, TRNG need not be enabled for this test. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_CONTROL_DRBG_ENABLE_VAL);

	/* Set 'test_sp_800_90' bit in the TRNG_TEST register. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_TEST_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_TEST_DRBG_VAL);

	/* Wait for 'test_ready' bit to be set. */
	ret = mlxbf_pka_dev_trng_wait_test_ready(csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	/* 'Instantiate' function. */
	mlxbf_pka_dev_trng_write_ps_ai_str(csr_reg_ptr,
					   csr_reg_base,
					   mlxbf_pka_trng_drbg_test_ps_str);
	ret = mlxbf_pka_dev_trng_wait_test_ready(csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	/* 'Generate' function. */
	mlxbf_pka_dev_trng_write_ps_ai_str(csr_reg_ptr,
					   csr_reg_base,
					   mlxbf_pka_trng_drbg_test_etpy_str1);
	ret = mlxbf_pka_dev_trng_wait_test_ready(csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	/*
	 * A standard NIST SP 800-90A DRBG known-answer test discards the result of the first
	 * 'Generate' function and only checks the result of the second 'Generate' function. Hence
	 * 'Generate' is performed again.
	 */

	/* 'Generate' function. */
	mlxbf_pka_dev_trng_write_ps_ai_str(csr_reg_ptr,
					   csr_reg_base,
					   mlxbf_pka_trng_drbg_test_etpy_str2);
	ret = mlxbf_pka_dev_trng_wait_test_ready(csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	/* Check output registers. */
	for (i = 0; i < MLXBF_PKA_TRNG_OUTPUT_CNT; i++) {
		csr_reg_off =
		mlxbf_pka_dev_get_register_offset(csr_reg_base,
						  MLXBF_PKA_TRNG_OUTPUT_0_ADDR +
						  (i * MLXBF_PKA_TRNG_OUTPUT_REG_OFFSET));

		csr_reg_val = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

		if ((u32)csr_reg_val != mlxbf_pka_trng_drbg_test_output[i]) {
			pr_debug
			("mlxbf_pka: DRBG known answer test failed: output register:%d, 0x%x\n",
			 i, (u32)csr_reg_val);
			return 1;
		}
	}

	/* Clear 'test_sp_800_90' bit in the TRNG_TEST register. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_TEST_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	return 0;
}

/* Configure the TRNG. */
static int mlxbf_pka_dev_config_trng_drbg(struct mlxbf_pka_dev_res_t *aic_csr_ptr,
					  struct mlxbf_pka_dev_res_t *trng_csr_ptr)
{
	u64  csr_reg_base, csr_reg_off;
	void __iomem *csr_reg_ptr;
	int ret;

	if (trng_csr_ptr->status != MLXBF_PKA_DEV_RES_STATUS_MAPPED ||
	    trng_csr_ptr->type != MLXBF_PKA_DEV_RES_TYPE_REG)
		return -EPERM;

	pr_debug("mlxbf_pka: starting up the TRNG\n");

	ret = mlxbf_pka_dev_config_trng_clk(aic_csr_ptr);
	if (ret)
		return ret;

	csr_reg_base = trng_csr_ptr->base;
	csr_reg_ptr  = trng_csr_ptr->ioaddr;

	/*
	 * Perform NIST known-answer tests on the complete SP 800-90A DRBG without BC_DF
	 * functionality.
	 */
	ret = mlxbf_pka_dev_test_trng_drbg(csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	/* Starting up the TRNG with a DRBG. */

	/* Make sure the engine is idle. */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	/* Disable all FROs initially. */
	csr_reg_off =
	mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_FROENABLE_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);
	csr_reg_off =
	mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_FRODETUNE_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	/*
	 * Write all configuration values in the TRNG_CONFIG and TRNG_ALARMCNT, write zeroes to the
	 * TRNG_ALARMMASK and TRNG_ALARMSTOP registers.
	 */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONFIG_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_CONFIG_REG_VAL);
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_ALARMCNT_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_ALARMCNT_REG_VAL);

	csr_reg_off =
	mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_ALARMMASK_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);
	csr_reg_off =
	mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_ALARMSTOP_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

	/*
	 * Enable all FROs in the TRNG_FROENABLE register. Note that this can only be done after
	 * clearing the TRNG_ALARMSTOP register.
	 */
	csr_reg_off =
	mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_FROENABLE_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_FROENABLE_REG_VAL);

	/*
	 * Optionally, write 'Personalization string' of up to 384 bits in TRNG_PS_AI_xxx registers.
	 * The contents of these registers will be XOR-ed into the output of the SHA-256
	 * 'Conditioning Function' to be used as seed value for the actual DRBG.
	 */
	mlxbf_pka_dev_trng_write_ps_ai_str(csr_reg_ptr, csr_reg_base, mlxbf_pka_trng_drbg_ps_str);

	/*
	 * Run TRNG tests after configuring TRNG.
	 * NOTE: TRNG need not be enabled to carry out these tests.
	 */
	ret = mlxbf_pka_dev_test_trng(csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	/*
	 * Start the actual engine by setting the 'enable_trng' and 'drbg_en' bit in the
	 * TRNG_CONTROL register (also a nice point to set the interrupt mask bits).
	 */
	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_CONTROL_DRBG_REG_VAL);

	/*
	 * The engine is now ready to handle the first 'Generate' request using the 'request_data'
	 * bit of the TRNG_CONTROL register. The first output for these requests will take a while,
	 * as Noise Source and Conditioning Function must first generate seed entropy for the DRBG.
	 *
	 * Optionally, when buffer RAM is configured: Set a data available interrupt threshold using
	 * the 'load_thresh' and 'blocks_thresh' fields of the TRNG_INTACK register. This allows
	 * delaying the data available interrupt until the indicated number of 128-bit words are
	 * available in the buffer RAM.
	 *
	 * Start the actual 'Generate' operation using the 'request_data' and 'data_blocks' fields
	 * of the TRNG_CONTROL register.
	 */
	mlxbf_pka_dev_trng_drbg_generate(csr_reg_ptr, csr_reg_base);

	/* Delay 200 ms. */
	mdelay(200);

	return 0;
}

/*
 * Initialize PKA IO block referred to as shim. It configures shim's parameters and prepare
 * resources by mapping corresponding memory. The function also configures shim registers and load
 * firmware to shim internal rams. The struct mlxbf_pka_dev_shim_s passed as input is also an
 * output. It returns 0 on success, a negative error code on failure.
 */
static int mlxbf_pka_dev_init_shim(struct mlxbf_pka_dev_shim_s *shim)
{
	u32 data[MLXBF_PKA_TRNG_OUTPUT_CNT];
	int ret;
	u8 i;

	if (shim->status != MLXBF_PKA_SHIM_STATUS_CREATED) {
		pr_err("mlxbf_pka error: PKA device must be created\n");
		return -EPERM;
	}

	/* Configure PKA Ring options control word. */
	ret = mlxbf_pka_dev_config_ring_options(&shim->resources.buffer_ram,
						shim->rings_num,
						shim->ring_priority);
	if (ret) {
		pr_err("mlxbf_pka error: failed to configure ring options\n");
		return ret;
	}

	shim->trng_enabled   = MLXBF_PKA_SHIM_TRNG_ENABLED;
	shim->trng_err_cycle = 0;

	/* Configure the TRNG. */
	ret = mlxbf_pka_dev_config_trng_drbg(&shim->resources.aic_csr, &shim->resources.trng_csr);

	/*
	 * Pull out data from the content of the TRNG buffer RAM and start the regeneration of new
	 * numbers; read and drop 512 words. The read must be done over the 4 TRNG_OUTPUT_X
	 * registers at a time.
	 */
	for (i = 0; i < MLXBF_PKA_TRNG_NUM_OF_FOUR_WORD; i++)
		mlxbf_pka_dev_trng_read(shim, data, sizeof(data));

	if (ret) {
		/* Keep running without TRNG since it does not hurt, but notify users. */
		pr_err("mlxbf_pka error: failed to configure TRNG\n");
		shim->trng_enabled = MLXBF_PKA_SHIM_TRNG_DISABLED;
	}

	mutex_init(&shim->mutex);
	shim->busy_ring_num = 0;
	shim->status = MLXBF_PKA_SHIM_STATUS_INITIALIZED;

	return ret;
}

/* Release a given shim. */
static int mlxbf_pka_dev_release_shim(struct mlxbf_pka_dev_shim_s *shim)
{
	u32 ring_idx;
	int ret = 0;

	if (shim->status != MLXBF_PKA_SHIM_STATUS_INITIALIZED &&
	    shim->status != MLXBF_PKA_SHIM_STATUS_STOPPED) {
		pr_err("mlxbf_pka error: PKA device must be initialized or stopped\n");
		return -EPERM;
	}

	/*
	 * Release rings which belong to the shim. The operating system might release ring devices
	 * before shim devices. The global configuration must be checked before proceeding to the
	 * release of ring devices.
	 */
	if (mlxbf_pka_gbl_config.dev_rings_cnt) {
		for (ring_idx = 0; ring_idx < shim->rings_num; ring_idx++) {
			ret = mlxbf_pka_dev_release_ring(shim->rings[ring_idx]);
			if (ret) {
				pr_err("mlxbf_pka error: failed to release ring %d\n", ring_idx);
				return ret;
			}
		}
	}

	shim->busy_ring_num = 0;
	shim->status = MLXBF_PKA_SHIM_STATUS_FINALIZED;

	return ret;
}

/* Return the ring associated with the given identifier. */
struct mlxbf_pka_dev_ring_t *mlxbf_pka_dev_get_ring(u32 ring_id)
{
	return mlxbf_pka_gbl_config.dev_rings[ring_id];
}

/* Return the shim associated with the given identifier. */
struct mlxbf_pka_dev_shim_s *mlxbf_pka_dev_get_shim(u32 shim_id)
{
	return mlxbf_pka_gbl_config.dev_shims[shim_id];
}

struct mlxbf_pka_dev_ring_t *mlxbf_pka_dev_register_ring(struct device *dev,
							 u32 ring_id,
							 u32 shim_id)
{
	struct mlxbf_pka_dev_shim_s *shim;
	struct mlxbf_pka_dev_ring_t *ring;
	int ret;

	shim = mlxbf_pka_dev_get_shim(shim_id);
	if (!shim)
		return NULL;

	ring = devm_kzalloc(dev, sizeof(*ring), GFP_KERNEL);
	if (!ring)
		return NULL;

	ring->status = MLXBF_PKA_DEV_RING_STATUS_UNDEFINED;

	/* Initialize ring. */
	ret = mlxbf_pka_dev_init_ring(dev, ring, ring_id, shim);
	if (ret) {
		dev_err(dev, "failed to initialize ring %d\n", ring_id);
		mlxbf_pka_dev_release_ring(ring);
		return NULL;
	}

	mlxbf_pka_gbl_config.dev_rings[ring->ring_id] = ring;
	mlxbf_pka_gbl_config.dev_rings_cnt += 1;

	return ring;
}

int mlxbf_pka_dev_unregister_ring(struct mlxbf_pka_dev_ring_t *ring)
{
	if (!ring)
		return -EINVAL;

	mlxbf_pka_gbl_config.dev_rings[ring->ring_id] = NULL;
	mlxbf_pka_gbl_config.dev_rings_cnt -= 1;

	/* Release ring. */
	return mlxbf_pka_dev_release_ring(ring);
}

struct mlxbf_pka_dev_shim_s *mlxbf_pka_dev_register_shim(struct device *dev,
							 u32 shim_id,
							 struct mlxbf_pka_dev_mem_res *mem_res)
{
	struct mlxbf_pka_dev_shim_s *shim;
	u8 split;
	int ret;

	dev_dbg(dev, "register shim id=%u\n", shim_id);

	shim = devm_kzalloc(dev, sizeof(*shim), GFP_KERNEL);
	if (!shim)
		return shim;

	/*
	 * Shim state MUST be set to undefined before calling 'mlxbf_pka_dev_create_shim' function.
	 */
	shim->status = MLXBF_PKA_SHIM_STATUS_UNDEFINED;

	/* Set the Window RAM user mode. */
	split = MLXBF_PKA_SPLIT_WINDOW_RAM_MODE;

	/* Create PKA shim. */
	ret = mlxbf_pka_dev_create_shim(dev, shim, shim_id, split, mem_res);
	if (ret) {
		dev_err(dev, "failed to create shim %u\n", shim_id);
		mlxbf_pka_dev_delete_shim(shim);
		return NULL;
	}

	/* Initialize PKA shim. */
	ret = mlxbf_pka_dev_init_shim(shim);
	if (ret) {
		dev_err(dev, "failed to init shim %u\n", shim_id);
		mlxbf_pka_dev_release_shim(shim);
		mlxbf_pka_dev_delete_shim(shim);
		return NULL;
	}

	mlxbf_pka_gbl_config.dev_shims[shim->shim_id] = shim;
	mlxbf_pka_gbl_config.dev_shims_cnt += 1;

	return shim;
}

int mlxbf_pka_dev_unregister_shim(struct mlxbf_pka_dev_shim_s *shim)
{
	int ret;

	if (!shim)
		return -EINVAL;

	mlxbf_pka_gbl_config.dev_shims[shim->shim_id] = NULL;
	mlxbf_pka_gbl_config.dev_shims_cnt -= 1;

	/* Release shim. */
	ret = mlxbf_pka_dev_release_shim(shim);
	if (ret)
		return ret;

	/* Delete shim. */
	return mlxbf_pka_dev_delete_shim(shim);
}

static bool mlxbf_pka_dev_trng_shutdown_oflo(struct mlxbf_pka_dev_res_t *trng_csr_ptr,
					     u64 *err_cycle)
{
	u64 curr_cycle_cnt, fro_stopped_mask, fro_enabled_mask;
	u64 csr_reg_base, csr_reg_off, csr_reg_value;
	void __iomem *csr_reg_ptr;

	csr_reg_base = trng_csr_ptr->base;
	csr_reg_ptr = trng_csr_ptr->ioaddr;

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_STATUS_ADDR);
	csr_reg_value = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

	if (csr_reg_value & MLXBF_PKA_TRNG_STATUS_SHUTDOWN_OFLO) {
		curr_cycle_cnt = get_cycles();
		/*
		 * See if any FROs were shut down. If they were, toggle bits in the FRO detune
		 * register and reenable the FROs.
		 */
		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_ALARMSTOP_ADDR);
		fro_stopped_mask = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
		if (fro_stopped_mask) {
			csr_reg_off =
			mlxbf_pka_dev_get_register_offset(csr_reg_base,
							  MLXBF_PKA_TRNG_FROENABLE_ADDR);
			fro_enabled_mask = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
			csr_reg_off =
			mlxbf_pka_dev_get_register_offset(csr_reg_base,
							  MLXBF_PKA_TRNG_FRODETUNE_ADDR);
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, fro_stopped_mask);

			csr_reg_off =
			mlxbf_pka_dev_get_register_offset(csr_reg_base,
							  MLXBF_PKA_TRNG_FROENABLE_ADDR);
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off,
					       fro_stopped_mask | fro_enabled_mask);
		}

		/* Reset the error. */
		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_ALARMMASK_ADDR);
		mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_ALARMSTOP_ADDR);
		mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, 0);

		csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
								MLXBF_PKA_TRNG_INTACK_ADDR);
		mlxbf_pka_dev_io_write(csr_reg_ptr,
				       csr_reg_off,
				       MLXBF_PKA_TRNG_STATUS_SHUTDOWN_OFLO);

		/*
		 * If this error occurs again within about a second, the hardware is malfunctioning.
		 * Disable the trng and return an error.
		 */
		if (*err_cycle &&
		    (curr_cycle_cnt - *err_cycle < MLXBF_PKA_TRNG_TEST_ERR_CYCLE_MAX)) {
			csr_reg_off =
			mlxbf_pka_dev_get_register_offset(csr_reg_base,
							  MLXBF_PKA_TRNG_CONTROL_ADDR);
			csr_reg_value  = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
			csr_reg_value &= ~MLXBF_PKA_TRNG_CONTROL_REG_VAL;
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, csr_reg_value);
			return false;
		}

		*err_cycle = curr_cycle_cnt;
	}

	return true;
}

static int mlxbf_pka_dev_trng_drbg_reseed(void __iomem *csr_reg_ptr, u64 csr_reg_base)
{
	u64 csr_reg_off;
	int ret;

	csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_CONTROL_ADDR);
	mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off, MLXBF_PKA_TRNG_CONTROL_DRBG_RESEED);

	ret = mlxbf_pka_dev_trng_wait_test_ready(csr_reg_ptr, csr_reg_base);
	if (ret)
		return ret;

	/* Write personalization string. */
	mlxbf_pka_dev_trng_write_ps_ai_str(csr_reg_ptr, csr_reg_base, mlxbf_pka_trng_drbg_ps_str);

	return ret;
}

/* Read from DRBG enabled TRNG. */
int mlxbf_pka_dev_trng_read(struct mlxbf_pka_dev_shim_s *shim, u32 *data, u32 cnt)
{
	u64 csr_reg_base, csr_reg_off, csr_reg_value, timer;
	struct mlxbf_pka_dev_res_t *trng_csr_ptr;
	u8 output_idx, trng_ready = 0;
	u32 data_idx, word_cnt;
	void __iomem *csr_reg_ptr;
	int ret = 0;

	if (!shim || !data || (cnt % MLXBF_PKA_TRNG_OUTPUT_CNT != 0))
		return -EINVAL;

	if (!cnt)
		return ret;

	mutex_lock(&shim->mutex);

	trng_csr_ptr = &shim->resources.trng_csr;

	if (trng_csr_ptr->status != MLXBF_PKA_DEV_RES_STATUS_MAPPED ||
	    trng_csr_ptr->type != MLXBF_PKA_DEV_RES_TYPE_REG) {
		ret = -EPERM;
		goto exit;
	}

	csr_reg_base = trng_csr_ptr->base;
	csr_reg_ptr = trng_csr_ptr->ioaddr;

	if (!mlxbf_pka_dev_trng_shutdown_oflo(trng_csr_ptr, &shim->trng_err_cycle)) {
		ret = -EWOULDBLOCK;
		goto exit;
	}

	/* Determine the number of 32-bit words. */
	word_cnt = cnt >> 2;

	for (data_idx = 0; data_idx < word_cnt; data_idx++) {
		output_idx = data_idx % MLXBF_PKA_TRNG_OUTPUT_CNT;

		/* Tell the hardware to advance. */
		if (!output_idx) {
			csr_reg_off = mlxbf_pka_dev_get_register_offset(csr_reg_base,
									MLXBF_PKA_TRNG_INTACK_ADDR);
			mlxbf_pka_dev_io_write(csr_reg_ptr, csr_reg_off,
					       MLXBF_PKA_TRNG_STATUS_READY);
			trng_ready = 0;

			/*
			 * Check if 'data_blocks' field is zero in TRNG_CONTROL register. If it is
			 * zero, need to issue a 'Reseed and Generate' request for DRBG enabled
			 * TRNG.
			 */
			csr_reg_off =
			mlxbf_pka_dev_get_register_offset(csr_reg_base,
							  MLXBF_PKA_TRNG_CONTROL_ADDR);
			csr_reg_value = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);

			if (!((u32)csr_reg_value & MLXBF_PKA_TRNG_DRBG_DATA_BLOCK_MASK)) {
				/* Issue reseed. */
				ret = mlxbf_pka_dev_trng_drbg_reseed(csr_reg_ptr, csr_reg_base);
				if (ret) {
					ret = -EBUSY;
					goto exit;
				}

				/* Issue generate request. */
				mlxbf_pka_dev_trng_drbg_generate(csr_reg_ptr, csr_reg_base);
			}
		}

		/*
		 * Wait until a data word is available in the TRNG_OUTPUT_X registers, using the
		 * interrupt and/or 'ready' status bit in the TRNG_STATUS register. The only way
		 * this would hang is if the TRNG is never initialized. This function cannot be
		 * called if that happened.
		 */
		timer = mlxbf_pka_dev_timer_start_msec(1000);
		csr_reg_off =
		mlxbf_pka_dev_get_register_offset(csr_reg_base, MLXBF_PKA_TRNG_STATUS_ADDR);
		while (!trng_ready) {
			csr_reg_value = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
			trng_ready = csr_reg_value & MLXBF_PKA_TRNG_STATUS_READY;

		if (mlxbf_pka_dev_timer_done(timer)) {
			pr_debug("mlxbf_pka: shim %u got error obtaining random number\n",
				 shim->shim_id);
			ret = -EBUSY;
			goto exit;
		}
	}

	/* Read the registers. */
	csr_reg_off =
	mlxbf_pka_dev_get_register_offset(csr_reg_base,
					  MLXBF_PKA_TRNG_OUTPUT_0_ADDR +
					  (output_idx * MLXBF_PKA_TRNG_OUTPUT_REG_OFFSET));
	csr_reg_value = mlxbf_pka_dev_io_read(csr_reg_ptr, csr_reg_off);
	data[data_idx] = (u32)csr_reg_value;
	}

exit:
	mutex_unlock(&shim->mutex);
	return ret;
}

bool mlxbf_pka_dev_has_trng(struct mlxbf_pka_dev_shim_s *shim)
{
	if (!shim)
		return false;

	return (shim->trng_enabled == MLXBF_PKA_SHIM_TRNG_ENABLED);
}

/* Syscall to open ring. */
static int __mlxbf_pka_dev_open_ring(u32 ring_id)
{
	struct mlxbf_pka_dev_shim_s *shim;
	struct mlxbf_pka_dev_ring_t *ring;
	int ret;

	if (!mlxbf_pka_gbl_config.dev_rings_cnt)
		return -EPERM;

	ring = mlxbf_pka_dev_get_ring(ring_id);
	if (!ring || !ring->shim)
		return -ENXIO;

	shim = ring->shim;

	mutex_lock(&ring->mutex);

	if (shim->status == MLXBF_PKA_SHIM_STATUS_UNDEFINED ||
	    shim->status == MLXBF_PKA_SHIM_STATUS_CREATED ||
	    shim->status == MLXBF_PKA_SHIM_STATUS_FINALIZED) {
		ret = -EPERM;
		goto unlock_return;
	}

	if (ring->status == MLXBF_PKA_DEV_RING_STATUS_BUSY) {
		ret = -EBUSY;
		goto unlock_return;
	}

	if (ring->status != MLXBF_PKA_DEV_RING_STATUS_INITIALIZED) {
		ret = -EPERM;
		goto unlock_return;
	}

	/* Set ring information words. */
	ret = mlxbf_pka_dev_set_ring_info(ring);
	if (ret) {
		pr_err("mlxbf_pka error: failed to set ring information\n");
		ret = -EWOULDBLOCK;
		goto unlock_return;
	}

	if (!shim->busy_ring_num)
		shim->status = MLXBF_PKA_SHIM_STATUS_RUNNING;

	ring->status = MLXBF_PKA_DEV_RING_STATUS_BUSY;
	shim->busy_ring_num += 1;

unlock_return:
	mutex_unlock(&ring->mutex);
	return ret;
}

/* Open ring. */
int mlxbf_pka_dev_open_ring(struct mlxbf_pka_ring_info_t *ring_info)
{
	return __mlxbf_pka_dev_open_ring(ring_info->ring_id);
}

/* Syscall to close ring. */
static int __mlxbf_pka_dev_close_ring(u32 ring_id)
{
	struct mlxbf_pka_dev_shim_s *shim;
	struct mlxbf_pka_dev_ring_t *ring;
	int ret = 0;

	if (!mlxbf_pka_gbl_config.dev_rings_cnt)
		return -EPERM;

	ring = mlxbf_pka_dev_get_ring(ring_id);
	if (!ring || !ring->shim)
		return -ENXIO;

	shim = ring->shim;

	mutex_lock(&ring->mutex);

	if (shim->status != MLXBF_PKA_SHIM_STATUS_RUNNING &&
	    ring->status != MLXBF_PKA_DEV_RING_STATUS_BUSY) {
		ret = -EPERM;
		goto unlock_return;
	}

	ring->status = MLXBF_PKA_DEV_RING_STATUS_INITIALIZED;
	shim->busy_ring_num -= 1;

	if (!shim->busy_ring_num)
		shim->status = MLXBF_PKA_SHIM_STATUS_STOPPED;

unlock_return:
	mutex_unlock(&ring->mutex);
	return ret;
}

/* Close ring. */
int mlxbf_pka_dev_close_ring(struct mlxbf_pka_ring_info_t *ring_info)
{
	if (ring_info)
		return __mlxbf_pka_dev_close_ring(ring_info->ring_id);

	return 0;
}
