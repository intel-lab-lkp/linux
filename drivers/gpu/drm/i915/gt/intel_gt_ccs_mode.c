//SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#include "i915_drv.h"

#include "intel_gt.h"
#include "intel_gt_ccs_mode.h"
#include "intel_gt_print.h"
#include "intel_gt_regs.h"
#include "intel_gt_types.h"

static void __intel_gt_apply_ccs_mode(struct intel_gt *gt)
{
	u32 mode = XEHP_CCS_MODE_CSLICE_0_3_MASK; /* disable all by default */
	int num_slices = hweight32(CCS_MASK(gt));
	int num_engines = gt->ccs.mode;
	int slice = 0;
	int i;

	if (!num_engines)
		return;

	/*
	 * Loop over all available slices and assign each a user engine.
	 *
	 * With 1 engine (ccs0):
	 *   slice 0, 1, 2, 3: ccs0
	 *
	 * With 2 engines (ccs0, ccs1):
	 *   slice 0, 2: ccs0
	 *   slice 1, 3: ccs1
	 *
	 * With 4 engines (ccs0, ccs1, ccs2, ccs3):
	 *   slice 0: ccs0
	 *   slice 1: ccs1
	 *   slice 2: ccs2
	 *   slice 3: ccs3
	 *
	 * Since the number of slices and the number of engines is
	 * known, and we ensure that there is an exact multiple of
	 * engines for slices, the double loop becomes a loop over each
	 * slice.
	 */
	for (i = num_slices / num_engines; i < num_slices; i++) {
		struct intel_engine_cs *engine;
		intel_engine_mask_t tmp;

		for_each_engine_masked(engine, gt, ALL_CCS(gt), tmp) {
			/* If a slice is fused off, leave disabled */
			while (!(CCS_MASK(gt) & BIT(slice)))
				slice++;

			mode &= ~XEHP_CCS_MODE_CSLICE(slice, XEHP_CCS_MODE_CSLICE_MASK);
			mode |= XEHP_CCS_MODE_CSLICE(slice, engine->instance);

			/* assign the next slice */
			slice++;
		}
	}

	intel_uncore_write(gt->uncore, XEHP_CCS_MODE, mode);
}

void intel_gt_apply_ccs_mode(struct intel_gt *gt)
{
	mutex_lock(&gt->ccs.mutex);
	__intel_gt_apply_ccs_mode(gt);
	mutex_unlock(&gt->ccs.mutex);
}

void intel_gt_init_ccs_mode(struct intel_gt *gt)
{
	mutex_init(&gt->ccs.mutex);
	gt->ccs.mode = 1;
}

void intel_gt_fini_ccs_mode(struct intel_gt *gt)
{
	mutex_destroy(&gt->ccs.mutex);
}

static ssize_t
ccs_mode_show(struct kobject *kobj, struct kobj_attribute *attr, char *buff)
{
	struct intel_gt *gt = container_of(kobj, struct intel_gt, sysfs_gt);

	return sysfs_emit(buff, "%u\n", gt->ccs.mode);
}

static ssize_t
ccs_mode_store(struct kobject *kobj, struct kobj_attribute *attr,
	       const char *buff, size_t count)
{
	struct intel_gt *gt = container_of(kobj, struct intel_gt, sysfs_gt);
	int num_slices = hweight32(CCS_MASK(gt));
	int err;
	u32 val;

	err = kstrtou32(buff, 0, &val);
	if (err)
		return err;

	if ((!val) || (val > num_slices) || (val % num_slices))
		return -EINVAL;

	mutex_lock(&gt->ccs.mutex);

	if (val == gt->ccs.mode)
		goto out;

	gt->ccs.mode = val;
	intel_gt_apply_ccs_mode(gt);

out:
	mutex_unlock(&gt->ccs.mutex);

	return count;
}

static ssize_t
num_slices_show(struct kobject *kobj, struct kobj_attribute *attr, char *buff)
{
	struct intel_gt *gt = container_of(kobj, struct intel_gt, sysfs_gt);
	u32 num_slices;

	num_slices = hweight32(CCS_MASK(gt));

	return sysfs_emit(buff, "%u\n", num_slices);
}

static struct kobj_attribute ccs_mode = __ATTR_RW(ccs_mode);
static struct kobj_attribute num_slices = __ATTR_RO(num_slices);

static const struct attribute * const ccs_mode_attrs[] = {
	&ccs_mode.attr,
	&num_slices.attr,
	NULL
};

void intel_gt_sysfs_ccs_mode(struct intel_gt *gt)
{
	int ret;

	ret = sysfs_create_files(&gt->sysfs_gt, ccs_mode_attrs);
	if (ret)
		gt_warn(gt, "Failed to create ccs mode sysfs files");
}
