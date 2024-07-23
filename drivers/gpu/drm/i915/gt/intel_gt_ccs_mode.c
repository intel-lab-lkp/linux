// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_gt.h"
#include "intel_gt_ccs_mode.h"
#include "intel_gt_print.h"
#include "intel_gt_regs.h"
#include "intel_gt_sysfs.h"

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

void intel_gt_apply_ccs_mode(struct intel_gt *gt)
{
	int cslice;
	u32 mode = 0;
	int first_ccs = __ffs(CCS_MASK(gt));

	lockdep_assert_held(&gt->ccs.mutex);

	if (!IS_DG2(gt->i915))
		return;

	/* Build the value for the fixed CCS load balancing */
	for (cslice = 0; cslice < I915_MAX_CCS; cslice++) {
		if (gt->ccs.cslice_mask & BIT(cslice))
			/*
			 * If available, assign the cslice
			 * to the first available engine...
			 */
			mode |= XEHP_CCS_MODE_CSLICE(cslice, first_ccs);

		else
			/*
			 * ... otherwise, mark the cslice as
			 * unavailable if no CCS dispatches here
			 */
			mode |= XEHP_CCS_MODE_CSLICE(cslice,
						     XEHP_CCS_MODE_CSLICE_MASK);
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

void intel_gt_sysfs_ccs_init(struct intel_gt *gt)
{
	int err;

	err = sysfs_create_file(&gt->sysfs_gt, &dev_attr_num_cslices.attr);
	if (err)
		gt_dbg(gt, "failed to create sysfs num_cslices files\n");
}
