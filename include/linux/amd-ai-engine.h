/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * amd-ai-engine.h - AMD AI engine external interface
 *
 * Copyright(C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef _AMD_AI_ENGINE_H_
#define _AMD_AI_ENGINE_H_

#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>

/*
 * AI engine partition initialize options
 */
#define AIE_PART_INIT_OPT_COLUMN_RST		BIT(0)
#define AIE_PART_INIT_OPT_SHIM_RST		BIT(1)
#define AIE_PART_INIT_OPT_BLOCK_NOCAXIMMERR	BIT(2)
#define AIE_PART_INIT_OPT_ISOLATE		BIT(3)
#define AIE_PART_INIT_OPT_ZEROIZEMEM		BIT(4)
#define AIE_PART_INIT_OPT_DEFAULT		GENMASK(3, 0)

/**
 * struct aie_partition_req - AIE request partition arguments
 * @start_col: start column of the partition
 * @num_col: number of columns in a partition
 * @uid: image identifier loaded on the AI engine partition
 * @meta_data: meta data to indicate which resources used by application.
 * @flag: used for application to indicate particular driver requirements
 *	  application wants to have for the partition. e.g. do not clean
 *	  resource when closing the partition.
 */
struct aie_partition_req {
	u8  start_col;
	u8  num_col;
	u32 uid;
	u64 meta_data;
	u32 flag;
};

/**
 * struct aie_location - AIE location information
 * @col: column id
 * @row: row id
 */
struct aie_location {
	u32 col;
	u32 row;
};

/**
 * struct aie_partition_init_args - AIE partition initialization arguments
 * @locs: Allocated array of tile locations that will be used
 * @num_tiles: Number of tiles to use
 * @init_opts: Partition initialization options
 */
struct aie_partition_init_args {
	struct aie_location *locs;
	u32 num_tiles;
	u32 init_opts;
};

void *aie_partition_request(struct device *dev, struct aie_partition_req *req);
void aie_partition_release(void *apart);
int aie_partition_initialize(void *apart, struct aie_partition_init_args *args);
int aie_partition_teardown(void *apart);
int aie_partition_set_freq_req(void *apart, u64 freq);
int aie_partition_get_freq(void *apart, u64 *freq);
int aie_partition_get_freq_req(void *apart, u64 *freq);

#endif
