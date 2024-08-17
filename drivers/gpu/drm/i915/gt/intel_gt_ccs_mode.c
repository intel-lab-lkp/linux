// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include "i915_drv.h"
#include "intel_gt_ccs_mode.h"
#include "intel_gt_print.h"
#include "intel_gt_regs.h"
#include "intel_gt_sysfs.h"

void intel_gt_apply_ccs_mode(struct intel_gt *gt, u32 mode)
{
	unsigned long cslices_mask = CCS_MASK(gt);
	u32 mode_val = 0;
	u32 m = mode;
	int ccs_id;
	int cslice;

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

void intel_gt_ccs_mode_init(struct intel_gt *gt)
{
	mutex_init(&gt->ccs.mutex);
}

static ssize_t num_cslices_show(struct device *dev,
				struct device_attribute *attr,
				char *buff)
{
	struct intel_gt *gt = kobj_to_gt(&dev->kobj);
	u32 num_slices;

	num_slices = hweight32(CCS_MASK(gt));

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
