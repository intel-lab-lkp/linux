// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_engine_user.h"
#include "intel_gt_ccs_mode.h"
#include "intel_gt_pm.h"
#include "intel_gt_print.h"
#include "intel_gt_regs.h"
#include "intel_gt_sysfs.h"
#include "i915_perf.h"
#include "sysfs_engines.h"

static void engine_update_mask(struct intel_gt *gt, u32 ccs_mode)
{
	unsigned long ccs_mask = gt->ccs.cslice_mask;
	struct intel_gt_info *info = &gt->info;
	int i;

	/* Mask off all the CCS engines */
	info->engine_mask &= ~GENMASK(CCS3, CCS0);

	for_each_set_bit(i, &ccs_mask, I915_MAX_CCS)
		info->engine_mask |= BIT(_CCS(i));
}

void intel_gt_ccs_mode_init(struct intel_gt *gt)
{
	struct intel_gt_info *info = &gt->info;
	unsigned long fused_mask;
	int ss_per_ccs;
	unsigned int i;
	u8 first_ccs;

	mutex_init(&gt->ccs.mutex);

	/* Calculate the slices considering the fused engines */
	ss_per_ccs = info->sseu.max_subslices / I915_MAX_CCS;
	fused_mask = intel_slicemask_from_xehp_dssmask(info->sseu.compute_subslice_mask,
						       ss_per_ccs);

	/* Remove the fused engines from the engine_mask */
	for_each_clear_bit(i, &fused_mask, I915_MAX_CCS) {
                info->engine_mask &= ~BIT(_CCS(i));
                gt_dbg(gt, "ccs%u fused off\n", i);
        }

	/*
	 * Store the number of active cslices before
	 * changing the CCS engine configuration
	 */
	gt->ccs.cslice_mask = CCS_MASK(gt);

	/*
	 * Normally only DG2 platforms have more than one CCS,
	 * no need to change the ccs balance settings all the GPU's.
	 */
	if (!IS_DG2(gt->i915))
		return;

	/*
	 * As a default behavior, do not create the command streamer for CCS
	 * slices beyond the first. All the workload submitted to the first
	 * engine will be shared among all the slices.
	 */
	first_ccs = __ffs(CCS_MASK(gt));

	/* Mask off all the CCS engine */
	info->engine_mask &= ~GENMASK(CCS3, CCS0);
	/* Put back in the first CCS engine */
	info->engine_mask |= BIT(_CCS(first_ccs));
}

void intel_gt_apply_ccs_mode(struct intel_gt *gt, u32 mode)
{
	unsigned long cslices_mask = gt->ccs.cslice_mask;
	u32 mode_val = 0;
	int ccs_id;
	int cslice;
	u32 m = mode;

	lockdep_assert_held(&gt->ccs.mutex);

	if (!IS_DG2(gt->i915))
		return;

	/*
	 * The mode has two bit dedicated for each engine
	 * that will be used for the CCS balancing algorithm:
	 *
	 *    BIT | CCS slice
	 *   ------------------
	 *     0  | CCS slice
	 *     1  |     0
	 *   ------------------
	 *     2  | CCS slice
	 *     3  |     1
	 *   ------------------
	 *     4  | CCS slice
	 *     5  |     2
	 *   ------------------
	 *     6  | CCS slice
	 *     7  |     3
	 *   ------------------
	 *
	 * When a CCS slice is not available, then we will write 0x7,
	 * oterwise we will write the user engine id which load will
	 * be forwarded to that slice.
	 *
	 * The possible configurations are:
	 *
	 * 1 engine (ccs0):
	 *   slice 0, 1, 2, 3: ccs0
	 *
	 * 2 engines (ccs0, ccs1):
	 *   slice 0, 2: ccs0
	 *   slice 1, 3: ccs1
	 *
	 * 4 engines (ccs0, ccs1, ccs2, ccs3):
	 *   slice 0: ccs0
	 *   slice 1: ccs1
	 *   slice 2: ccs2
	 *   slice 3: ccs3
	 */
	ccs_id = __ffs(cslices_mask);

	for (cslice = 0; cslice < I915_MAX_CCS; cslice++) {
		if (!(cslices_mask & BIT(cslice))) {
			/*
			 * If not available, mark the slice as unavailable
			 * and no task will be dispatched here.
			 */
			mode_val |= XEHP_CCS_MODE_CSLICE(cslice,
						     XEHP_CCS_MODE_CSLICE_MASK);
			continue;
		}

		mode_val |= XEHP_CCS_MODE_CSLICE(cslice, ccs_id);

		if (!m) {
			m = mode;
			ccs_id = __ffs(cslices_mask);
			continue;
		}

		m--;
		ccs_id = find_next_bit(&cslices_mask, I915_MAX_CCS, ccs_id + 1);
	}

	gt->ccs.mode_reg_val = mode;
}

static ssize_t num_cslices_show(struct device *dev,
				struct device_attribute *attr,
				char *buff)
{
	struct intel_gt *gt = kobj_to_gt(&dev->kobj);
	u32 num_slices;

	num_slices = hweight32(gt->ccs.cslice_mask);

	return sysfs_emit(buff, "%u\n", num_slices);
}
static DEVICE_ATTR_RO(num_cslices);

static ssize_t ccs_mode_show(struct device *dev,
			     struct device_attribute *attr, char *buff)
{
	struct intel_gt *gt = kobj_to_gt(&dev->kobj);
	u32 ccs_mode;

	ccs_mode = hweight32(CCS_MASK(gt));

	return sysfs_emit(buff, "%u\n", ccs_mode);
}

static ssize_t ccs_mode_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buff, size_t count)
{
	struct intel_gt *gt = kobj_to_gt(&dev->kobj);
	int num_cslices = hweight32(gt->ccs.cslice_mask);
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	intel_wakeref_t wakeref;
	ssize_t ret;
	u32 val;

	/*
	 * We don't want to change the CCS
	 * mode while someone is using the GT
	 */
	if (intel_gt_pm_is_awake(gt))
		return -EBUSY;

	ret = kstrtou32(buff, 0, &val);
	if (ret)
		return ret;

	/*
	 * As of now possible values to be set are 1, 2, 4,
	 * up to the maximum number of available slices
	 */
	if ((!val) || (val > num_cslices) || (num_cslices % val))
		return -EINVAL;

	/*
	 * Nothing to do if the requested setting
	 * is the same as the current one
	 */
	if (val == hweight32(CCS_MASK(gt)))
		return count;

	/* Recreate engine exposure */
	intel_engines_remove_sysfs(gt->i915);

	mutex_lock(&gt->ccs.mutex);
	intel_gt_apply_ccs_mode(gt, val - 1);
	mutex_unlock(&gt->ccs.mutex);

	wakeref = intel_runtime_pm_get(gt->uncore->rpm);

	i915_perf_fini(gt->i915);
	intel_engines_release(gt);
	intel_engines_free(gt);

	mutex_lock(&gt->ccs.mutex);
	engine_update_mask(gt, val);
	mutex_unlock(&gt->ccs.mutex);

	intel_engines_init_mmio(gt);
	i915_perf_init(gt->i915);
	intel_engines_init(gt);

	gt->i915->uabi_engines = RB_ROOT;
	intel_engines_driver_register(gt->i915);

	intel_runtime_pm_put(gt->uncore->rpm, wakeref);

	intel_engines_add_sysfs(gt->i915);

	return count;
}
static DEVICE_ATTR_RW(ccs_mode);

void intel_gt_sysfs_ccs_init(struct intel_gt *gt)
{
	int err;

	err = sysfs_create_file(&gt->sysfs_gt, &dev_attr_num_cslices.attr);
	if (err)
		gt_dbg(gt, "failed to create sysfs num_cslices files\n");

	/*
	 * Do not create the ccs_mode file for non DG2 platforms
	 * because they don't need it as they have only one CCS engine
	 */
	if (!IS_DG2(gt->i915))
		return;

	err = sysfs_create_file(&gt->sysfs_gt, &dev_attr_ccs_mode.attr);
	if (err)
		gt_dbg(gt, "failed to create sysfs ccs_mode files\n");
}
