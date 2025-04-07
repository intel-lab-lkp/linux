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

/* Per-package event groups active on this machine */
static struct pkg_info {
	int			count;
	struct telemetry_region	*regions;
} *pkg_info;

/**
 * struct pmt_event - Telemetry event.
 * @evtid:	Resctrl event id
 * @evt_offset:	MMIO offset of counter
 * @type:	Type for format user display of event value
 */
struct pmt_event {
	enum resctrl_event_id	evtid;
	int			evt_offset;
	enum resctrl_event_type	type;
};

/**
 * struct telem_entry - Summarized form from XML telemetry description
 * @name:			Name for this group of events
 * @guid:			Unique ID for this group
 * @size:			Size of MMIO mapped counter registers
 * @num_rmids:			Number of RMIDS supported
 * @overflow_counter_off:	Offset of overflow count
 * @last_overflow_tstamp_off:	Offset of overflow timestamp
 * @last_update_tstamp_off:	Offset of last update timestamp
 * @active:			Marks this group as active on this system
 * @num_events:			Size of @evts array
 * @evts:			Telemetry events in this group
 */
struct telem_entry {
	char	*name;
	int	guid;
	int	size;
	int	num_rmids;
	int	overflow_counter_off;
	int	last_overflow_tstamp_off;
	int	last_update_tstamp_off;
	bool	active;
	int	num_events;
	struct pmt_event evts[];
};

/* All known telemetry event groups */
static struct telem_entry *telem_entry[] = {
	NULL
};

/*
 * Scan a feature group looking for guids recognized
 * and update the per-package counts of known groups.
 */
static bool count_events(struct pkg_info *pkg, int max_pkgs, struct pmt_feature_group *p)
{
	struct telem_entry **tentry;
	bool found = false;

	if (IS_ERR_OR_NULL(p))
		return false;

	for (int i = 0; i < p->count; i++) {
		struct telemetry_region *tr = &p->regions[i];

		for (tentry = telem_entry; *tentry; tentry++) {
			if (tr->guid == (*tentry)->guid) {
				if (tr->plat_info.package_id > max_pkgs) {
					pr_warn_once("Bad package %d\n", tr->plat_info.package_id);
					continue;
				}
				if (tr->size > (*tentry)->size) {
					pr_warn_once("MMIO region for guid 0x%x too small\n", tr->guid);
					continue;
				}
				found = true;
				(*tentry)->active = true;
				pkg[tr->plat_info.package_id].count++;
				break;
			}
		}
	}

	return found;
}

DEFINE_FREE(intel_pmt_put_feature_group, struct pmt_feature_group *,	\
	if (!IS_ERR_OR_NULL(_T))					\
		intel_pmt_put_feature_group(_T))

DEFINE_FREE(free_pkg_info, struct pkg_info *,				\
	if (_T)								\
		for (int i = 0; i < topology_max_packages(); i++)	\
			kfree(_T[i].regions);				\
	kfree(_T))
/*
 * Ask OOBMSM discovery driver for all the RMID based telemetry groups
 * that it supports.
 */
bool intel_aet_get_events(void)
{
	struct pmt_feature_group *p1 __free(intel_pmt_put_feature_group) = NULL;
	struct pmt_feature_group *p2 __free(intel_pmt_put_feature_group) = NULL;
	struct pkg_info *pkg __free(free_pkg_info) = NULL;
	int num_pkgs = topology_max_packages();
	bool use_p1, use_p2;

	pkg = kcalloc(num_pkgs, sizeof(*pkg_info), GFP_KERNEL);
	if (!pkg)
		return false;

	p1 = intel_pmt_get_regions_by_feature(FEATURE_PER_RMID_ENERGY_TELEM);
	p2 = intel_pmt_get_regions_by_feature(FEATURE_PER_RMID_PERF_TELEM);
	use_p1 = count_events(pkg, num_pkgs, p1);
	use_p2 = count_events(pkg, num_pkgs, p2);

	if (!use_p1 && !use_p2)
		return false;

	if (!resctrl_arch_mon_capable()) {
		pr_info("Telemetry available but monitor support disabled\n");
		return false;
	}

	if (use_p1)
		feat_energy = no_free_ptr(p1);
	if (use_p2)
		feat_perf = no_free_ptr(p2);
	pkg_info = no_free_ptr(pkg);

	return true;
}

void __exit intel_aet_exit(void)
{
	if (feat_energy)
		intel_pmt_put_feature_group(feat_energy);
	if (feat_perf)
		intel_pmt_put_feature_group(feat_perf);

	if (pkg_info) {
		for (int i = 0; i < topology_max_packages(); i++)
			kfree(pkg_info[i].regions);
	}
	kfree(pkg_info);
}
