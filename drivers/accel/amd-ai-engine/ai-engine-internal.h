/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AMD AI Engine driver internal header
 *
 * Copyright(C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef AIE_INTERNAL_H
#define AIE_INTERNAL_H

#include <linux/amd-ai-engine.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#define AIE_DEVICE_GEN_AIE	1U
#define AIE_DEVICE_GEN_AIEML	2U

#define KBYTES(n)		((n) * SZ_1K)

/*
 * Macros for AI engine tile type bitmasks
 */
enum aie_tile_type {
	AIE_TILE_TYPE_TILE,
	AIE_TILE_TYPE_SHIMPL,
	AIE_TILE_TYPE_SHIMNOC,
	AIE_TILE_TYPE_MEMORY,
	AIE_TILE_TYPE_MAX
};

#define AIE_TILE_TYPE_MASK_TILE		BIT(AIE_TILE_TYPE_TILE)
#define AIE_TILE_TYPE_MASK_SHIMPL	BIT(AIE_TILE_TYPE_SHIMPL)
/* SHIM NOC tile includes SHIM PL and SHIM NOC modules */
#define AIE_TILE_TYPE_MASK_SHIMNOC	BIT(AIE_TILE_TYPE_SHIMNOC)
#define AIE_TILE_TYPE_MASK_MEMORY	BIT(AIE_TILE_TYPE_MEMORY)

/**
 * struct aie_tile_regs - contiguous range of AI engine register
 *			  within an AI engine tile
 * @soff: start offset of the range
 * @eoff: end offset of the range
 * @attribute: registers attribute. It uses AIE_REGS_ATTR_* macros defined
 *	       above.
 */
struct aie_tile_regs {
	size_t soff;
	size_t eoff;
	u32 attribute;
};

/**
 * struct aie_single_reg_field - AI engine single field register attribute
 * @mask: field mask
 * @regoff: register offset of the field
 */
struct aie_single_reg_field {
	u32 mask;
	u32 regoff;
};

struct aie_device;
struct aie_partition;
struct aie_aperture;

/**
 * struct aie_resource - AI engine resource structure
 * @bitmap: resource bitmap
 * @total: total number of resource
 */
struct aie_resource {
	unsigned long *bitmap;
	u32 total;
};

/**
 * struct aie_range - AIE range information
 * @start: start tile location
 * @size: size of the range, number of columns and rows
 */
struct aie_range {
	struct aie_location start;
	struct aie_location size;
};

/**
 * struct aie_mem - AIE memory information
 * @range: range of tiles of the memory
 * @offset: register offset within a tile of the memory
 * @size: of a the memory in one tile
 */
struct aie_mem {
	struct aie_range range;
	__kernel_size_t offset;
	__kernel_size_t size;
};

/**
 * struct aie_part_mem - AI engine partition memory information structure
 * @apart: AI engine partition
 * @mem: memory information of a type of memory
 * @size: size of the total memories in the partition
 *
 * This structure is to keep the information of a type of memory in a
 * partition. The memory information will be stored in @mem property.
 * The following information will be kept:
 *  * memory start address offset within a tile
 *  * memory size
 *  * what tiles contain this type of memory
 */
struct aie_part_mem {
	struct aie_partition *apart;
	struct aie_mem mem;
	size_t size;
};

/**
 * struct aie_tile_attr - AI engine device tile type attributes
 * @start_row: start row
 * @num_rows: number of rows
 * @num_mods: number of modules of this tile type
 * @mods: array of module types of this tile type
 */
struct aie_tile_attr {
	u8 start_row;
	u8 num_rows;
	u8 num_mods;
	const enum aie_module_type *mods;
};

/**
 * struct aie_tile_operations - AI engine device operations
 * @get_tile_type: get type of tile based on tile operation
 * @get_mem_info: get different types of memories information
 * @scan_part_clocks: scan partition modules to check whether the modules are
 *		      clock gated or not, and update the soft clock states
 *		      structure. It is required to be called when the partition
 *		      is requested so that the driver knows which modules are
 *		      clock gated when the partition is requested. This function
 *		      expects the caller to apply partition lock before calling
 *		      this function.
 * @set_part_clocks: set partition modules clocks gate registers based on the
 *		     partition clock states bitmap. This function expects the
 *		     caller to apply partition lock before calling this
 *		     function. The caller function will need to set the bitmap
 *		     on which tiles are required to be clocked on.
 * Different AI engine device version has its own device
 * operation.
 */
struct aie_tile_operations {
	u32 (*get_tile_type)(struct aie_device *adev, struct aie_location *loc);
	unsigned int (*get_mem_info)(struct aie_device *adev,
				     struct aie_range *range,
				     struct aie_part_mem *pmem);
	int (*scan_part_clocks)(struct aie_partition *apart);
	int (*set_part_clocks)(struct aie_partition *apart);
};

/**
 * struct aie_device - AI engine device structure
 * @apertures: list of apertures
 * @dev: device pointer for the AI engine device
 * @mlock: protection for AI engine device operations
 * @clk: AI enigne device clock
 * @ops: tile operations
 * @array_shift: array address shift
 * @col_shift: column address shift
 * @row_shift: row address shift
 * @dev_gen: aie hardware device generation
 * @pm_node_id: AI Engine platform management node ID
 * @ttype_attr: tile type attributes
 */
struct aie_device {
	struct list_head apertures;
	struct device *dev;
	struct mutex mlock; /* protection for AI engine apertures */
	struct clk *clk;
	const struct aie_tile_operations *ops;
	u32 array_shift;
	u32 col_shift;
	u32 row_shift;
	u32 dev_gen;
	u32 pm_node_id;
	struct aie_tile_attr ttype_attr[AIE_TILE_TYPE_MAX];
};

/**
 * struct aie_aperture - AI engine aperture structure
 * @node: list node
 * @partitions: list of partitions of this aperture
 * @dev: device pointer for the AI engine aperture device
 * @adev: pointer to AI device instance
 * @mlock: protection for AI engine aperture operations
 * @base: AI engine aperture base virtual address
 * @cols_res: AI engine columns resources to indicate
 *	      while columns are occupied by partitions.
 * @node_id: AI engine aperture node id which is to identify
 *	     the aperture in the system in firmware
 * @range: range of aperture
 */
struct aie_aperture {
	struct list_head node;
	struct list_head partitions;
	struct device *dev;
	struct aie_device *adev;
	struct mutex mlock; /* protection for AI engine aperture operations */
	void __iomem *base;
	struct aie_resource cols_res;
	u32 node_id;
	struct aie_range range;
};

/**
 * struct aie_partition - AI engine partition structure
 * @node: list node
 * @aperture: pointer to AI engine aperture
 * @adev: pointer to AI device instance
 * @range: range of partition
 * @cores_clk_state: bitmap to indicate the power state of core and mem tiles
 * @tiles_inuse: bitmap to indicate if a tile is in use
 * @pmems: pointer to partition memories types
 * @mlock: protection for AI engine partition operations
 * @freq_req: required frequency
 */
struct aie_partition {
	struct list_head node;
	struct aie_aperture *aperture;
	struct aie_device *adev;
	struct aie_range range;
	struct aie_resource cores_clk_state;
	struct aie_resource tiles_inuse;
	struct aie_part_mem *pmems;
	struct mutex mlock; /* protection for AI engine partition operations */
	u64 freq_req;
};

#define dev_to_aiedev(_dev) container_of((_dev), struct aie_device, dev)
#define dev_to_aieaperture(_dev) container_of((_dev), struct aie_aperture, dev)
#define dev_to_aiepart(_dev) container_of((_dev), struct aie_partition, dev)

#define aie_col_mask(adev) ({ \
	struct aie_device *_adev = (adev); \
	GENMASK_ULL(_adev->array_shift - 1, _adev->col_shift);  \
	})

#define aie_row_mask(adev) ({ \
	struct aie_device *_adev = (adev); \
	GENMASK_ULL(_adev->col_shift - 1, _adev->row_shift);  \
	})

#define aie_tile_reg_mask(adev) ({ \
	struct aie_device *_adev = (adev); \
	GENMASK_ULL(_adev->row_shift - 1, 0);  \
	})

/*
 * Need to define field get, as AI engine shift mask is not constant.
 * Cannot use FIELD_GET()
 */
#define aie_tile_reg_field_get(mask, shift, regoff) ( \
	((regoff) & (mask)) >> (shift))

#define aie_cal_tile_reg(adev, regoff) ( \
	aie_tile_reg_field_get(aie_tile_reg_mask(adev), 0, regoff))

/**
 * aie_cal_regoff() - calculate register offset to the whole AI engine
 *		      device start address
 * @adev: AI engine device
 * @loc: AI engine tile location
 * @regoff_intile: register offset within a tile
 * @return: register offset to the whole AI engine device start address
 */
static inline u32 aie_cal_regoff(struct aie_device *adev,
				 struct aie_location loc, u32 regoff_intile)
{
	return regoff_intile + (loc.col << adev->col_shift) +
	       (loc.row << adev->row_shift);
}

void aie_device_init(struct aie_device *adev);
void aieml_device_init(struct aie_device *adev);
struct aie_partition *
aie_aperture_request_part(struct aie_aperture *aperture,
			  struct aie_partition_req *req);
int aie_aperture_probe(struct platform_device *pdev);
void aie_aperture_remove(struct platform_device *pdev);
struct aie_partition *aie_part_create(struct aie_aperture *aperture,
				      u8 start_col, u8 num_col);
void aie_part_release(struct aie_partition *apart);
int aie_part_set_freq(struct aie_partition *apart, u64 freq);
int aie_part_scan_clk_state(struct aie_partition *apart);
bool aie_part_check_clk_enable_loc(struct aie_partition *apart,
				   struct aie_location *loc);
int aie_part_request_tiles(struct aie_partition *apart, int num_tiles,
			   struct aie_location *locs);
int aie_part_release_tiles(struct aie_partition *apart, int num_tiles,
			   struct aie_location *locs);
int aie_resource_initialize(struct aie_resource *res, int count);
void aie_resource_uninitialize(struct aie_resource *res);
int aie_resource_check_region(struct aie_resource *res, u32 start,
			      u32 count);
int aie_resource_get_region(struct aie_resource *res, u32 start,
			    u32 count);
void aie_resource_put_region(struct aie_resource *res, int start, u32 count);
int aie_resource_set(struct aie_resource *res, u32 start, u32 count);
int aie_resource_clear(struct aie_resource *res, u32 start, u32 count);
bool aie_resource_testbit(struct aie_resource *res, u32 bit);

#endif /* AIE_INTERNAL_H */
