// SPDX-License-Identifier: GPL-2.0
/*
 * AMD AI Engine clock operations
 *
 * Copyright(C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 */

#include <linux/amd-ai-engine.h>
#include <linux/clk.h>
#include <linux/firmware/xlnx-zynqmp.h>
#include <linux/list.h>
#include <linux/mutex.h>

#include "ai-engine-internal.h"

/**
 * aie_part_get_clk_state_bit() - return bit position of the clock state of a
 *				  tile
 * @apart: AI engine partition
 * @loc: AI engine tile location
 *
 * Return: bit position for success, negative value for failure
 */
static int aie_part_get_clk_state_bit(struct aie_partition *apart,
				      struct aie_location *loc)
{
	u32 ttype = apart->adev->ops->get_tile_type(apart->adev, loc);

	if (ttype != AIE_TILE_TYPE_TILE && ttype != AIE_TILE_TYPE_MEMORY)
		return -EINVAL;

	return (loc->col - apart->range.start.col) *
	       (apart->range.size.row - 1) + loc->row - 1;
}

/**
 * aie_part_scan_clk_state() - scan the clock states of tiles of the AI engine
 *			       partition
 * @apart: AI engine partition
 *
 * Return: 0 for success, negative value for failure.
 *
 * This function will scan the clock status of both the memory and core
 * modules.
 */
int aie_part_scan_clk_state(struct aie_partition *apart)
{
	return apart->adev->ops->scan_part_clocks(apart);
}

/**
 * aie_part_check_clk_enable_loc() - return if clock of a tile is enabled
 * @apart: AI engine partition
 * @loc: AI engine tile location
 *
 * Return: true for enabled, false for disabled
 */
bool aie_part_check_clk_enable_loc(struct aie_partition *apart,
				   struct aie_location *loc)
{
	u32 ttype = apart->adev->ops->get_tile_type(apart->adev, loc);
	int bit;

	if (ttype != AIE_TILE_TYPE_TILE && ttype != AIE_TILE_TYPE_MEMORY)
		return true;

	bit = aie_part_get_clk_state_bit(apart, loc);
	return aie_resource_testbit(&apart->cores_clk_state, bit);
}

/**
 * aie_part_request_tiles() - request tiles from an AI engine partition.
 * @apart: AI engine partition
 * @num_tiles: number of tiles to request. If it is 0, it means all tiles
 * @locs: the AI engine tiles locations array which will be requested
 *
 * Return: 0 for success, negative value for failure.
 *
 * This function will enable clocks of the specified tiles.
 */
int aie_part_request_tiles(struct aie_partition *apart, int num_tiles,
			   struct aie_location *locs)
{
	int ret;

	mutex_lock(&apart->mlock);
	if (num_tiles == 0) {
		aie_resource_set(&apart->tiles_inuse, 0,
				 apart->tiles_inuse.total);
	} else {
		u32 n;

		if (!locs) {
			mutex_unlock(&apart->mlock);
			return -EINVAL;
		}

		for (n = 0; n < num_tiles; n++) {
			int bit = aie_part_get_clk_state_bit(apart, &locs[n]);

			if (bit >= 0)
				aie_resource_set(&apart->tiles_inuse, bit, 1);
		}
	}
	ret = apart->adev->ops->set_part_clocks(apart);
	mutex_unlock(&apart->mlock);

	return ret;
}

/**
 * aie_part_release_tiles() - release tiles from an AI engine partition.
 * @apart: AI engine partition
 * @num_tiles: number of tiles to release. If it is 0, it means all tiles
 * @locs: the AI engine tiles locations array which will be released
 *
 * Return: 0 for success, negative value for failure.
 *
 * This function will disable clocks of the specified tiles.
 */
int aie_part_release_tiles(struct aie_partition *apart, int num_tiles,
			   struct aie_location *locs)
{
	int ret;

	mutex_lock(&apart->mlock);
	if (num_tiles == 0) {
		aie_resource_clear(&apart->tiles_inuse, 0,
				   apart->tiles_inuse.total);
	} else {
		u32 n;

		if (!locs) {
			mutex_unlock(&apart->mlock);
			return -EINVAL;
		}

		for (n = 0; n < num_tiles; n++) {
			int bit = aie_part_get_clk_state_bit(apart, &locs[n]);

			if (bit >= 0)
				aie_resource_clear(&apart->tiles_inuse, bit, 1);
		}
	}

	ret = apart->adev->ops->set_part_clocks(apart);
	mutex_unlock(&apart->mlock);

	return ret;
}

/**
 * aie_aperture_get_freq_req() - get current required frequency of aperture
 * @aperture: AI engine aperture
 *
 * Return: required clock frequency of the aperture which is the largest
 *	   required clock frequency of all partitions of the aperture. If
 *	   return value is 0, it means no partition has specific frequency
 *	   requirement.
 */
static unsigned long aie_aperture_get_freq_req(struct aie_aperture *aperture)
{
	struct aie_partition *apart;
	unsigned long freq_req = 0;

	mutex_lock(&aperture->mlock);
	list_for_each_entry(apart, &aperture->partitions, node) {
		if (apart->freq_req > freq_req)
			freq_req = apart->freq_req;
	}
	mutex_unlock(&aperture->mlock);

	return freq_req;
}

/**
 * aie_part_set_freq() - set frequency requirement of an AI engine partition
 *
 * @apart: AI engine partition
 * @freq: required frequency
 *
 * Return: 0 for success, negative value for failure
 *
 * This function sets frequency requirement for the partition.
 * It will call aie_dev_set_freq() to check the frequency requirements
 * of all partitions. it will send QoS EEMI request to request the max
 * frequency of all the partitions.
 */
int aie_part_set_freq(struct aie_partition *apart, u64 freq)
{
	struct aie_aperture *aperture = apart->aperture;
	struct aie_device *adev = apart->adev;
	u32 boot_qos, current_qos, target_qos;
	unsigned long clk_rate;
	u64 temp_freq;
	int ret;

	clk_rate = clk_get_rate(adev->clk);
	if (freq > (u64)clk_rate) {
		dev_err(aperture->dev,
			"Invalid frequency to set, larger than full frequency(%lu).\n",
			clk_rate);
		return -EINVAL;
	}
	mutex_lock(&apart->mlock);

	temp_freq = apart->freq_req;
	apart->freq_req = freq;

	freq = aie_aperture_get_freq_req(aperture);
	if (!freq) {
		mutex_unlock(&apart->mlock);
		return 0;
	}

	ret = zynqmp_pm_get_qos(aperture->node_id, &boot_qos, &current_qos);
	if (ret < 0) {
		dev_err(aperture->dev, "Failed to get clock divider value.\n");
		goto out;
	}

	target_qos = (boot_qos * clk_rate) / freq;

	/* The clock divisor value (QoS) is a 10-bit value */
	if (target_qos > (BIT(10) - 1)) {
		/*
		 * Reset the logged partition frequency requirement to its
		 * previous value.
		 */
		dev_err(aperture->dev,
			"Failed to set frequency requirement. Frequency value out-of bound.\n");
		ret = -EINVAL;
		goto out;
	}

	ret = zynqmp_pm_set_requirement(aperture->node_id,
					ZYNQMP_PM_CAPABILITY_ACCESS, target_qos,
					ZYNQMP_PM_REQUEST_ACK_BLOCKING);
	if (ret < 0) {
		dev_err(aperture->dev, "Failed to set frequency requirement.\n");
		goto out;
	}

	mutex_unlock(&apart->mlock);
	return 0;
out:
	apart->freq_req = temp_freq;
	mutex_unlock(&apart->mlock);
	return ret;
}

/**
 * aie_partition_set_freq_req() - set partition frequency requirement
 *
 * @apart: AI engine partition instance
 * @freq: required frequency
 *
 * Return: 0 for success, negative value for failure
 *
 * This function sets the minimum required frequency for the AI engine
 * partition. If there are other partitions requiring a higher frequency in the
 * system, AI engine device will be clocked at that value to satisfy frequency
 * requirements of all partitions.
 */
int aie_partition_set_freq_req(void *apart, u64 freq)
{
	if (!apart)
		return -EINVAL;
	return aie_part_set_freq((struct aie_partition *)apart, freq);
}
EXPORT_SYMBOL_GPL(aie_partition_set_freq_req);

/**
 * aie_part_get_freq() - get running frequency of AI engine device.
 *
 * @apart: AI engine partition
 * @freq: return running frequency
 *
 * Return: 0 for success, negative value for failure
 *
 * This function gets clock divider value with EEMI requests, and it gets the
 * full clock frequency from common clock framework. And then it divides the
 * full clock frequency by the divider value and returns the result.
 */
static int aie_part_get_freq(struct aie_partition *apart, u64 *freq)
{
	struct aie_aperture *aperture = apart->aperture;
	struct aie_device *adev = apart->adev;
	u32 boot_qos, current_qos;
	unsigned long clk_rate;
	int ret;

	clk_rate = clk_get_rate(adev->clk);
	ret = zynqmp_pm_get_qos(aperture->node_id, &boot_qos,
				&current_qos);
	if (ret < 0) {
		dev_err(aperture->dev, "Failed to get clock divider value.\n");
		return ret;
	}

	*freq = (clk_rate * boot_qos) / current_qos;
	return 0;
}

/**
 * aie_partition_get_freq() - get partition running frequency
 *
 * @apart: AI engine partition instance
 * @freq: return running frequency
 *
 * Return: 0 for success, negative value for failure
 */
int aie_partition_get_freq(void *apart, u64 *freq)
{
	if (!apart || !freq)
		return -EINVAL;
	return aie_part_get_freq((struct aie_partition *)apart, freq);
}
EXPORT_SYMBOL_GPL(aie_partition_get_freq);

/**
 * aie_partition_get_freq_req() - get partition required frequency
 *
 * @apart: AI engine partition instance
 * @freq: return partition required frequency. 0 means partition doesn't have
 *	  frequency requirement.
 *
 * Return: 0 for success, negative value for failure
 */
int aie_partition_get_freq_req(void *apart, u64 *freq)
{
	if (!apart || !freq)
		return -EINVAL;

	*freq = ((struct aie_partition *)apart)->freq_req;
	return 0;
}
EXPORT_SYMBOL_GPL(aie_partition_get_freq_req);
