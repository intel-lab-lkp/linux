// SPDX-License-Identifier: GPL-2.0-only OR BSD-3-Clause
// SPDX-FileCopyrightText: Copyright (c) 2025 NVIDIA CORPORATION. All rights reserved.

#include <linux/device.h>

#include "mlxbf_pka_dev.h"
#include "mlxbf_pka_ring.h"

/*
 * Mapping PKA Ring address into Window RAM offset address.
 *
 * It converts the ring address, either physical address or virtual address, to
 * valid address into the Window RAM. This is done using the provided Window RAM
 * win_base, ring_addr and win_mask parameters. Here, win_base is the actual
 * physical address of the Window RAM, with the help of win_mask it is reduced
 * to Window RAM offset within that PKA block. Further, with the help of
 * ring_addr and ring_size, we arrive at the Window RAM offset address for a
 * PKA Ring within the given Window RAM.
 *
 * The hardware encoded the ring size in 32-bit words, not bytes. Therefore,
 * the ring size is right-shifted to convert bytes into words.
 */
static inline u64 mlxbf_pka_ring_mem_addr(u64 win_base, u64 win_mask, u64 ring_addr, u64 ring_size)
{
	return (win_base & win_mask) |
	      FIELD_PREP(MLXBF_PKA_WINDOW_RAM_RING_ADDR_MASK, ring_addr) |
	      FIELD_PREP(MLXBF_PKA_WINDOW_RAM_RING_SIZE_MASK,
			 ((ring_addr & ~(ring_size - 1)) >>
			  MLXBF_PKA_WINDOW_RAM_RING_SIZE_SHIFT));
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
	mlxbf_pka_dev_io_write(master_reg_ptr, master_reg_off, MLXBF_PKA_MASTER_SEQ_CTRL_RESET);

	/* Clear counters. */
	mlxbf_pka_dev_io_write(master_reg_ptr, master_reg_off,
			       MLXBF_PKA_MASTER_SEQ_CTRL_CLEAR_COUNTERS);

	/* Take the EIP-154 master controller out of reset. */
	mlxbf_pka_dev_io_write(master_reg_ptr, master_reg_off, MASTER_CONTROLLER_OUT_OF_RESET);

	return 0;
}

/*
 * Initialize ring. Set ring parameters and configure ring resources. It returns
 * 0 on success, a negative error code on failure.
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
	int ret;

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

	ret = devm_mutex_init(dev, &ring->mutex);
	if (ret)
		return ret;

	ring->status = MLXBF_PKA_DEV_RING_STATUS_INITIALIZED;

	return 0;
}

/* Release a given Ring. */
int mlxbf_pka_dev_release_ring(struct device *dev, struct mlxbf_pka_dev_ring_t *ring)
{
	struct mlxbf_pka_dev_shim_s *shim;
	u32 shim_ring_id;

	if (ring->status == MLXBF_PKA_DEV_RING_STATUS_UNDEFINED)
		return 0;

	if (ring->status == MLXBF_PKA_DEV_RING_STATUS_BUSY) {
		dev_err(dev, "PKA ring is busy\n");
		return -EBUSY;
	}

	shim = ring->shim;

	if (shim->status == MLXBF_PKA_SHIM_STATUS_RUNNING) {
		dev_err(dev, "PKA shim is running\n");
		return -EPERM;
	}

	mlxbf_pka_dev_unset_resource_config(dev, shim, &ring->resources.info_words);
	mlxbf_pka_dev_unset_resource_config(dev, shim, &ring->resources.counters);
	mlxbf_pka_dev_unset_resource_config(dev, shim, &ring->resources.window_ram);

	ring->status = MLXBF_PKA_DEV_RING_STATUS_UNDEFINED;
	shim_ring_id = ring->ring_id % MLXBF_PKA_MAX_NUM_IO_BLOCK_RINGS;
	shim->rings[shim_ring_id] = NULL;
	shim->rings_num--;

	return 0;
}

/*
 * Partition the window RAM for a given PKA ring. Here we statically divide the
 * 16K memory region into three partitions: First partition is reserved for
 * command descriptor ring (1K), second partition is reserved for result
 * descriptor ring (1K), and the remaining 14K are reserved for vector data.
 * Through this memory partition scheme, command/result descriptor rings hold a
 * total of 1KB/64B = 16 descriptors each. The addresses for the rings start at
 * offset 0x3800. Also note that it is possible to have rings full while the
 * vector data can support more data, the opposite can also happen, but it is
 * not suitable. For instance ECC point multiplication requires 8 input vectors
 * and 2 output vectors, a total of 10 vectors. If each vector has a length of
 * 24 words (24x4B = 96B), we can process 14KB/960B = 14 operations which is
 * close to 16 the total descriptors supported by rings. On the other hand,
 * using 12K vector data region, allows to process only 12 operations, while
 * rings can hold 32 descriptors (ring usage is significantly low).
 *
 * For ECDSA verify, we have 12 vectors which require 1152B, with 14KB we can
 * handle 12 operations, against 10 operations with 12KB vector data memory. We
 * believe that the aforementioned memory partition help us to leverage the
 * trade-off between supported descriptors and required vectors. Note that these
 * examples give approximative values and does not include buffer word padding
 * across vectors.
 *
 * The function also writes the result descriptor rings base addresses, size and
 * type. And initialize the read and write pointers and statistics. It returns
 * 0 on success, a negative error code on failure.
 *
 * This function must be called once per ring, at initialization before any
 * other functions are called.
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
	 * Partition ring memory. Give ring pair (cmd descriptor ring and rslt
	 * descriptor ring) an equal portion of the memory. The cmd descriptor
	 * ring and result descriptor ring are used as "non-overlapping" ring.
	 * Currently set aside 1/8 of the window RAM for command and result
	 * descriptor rings - giving a total of 1K/64B = 16 descriptors per
	 * ring. The remaining memory is "Data Memory" - i.e. memory to hold
	 * the command operands and results - also called input/output vectors
	 * (in all cases these vectors are just single large integers - often in
	 * the range of hundreds to thousands of bits long).
	 */
	ring_mem_base = window_ram_base + MLXBF_PKA_WINDOW_RAM_DATA_MEM_SIZE;
	cmd_desc_ring_size = MLXBF_PKA_WINDOW_RAM_RING_MEM_SIZE /
			    MLXBF_PKA_WINDOW_RAM_RING_MEM_DIV;
	ring->num_cmd_desc = MLXBF_PKA_WINDOW_RAM_RING_MEM_SIZE /
			    MLXBF_PKA_WINDOW_RAM_RING_MEM_DIV / CMD_DESC_SIZE;
	/*
	 * The command and result descriptor rings may be placed at different
	 * (non-overlapping) locations in Window RAM memory space. PKI command
	 * interface: Most of the functionality is defined by the EIP-154 master
	 * firmware on the EIP-154 master controller Sequencer.
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
	memset(ring->ring_info, 0, sizeof(*ring->ring_info));

	ring->ring_info->cmd_base = cmd_desc_ring_base;
	ring->ring_info->rslt_base = rslt_desc_ring_base;
	ring->ring_info->size = MLXBF_PKA_WINDOW_RAM_RING_MEM_SIZE /
				MLXBF_PKA_WINDOW_RAM_RING_MEM_DIV / CMD_DESC_SIZE - 1;
	ring->ring_info->host_desc_size = CMD_DESC_SIZE / sizeof(u32);
	ring->ring_info->in_order = ring->shim->ring_type;

	return 0;
}

/*
 * Write the ring base address, ring size and type, and initialize (clear) the
 * read and write pointers and statistics.
 */
static int mlxbf_pka_dev_write_ring_info(struct device *dev,
					 struct mlxbf_pka_dev_res_t *buffer_ram_ptr,
					 u8 ring_id,
					 u32 ring_cmd_base_val,
					 u32 ring_rslt_base_val,
					 u32 ring_size_type_val)
{
	u32 ring_spacing;
	u64 word_off;

	if (buffer_ram_ptr->status != MLXBF_PKA_DEV_RES_STATUS_MAPPED ||
	    buffer_ram_ptr->type != MLXBF_PKA_DEV_RES_TYPE_MEM)
		return -EPERM;

	dev_dbg(dev, "writing ring information control/status words\n");

	ring_spacing = ring_id * MLXBF_PKA_RING_WORDS_SPACING;

	/* Get command descriptors from the Host ring. */
	word_off = mlxbf_pka_dev_get_word_offset(buffer_ram_ptr->base,
						 MLXBF_PKA_RING_CMD_BASE_0_ADDR + ring_spacing,
						 MLXBF_PKA_BUFFER_RAM_SIZE);
	mlxbf_pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, ring_cmd_base_val);

	/* Put the result descriptors in the Host ring. */
	word_off = mlxbf_pka_dev_get_word_offset(buffer_ram_ptr->base,
						 MLXBF_PKA_RING_RSLT_BASE_0_ADDR + ring_spacing,
						 MLXBF_PKA_BUFFER_RAM_SIZE);
	mlxbf_pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, ring_rslt_base_val);

	/* Write the ring size (number of descriptors) */
	word_off = mlxbf_pka_dev_get_word_offset(buffer_ram_ptr->base,
						 MLXBF_PKA_RING_SIZE_TYPE_0_ADDR + ring_spacing,
						 MLXBF_PKA_BUFFER_RAM_SIZE);
	mlxbf_pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, ring_size_type_val);

	/* Write the command and result ring indices. */
	word_off = mlxbf_pka_dev_get_word_offset(buffer_ram_ptr->base,
						 MLXBF_PKA_RING_RW_PTRS_0_ADDR + ring_spacing,
						 MLXBF_PKA_BUFFER_RAM_SIZE);
	mlxbf_pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, 0);

	/*
	 * Write the ring statistics (two 16-bit counters, one for commands and
	 * one for results).
	 */
	word_off = mlxbf_pka_dev_get_word_offset(buffer_ram_ptr->base,
						 MLXBF_PKA_RING_RW_STAT_0_ADDR + ring_spacing,
						 MLXBF_PKA_BUFFER_RAM_SIZE);
	mlxbf_pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, 0);

	return 0;
}

/*
 * Set up the control/status words. Upon a PKI command the EIP-154 master
 * firmware will read and partially update the ring information.
 */
static int mlxbf_pka_dev_set_ring_info(struct device *dev, struct mlxbf_pka_dev_ring_t *ring)
{
	u32 ring_cmd_base_val;
	u32 ring_rslt_base_val;
	u32 ring_size_type_val;
	int ret;

	/*
	 * Ring info configuration MUST be done when the PKA ring is
	 * initialized.
	 */
	if ((ring->shim->status != MLXBF_PKA_SHIM_STATUS_INITIALIZED &&
	     ring->shim->status != MLXBF_PKA_SHIM_STATUS_RUNNING &&
	     ring->shim->status != MLXBF_PKA_SHIM_STATUS_STOPPED) ||
	     ring->status != MLXBF_PKA_DEV_RING_STATUS_INITIALIZED)
		return -EPERM;

	/* Partition ring memory. */
	ret = mlxbf_pka_dev_partition_mem(ring);
	if (ret) {
		dev_err(dev, "failed to initialize ring memory\n");
		return ret;
	}

	/* Fill ring information. */
	ring_cmd_base_val = ring->ring_info->cmd_base;
	ring_rslt_base_val = ring->ring_info->rslt_base;
	ring_size_type_val = FIELD_PREP(MLXBF_PKA_RING_INFO_IN_ORDER_MASK,
					ring->ring_info->in_order);
	ring_size_type_val |= FIELD_PREP(MLXBF_PKA_RING_INFO_HOST_DESC_SIZE_MASK,
					ring->ring_info->host_desc_size);
	ring_size_type_val |= FIELD_PREP(MLXBF_PKA_RING_NUM_CMD_DESC_MASK, ring->num_cmd_desc - 1);

	/* Write ring information status/control words in the PKA Buffer RAM. */
	ret = mlxbf_pka_dev_write_ring_info(dev,
					    &ring->shim->resources.buffer_ram,
					    ring->ring_id % MLXBF_PKA_MAX_NUM_IO_BLOCK_RINGS,
					    ring_cmd_base_val,
					    ring_rslt_base_val,
					    ring_size_type_val);
	if (ret) {
		dev_err(dev, "failed to write ring information\n");
		return ret;
	}

	ring->status = MLXBF_PKA_DEV_RING_STATUS_READY;

	return ret;
}

/* Configure ring options. */
int mlxbf_pka_dev_config_ring_options(struct device *dev,
				      struct mlxbf_pka_dev_res_t *buffer_ram_ptr,
				      u32 rings_num,
				      u8 ring_priority)
{
	u64 control_word;
	u64 word_off;

	if (buffer_ram_ptr->status != MLXBF_PKA_DEV_RES_STATUS_MAPPED ||
	    buffer_ram_ptr->type != MLXBF_PKA_DEV_RES_TYPE_MEM)
		return -EPERM;

	if (rings_num > MLXBF_PKA_MAX_NUM_RINGS || rings_num < 1) {
		dev_err(dev, "invalid rings number\n");
		return -EINVAL;
	}

	dev_dbg(dev, "configure PKA ring options control word\n");

	/*
	 * Write MLXBF_PKA_RING_OPTIONS control word located in the
	 * MLXBF_PKA_BUFFER_RAM. The value of this word is determined by the
	 * PKA I/O block (Shim). Set the number of implemented command/result
	 * ring pairs that is available in this EIP-154, encoded as binary
	 * value, which is 4.
	 */
	control_word = FIELD_PREP(MLXBF_PKA_RING_OPTIONS_RING_PRIORITY_MASK, ring_priority) |
		       FIELD_PREP(MLXBF_PKA_RING_OPTIONS_RING_NUM_MASK, (rings_num - 1)) |
		       FIELD_PREP(MLXBF_PKA_RING_OPTIONS_SIGNATURE_BYTE_MASK,
				  MLXBF_PKA_RING_OPTIONS_SIGNATURE_BYTE);
	word_off = mlxbf_pka_dev_get_word_offset(buffer_ram_ptr->base,
						 MLXBF_PKA_RING_OPTIONS_ADDR,
						 MLXBF_PKA_BUFFER_RAM_SIZE);
	mlxbf_pka_dev_io_write(buffer_ram_ptr->ioaddr, word_off, control_word);

	return 0;
}

/* Return the ring associated with the given identifier. */
struct mlxbf_pka_dev_ring_t *mlxbf_pka_dev_get_ring(u32 ring_id)
{
	return mlxbf_pka_gbl_config.dev_rings[ring_id];
}

int mlxbf_pka_dev_register_ring(struct device *dev,
				u32 ring_id,
				u32 shim_id,
				struct mlxbf_pka_dev_ring_t **ring)
{
	struct mlxbf_pka_dev_ring_t *ring_ptr;
	struct mlxbf_pka_dev_shim_s *shim;
	int ret;

	if (!ring)
		return -EINVAL;

	shim = mlxbf_pka_dev_get_shim(shim_id);
	if (!shim)
		return -ENODEV;

	ring_ptr = devm_kzalloc(dev, sizeof(*ring_ptr), GFP_KERNEL);
	if (!ring_ptr)
		return -ENOMEM;

	ring_ptr->status = MLXBF_PKA_DEV_RING_STATUS_UNDEFINED;

	/* Initialize ring. */
	ret = mlxbf_pka_dev_init_ring(dev, ring_ptr, ring_id, shim);
	if (ret) {
		dev_err(dev, "failed to initialize ring %d\n", ring_id);
		mlxbf_pka_dev_release_ring(dev, ring_ptr);
		return ret;
	}

	mlxbf_pka_gbl_config.dev_rings[ring_ptr->ring_id] = ring_ptr;
	mlxbf_pka_gbl_config.dev_rings_cnt += 1;

	*ring = ring_ptr;
	return 0;
}

int mlxbf_pka_dev_unregister_ring(struct device *dev, struct mlxbf_pka_dev_ring_t *ring)
{
	if (!ring)
		return -EINVAL;

	mlxbf_pka_gbl_config.dev_rings[ring->ring_id] = NULL;
	mlxbf_pka_gbl_config.dev_rings_cnt -= 1;

	/* Release ring. */
	return mlxbf_pka_dev_release_ring(dev, ring);
}

/* Syscall to open ring. */
static int __mlxbf_pka_dev_open_ring(struct device *dev, u32 ring_id)
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

	guard(mutex)(&ring->mutex);

	if (shim->status == MLXBF_PKA_SHIM_STATUS_UNDEFINED ||
	    shim->status == MLXBF_PKA_SHIM_STATUS_CREATED ||
	    shim->status == MLXBF_PKA_SHIM_STATUS_FINALIZED)
		return -EPERM;

	if (ring->status == MLXBF_PKA_DEV_RING_STATUS_BUSY)
		return -EBUSY;

	if (ring->status != MLXBF_PKA_DEV_RING_STATUS_INITIALIZED)
		return -EPERM;

	/* Set ring information words. */
	ret = mlxbf_pka_dev_set_ring_info(dev, ring);
	if (ret) {
		dev_err(dev, "failed to set ring information\n");
		return -EWOULDBLOCK;
	}

	if (!shim->busy_ring_num)
		shim->status = MLXBF_PKA_SHIM_STATUS_RUNNING;

	ring->status = MLXBF_PKA_DEV_RING_STATUS_BUSY;
	shim->busy_ring_num += 1;

	return ret;
}

/* Open ring. */
int mlxbf_pka_dev_open_ring(struct device *dev, struct mlxbf_pka_ring_info_t *ring_info)
{
	return __mlxbf_pka_dev_open_ring(dev, ring_info->ring_id);
}

/* Syscall to close ring. */
static int __mlxbf_pka_dev_close_ring(u32 ring_id)
{
	struct mlxbf_pka_dev_shim_s *shim;
	struct mlxbf_pka_dev_ring_t *ring;

	if (!mlxbf_pka_gbl_config.dev_rings_cnt)
		return -EPERM;

	ring = mlxbf_pka_dev_get_ring(ring_id);
	if (!ring || !ring->shim)
		return -ENXIO;

	shim = ring->shim;

	guard(mutex)(&ring->mutex);

	if (shim->status != MLXBF_PKA_SHIM_STATUS_RUNNING &&
	    ring->status != MLXBF_PKA_DEV_RING_STATUS_BUSY)
		return -EPERM;

	ring->status = MLXBF_PKA_DEV_RING_STATUS_INITIALIZED;
	shim->busy_ring_num -= 1;

	if (!shim->busy_ring_num)
		shim->status = MLXBF_PKA_SHIM_STATUS_STOPPED;

	return 0;
}

/* Close ring. */
int mlxbf_pka_dev_close_ring(struct mlxbf_pka_ring_info_t *ring_info)
{
	if (ring_info)
		return __mlxbf_pka_dev_close_ring(ring_info->ring_id);

	return 0;
}
