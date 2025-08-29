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

#include <linux/cleanup.h>
#include <linux/cpu.h>
#include <linux/intel_vsec.h>
#include <linux/resctrl.h>

#include "internal.h"

/**
 * struct event_group - All information about a group of telemetry events.
 * @pfg:		Points to the aggregated telemetry space information
 *			within the INTEL_PMT_TELEMETRY driver that contains data for all
 *			telemetry regions.
 * @guid:		Unique number per XML description file.
 */
struct event_group {
	/* Data fields for additional structures to manage this group. */
	struct pmt_feature_group	*pfg;

	/* Remaining fields initialized from XML file. */
	u32				guid;
};

/*
 * Link: https://github.com/intel/Intel-PMT
 * File: xml/CWF/OOBMSM/RMID-ENERGY/cwf_aggregator.xml
 */
static struct event_group energy_0x26696143 = {
	.guid		= 0x26696143,
};

/*
 * Link: https://github.com/intel/Intel-PMT
 * File: xml/CWF/OOBMSM/RMID-PERF/cwf_aggregator.xml
 */
static struct event_group perf_0x26557651 = {
	.guid		= 0x26557651,
};

static struct event_group *known_energy_event_groups[] = {
	&energy_0x26696143,
};

static struct event_group *known_perf_event_groups[] = {
	&perf_0x26557651,
};

#define for_each_enabled_event_group(_peg, _grp)			\
	for (_peg = _grp; _peg < &_grp[ARRAY_SIZE(_grp)]; _peg++)	\
		if ((*_peg)->pfg)

/* Stub for now */
static bool enable_events(struct event_group *e, struct pmt_feature_group *p)
{
	return false;
}

DEFINE_FREE(intel_pmt_put_feature_group, struct pmt_feature_group *,
		if (!IS_ERR_OR_NULL(_T))
			intel_pmt_put_feature_group(_T))

/*
 * Make a request to the INTEL_PMT_TELEMETRY driver for the pmt_feature_group
 * for a specific feature. If there is one the returned structure has an array
 * of telemetry_region structures. Each describes one telemetry aggregator.
 * Try to use every telemetry aggregator with a known guid.
 */
static bool get_pmt_feature(enum pmt_feature_id feature, struct event_group **evgs,
			    unsigned int num_evg)
{
	struct pmt_feature_group *p __free(intel_pmt_put_feature_group) = NULL;
	struct event_group **peg;
	int ret;

	p = intel_pmt_get_regions_by_feature(feature);

	if (IS_ERR_OR_NULL(p))
		return false;

	for (peg = evgs; peg < &evgs[num_evg]; peg++) {
		ret = enable_events(*peg, p);
		if (ret) {
			(*peg)->pfg = no_free_ptr(p);
			return true;
		}
	}

	return false;
}

/*
 * Ask INTEL_PMT_TELEMETRY driver for all the RMID based telemetry groups
 * that it supports.
 */
bool intel_aet_get_events(void)
{
	bool ret1, ret2;

	ret1 = get_pmt_feature(FEATURE_PER_RMID_ENERGY_TELEM,
			       known_energy_event_groups,
			       ARRAY_SIZE(known_energy_event_groups));
	ret2 = get_pmt_feature(FEATURE_PER_RMID_PERF_TELEM,
			       known_perf_event_groups,
			       ARRAY_SIZE(known_perf_event_groups));

	return ret1 || ret2;
}

void __exit intel_aet_exit(void)
{
	struct event_group **peg;

	for_each_enabled_event_group(peg, known_energy_event_groups) {
		intel_pmt_put_feature_group((*peg)->pfg);
		(*peg)->pfg = NULL;
	}
	for_each_enabled_event_group(peg, known_perf_event_groups) {
		intel_pmt_put_feature_group((*peg)->pfg);
		(*peg)->pfg = NULL;
	}
}
