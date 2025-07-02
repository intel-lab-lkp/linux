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

void *aie_partition_request(struct device *dev, struct aie_partition_req *req);
void aie_partition_release(void *apart);
int aie_partition_set_freq_req(void *apart, u64 freq);
int aie_partition_get_freq(void *apart, u64 *freq);
int aie_partition_get_freq_req(void *apart, u64 *freq);

#endif
