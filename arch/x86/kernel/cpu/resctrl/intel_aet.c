// SPDX-License-Identifier: GPL-2.0-only
/*
 * Resource Director Technology(RDT)
 * - Intel Application Energy Telemetry
 *
 * Copyright (C) 2025 Intel Corporation
 *
 * Author:
 *    Tony Luck <tony.luck@intel.com>
 */

#define pr_fmt(fmt)   "resctrl: " fmt

#include <linux/cpu.h>
#include <linux/cleanup.h>
#include "fake_intel_aet_features.h"
#include <linux/intel_vsec.h>
#include <linux/resctrl.h>
#include <linux/slab.h>

#include "internal.h"

static struct pmt_feature_group *feat_energy;
static struct pmt_feature_group *feat_perf;

DEFINE_FREE(intel_pmt_put_feature_group, struct pmt_feature_group *,	\
	if (!IS_ERR_OR_NULL(_T))					\
		intel_pmt_put_feature_group(_T))

/*
 * Ask OOBMSM discovery driver for all the RMID based telemetry groups
 * that it supports.
 */
bool intel_aet_get_events(void)
{
	struct pmt_feature_group *p1 __free(intel_pmt_put_feature_group) = NULL;
	struct pmt_feature_group *p2 __free(intel_pmt_put_feature_group) = NULL;
	bool use_p1, use_p2;

	p1 = intel_pmt_get_regions_by_feature(FEATURE_PER_RMID_ENERGY_TELEM);
	p2 = intel_pmt_get_regions_by_feature(FEATURE_PER_RMID_PERF_TELEM);
	use_p1 = !IS_ERR_OR_NULL(p1);
	use_p2 = !IS_ERR_OR_NULL(p2);

	if (!use_p1 && !use_p2)
		return false;

	if (use_p1)
		feat_energy = no_free_ptr(p1);
	if (use_p2)
		feat_perf = no_free_ptr(p2);

	return true;
}

void __exit intel_aet_exit(void)
{
	if (feat_energy)
		intel_pmt_put_feature_group(feat_energy);
	if (feat_perf)
		intel_pmt_put_feature_group(feat_perf);
}
