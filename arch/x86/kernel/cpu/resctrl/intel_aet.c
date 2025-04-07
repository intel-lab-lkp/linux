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
#include <linux/minmax.h>
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

#define EVT(id, offset, _type) { .evtid = id, .evt_offset = offset, .type = _type }

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
 * @rmid_warned:		Set to stop multiple rmid sanity warnings
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
	bool	rmid_warned;
	int	num_events;
	struct pmt_event evts[];
};

/* Lookup table to get from resctrl event id to useful structures */
static struct evtinfo {
	struct telem_entry	*telem_entry;
	struct pmt_event	*pmt_event;
} evtinfo[QOS_NUM_EVENTS];

#define EVT_NUM_RMIDS(evtid)	(evtinfo[evtid].telem_entry->num_rmids)
#define EVT_NUM_EVENTS(evtid)	(evtinfo[evtid].telem_entry->num_events)
#define EVT_GUID(evtid)		(evtinfo[evtid].telem_entry->guid)

#define EVT_OFFSET(evtid)	(evtinfo[evtid].pmt_event->evt_offset)

/*
 * https://github.com/intel/Intel-PMT
 * xml/CWF/OOBMSM/RMID-ENERGY *.xml
 */
#define NUM_RMIDS_0x26696143	576
#define GUID_0x26696143		0x26696143
#define NUM_EVENTS_0x26696143	2
#define EVT_BYTES_0x26696143	(NUM_RMIDS_0x26696143 * NUM_EVENTS_0x26696143 * sizeof(u64))

static struct telem_entry energy_0x26696143 = {
	.name				= "energy",
	.guid				= GUID_0x26696143,
	.size				= EVT_BYTES_0x26696143 + sizeof(u64) * 3,
	.num_rmids			= NUM_RMIDS_0x26696143,
	.overflow_counter_off		= EVT_BYTES_0x26696143 + sizeof(u64) * 0,
	.last_overflow_tstamp_off	= EVT_BYTES_0x26696143 + sizeof(u64) * 1,
	.last_update_tstamp_off		= EVT_BYTES_0x26696143 + sizeof(u64) * 2,
	.num_events			= NUM_EVENTS_0x26696143,
	.evts				= {
		EVT(PMT_EVENT_ENERGY, 0x0, EVT_TYPE_U46_18),
		EVT(PMT_EVENT_ACTIVITY, 0x8, EVT_TYPE_U46_18),
	}
};

/*
 * https://github.com/intel/Intel-PMT
 * xml/CWF/OOBMSM/RMID-PERF *.xml
 */
#define NUM_RMIDS_0x26557651	576
#define GUID_0x26557651		0x26557651
#define NUM_EVENTS_0x26557651	7
#define EVT_BYTES_0x26557651	(NUM_RMIDS_0x26557651 * NUM_EVENTS_0x26557651 * sizeof(u64))

static struct telem_entry perf_0x26557651 = {
	.name				= "perf",
	.guid				= GUID_0x26557651,
	.size				= EVT_BYTES_0x26557651 + sizeof(u64) * 3,
	.num_rmids			= NUM_RMIDS_0x26557651,
	.overflow_counter_off		= EVT_BYTES_0x26557651 + sizeof(u64) * 0,
	.last_overflow_tstamp_off	= EVT_BYTES_0x26557651 + sizeof(u64) * 1,
	.last_update_tstamp_off		= EVT_BYTES_0x26557651 + sizeof(u64) * 2,
	.num_events			= NUM_EVENTS_0x26557651,
	.evts				= {
		EVT(PMT_EVENT_STALLS_LLC_HIT, 0x0, EVT_TYPE_U64),
		EVT(PMT_EVENT_C1_RES, 0x8, EVT_TYPE_U64),
		EVT(PMT_EVENT_UNHALTED_CORE_CYCLES, 0x10, EVT_TYPE_U64),
		EVT(PMT_EVENT_STALLS_LLC_MISS, 0x18, EVT_TYPE_U64),
		EVT(PMT_EVENT_AUTO_C6_RES, 0x20, EVT_TYPE_U64),
		EVT(PMT_EVENT_UNHALTED_REF_CYCLES, 0x28, EVT_TYPE_U64),
		EVT(PMT_EVENT_UOPS_RETIRED, 0x30, EVT_TYPE_U64),
	}
};

/* All known telemetry event groups */
static struct telem_entry *telem_entry[] = {
	&energy_0x26696143,
	&perf_0x26557651,
	NULL
};

static void rmid_sanity_check(struct telemetry_region *tr, struct telem_entry *tentry)
{
	struct rdt_resource *r = &rdt_resources_all[RDT_RESOURCE_PERF_PKG].r_resctrl;
	int system_rmids = boot_cpu_data.x86_cache_max_rmid + 1;

	if (tentry->rmid_warned)
		return;

	if (tentry->num_rmids != system_rmids) {
		pr_info("Telemetry region %s has %d RMIDs system supports %d\n",
			tentry->name, tentry->num_rmids, system_rmids);
		tentry->rmid_warned = true;
	}

	if (tr->num_rmids < tentry->num_rmids) {
		pr_info("Telemetry region %s only supports %d simultaneous RMIDS\n",
			tentry->name, tr->num_rmids);
		tentry->rmid_warned = true;
	}

	/* info/PKG_PERF_MON/num_rmids reports number of guaranteed counters */
	if (!r->num_rmid)
		r->num_rmid = tr->num_rmids;
	else
		r->num_rmid = min((u32)r->num_rmid, tr->num_rmids);
}

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
				rmid_sanity_check(tr, *tentry);
				found = true;
				(*tentry)->active = true;
				pkg[tr->plat_info.package_id].count++;
				break;
			}
		}
	}

	return found;
}

/*
 * Copy the pointers to telemetry regions associated with a given package
 * and with known guids over to the pkg_info structure for that package.
 */
static int setup(struct pkg_info *pkg, int pkgnum, struct pmt_feature_group *p, int slot)
{
	struct telem_entry **tentry;

	for (int i = 0; i < p->count; i++) {
		for (tentry = telem_entry; *tentry; tentry++) {
			if (!(*tentry)->active)
				continue;
			if (pkgnum != p->regions[i].plat_info.package_id)
				continue;
			if (p->regions[i].guid != (*tentry)->guid)
				continue;

			pkg[pkgnum].regions[slot++] =  p->regions[i];
		}
	}

	return slot;
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
	struct telem_entry **tentry;
	bool use_p1, use_p2;
	int slot;

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

	for (int i = 0; i < num_pkgs; i++) {
		if (!pkg[i].count)
			continue;
		pkg[i].regions = kmalloc_array(pkg[i].count, sizeof(*pkg[i].regions), GFP_KERNEL);
		if (!pkg[i].regions)
			return false;
		slot = 0;
		if (use_p1)
			slot = setup(pkg, i, p1, slot);
		if (use_p2)
			slot = setup(pkg, i, p2, slot);
	}

	for (tentry = telem_entry; *tentry; tentry++) {
		if (!(*tentry)->active)
			continue;
		for (int i = 0; i < (*tentry)->num_events; i++) {
			enum resctrl_event_id evtid = (*tentry)->evts[i].evtid;
			enum resctrl_event_type type;

			evtinfo[evtid].telem_entry = *tentry;
			evtinfo[evtid].pmt_event = &(*tentry)->evts[i];

			__set_bit(evtid, rdt_mon_features);
			type = (*tentry)->evts[i].type;
			resctrl_set_event_attributes(evtid, type, true);
		}
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

#define VALID_BIT	BIT_ULL(63)
#define DATA_BITS	GENMASK_ULL(62, 0)

/*
 * Walk the array of telemetry groups on a specific package.
 * Read and sum values for a specific counter (described by
 * guid and offset).
 * Return failure (~0x0ull) if any counter isn't valid.
 */
static u64 scan_pmt_devs(int package, int guid, int offset)
{
	u64 rval, val;
	int ndev = 0;

	rval = 0;

	for (int i = 0; i < pkg_info[package].count; i++) {
		if (pkg_info[package].regions[i].guid != guid)
			continue;
		ndev++;
		val = readq(pkg_info[package].regions[i].addr + offset);

		if (!(val & VALID_BIT))
			return ~0ull;
		rval += val & DATA_BITS;
	}

	return ndev ? rval : ~0ull;
}

/*
 * Read counter for an event on a domain (summing all aggregators
 * on the domain).
 */
int intel_aet_read_event(int domid, int rmid, int evtid, u64 *val)
{
	u64 evtcount;
	int offset;

	if (rmid >= EVT_NUM_RMIDS(evtid))
		return -ENOENT;

	offset = rmid * EVT_NUM_EVENTS(evtid) * sizeof(u64);
	offset += EVT_OFFSET(evtid);
	evtcount = scan_pmt_devs(domid, EVT_GUID(evtid), offset);

	if (evtcount != ~0ull || *val == 0)
		*val += evtcount;

	return evtcount != ~0ull ? 0 : -EINVAL;
}
