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
#include <linux/io.h>
#include <linux/minmax.h>
#include <linux/resctrl.h>

#include "internal.h"

/**
 * struct pmt_event - Telemetry event.
 * @id:		Resctrl event id.
 * @idx:	Counter index within each per-RMID block of counters.
 * @bin_bits:	Zero for integer valued events, else number bits in fraction
 *		part of fixed-point.
 */
struct pmt_event {
	enum resctrl_event_id	id;
	unsigned int		idx;
	unsigned int		bin_bits;
};

#define EVT(_id, _idx, _bits) { .id = _id, .idx = _idx, .bin_bits = _bits }

/**
 * struct event_group - All information about a group of telemetry events.
 * @name:		Name for this group (used by boot rdt= option)
 * @pfg:		Points to the aggregated telemetry space information
 *			within the INTEL_PMT_TELEMETRY driver that contains data for all
 *			telemetry regions.
 * @guid:		Unique number per XML description file.
 * @num_rmids:		Number of RMIDs supported by this group. May be djusted downwards
 *			if enumeration from intel_pmt_get_regions_by_feature() indicates
 *			fewer RMIDs can be tracked simultaneously.
 * @mmio_size:		Number of bytes of MMIO registers for this group.
 * @num_events:		Number of events in this group.
 * @evts:		Array of event descriptors.
 */
struct event_group {
	/* Data fields for additional structures to manage this group. */
	char				*name;
	struct pmt_feature_group	*pfg;

	/* Remaining fields initialized from XML file. */
	u32				guid;
	u32				num_rmids;
	size_t				mmio_size;
	unsigned int			num_events;
	struct pmt_event		evts[] __counted_by(num_events);
};

#define XML_MMIO_SIZE(num_rmids, num_events, num_extra_status) \
		      (((num_rmids) * (num_events) + (num_extra_status)) * sizeof(u64))

/*
 * Link: https://github.com/intel/Intel-PMT
 * File: xml/CWF/OOBMSM/RMID-ENERGY/cwf_aggregator.xml
 */
static struct event_group energy_0x26696143 = {
	.name		= "energy",
	.guid		= 0x26696143,
	.num_rmids	= 576,
	.mmio_size	= XML_MMIO_SIZE(576, 2, 3),
	.num_events	= 2,
	.evts		= {
		EVT(PMT_EVENT_ENERGY, 0, 18),
		EVT(PMT_EVENT_ACTIVITY, 1, 18),
	}
};

/*
 * Link: https://github.com/intel/Intel-PMT
 * File: xml/CWF/OOBMSM/RMID-PERF/cwf_aggregator.xml
 */
static struct event_group perf_0x26557651 = {
	.name		= "perf",
	.guid		= 0x26557651,
	.num_rmids	= 576,
	.mmio_size	= XML_MMIO_SIZE(576, 7, 3),
	.num_events	= 7,
	.evts		= {
		EVT(PMT_EVENT_STALLS_LLC_HIT, 0, 0),
		EVT(PMT_EVENT_C1_RES, 1, 0),
		EVT(PMT_EVENT_UNHALTED_CORE_CYCLES, 2, 0),
		EVT(PMT_EVENT_STALLS_LLC_MISS, 3, 0),
		EVT(PMT_EVENT_AUTO_C6_RES, 4, 0),
		EVT(PMT_EVENT_UNHALTED_REF_CYCLES, 5, 0),
		EVT(PMT_EVENT_UOPS_RETIRED, 6, 0),
	}
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

static bool skip_this_region(struct telemetry_region *tr, struct event_group *e)
{
	if (tr->guid != e->guid)
		return true;
	if (tr->plat_info.package_id >= topology_max_packages()) {
		pr_warn_once("Bad package %d in guid 0x%x\n", tr->plat_info.package_id,
			     tr->guid);
		return true;
	}
	if (tr->size != e->mmio_size) {
		pr_warn_once("MMIO space wrong size (%zu bytes) for guid 0x%x. Expected %zu bytes.\n",
			     tr->size, e->guid, e->mmio_size);
		return true;
	}

	return false;
}

static bool all_regions_have_sufficient_rmid(struct event_group *e, struct pmt_feature_group *p)
{
	struct telemetry_region *tr;

	for (int i = 0; i < p->count; i++) {
		tr = &p->regions[i];
		if (skip_this_region(tr, e))
			continue;

		if (tr->num_rmids < e->num_rmids)
			return false;
	}

	return true;
}

static bool enable_events(struct event_group *e, struct pmt_feature_group *p)
{
	struct rdt_resource *r = &rdt_resources_all[RDT_RESOURCE_PERF_PKG].r_resctrl;
	bool usable_events = false;

	/* Disable feature if insufficient RMIDs */
	if (!all_regions_have_sufficient_rmid(e, p))
		rdt_set_feature_disabled(e->name);

	/* User can override above disable from kernel command line */
	if (!rdt_is_feature_enabled(e->name))
		return false;

	for (int i = 0; i < p->count; i++) {
		if (skip_this_region(&p->regions[i], e)) {
			/*
			 * Clear addr so that intel_aet_read_event() will
			 * skip this region.
			 */
			p->regions[i].addr = NULL;
			continue;
		}

		/*
		 * e->num_rmids only adjusted lower if user forces an unusable
		 * region to be usable
		 */
		e->num_rmids = min(e->num_rmids, p->regions[i].num_rmids);
		usable_events = true;
	}

	if (!usable_events)
		return false;

	if (r->num_rmid)
		r->num_rmid = min(r->num_rmid, e->num_rmids);
	else
		r->num_rmid = e->num_rmids;

	for (int j = 0; j < e->num_events; j++)
		resctrl_enable_mon_event(e->evts[j].id, true,
					 e->evts[j].bin_bits, &e->evts[j]);

	return true;
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

#define DATA_VALID	BIT_ULL(63)
#define DATA_BITS	GENMASK_ULL(62, 0)

/*
 * Read counter for an event on a domain (summing all aggregators
 * on the domain). If an aggregator hasn't received any data for a
 * specific RMID, the MMIO read indicates that data is not valid.
 * Return success if at least one aggregator has valid data.
 */
int intel_aet_read_event(int domid, int rmid, enum resctrl_event_id eventid,
			 void *arch_priv, u64 *val)
{
	struct pmt_event *pevt = arch_priv;
	struct event_group *e;
	bool valid = false;
	u64 evtcount;
	void *pevt0;
	int idx;

	pevt0 = pevt - pevt->idx;
	e = container_of(pevt0, struct event_group, evts);
	idx = rmid * e->num_events;
	idx += pevt->idx;

	if (idx * sizeof(u64) + sizeof(u64) > e->mmio_size) {
		pr_warn_once("MMIO index %d out of range\n", idx);
		return -EIO;
	}

	for (int i = 0; i < e->pfg->count; i++) {
		if (!e->pfg->regions[i].addr)
			continue;
		if (e->pfg->regions[i].plat_info.package_id != domid)
			continue;
		evtcount = readq(e->pfg->regions[i].addr + idx * sizeof(u64));
		if (!(evtcount & DATA_VALID))
			continue;
		*val += evtcount & DATA_BITS;
		valid = true;
	}

	return valid ? 0 : -EINVAL;
}

void intel_aet_setup_mon_domain(int cpu, int id, struct rdt_resource *r,
				struct list_head *add_pos)
{
	struct rdt_perf_pkg_mon_domain *d;
	int err;

	d = kzalloc_node(sizeof(*d), GFP_KERNEL, cpu_to_node(cpu));
	if (!d)
		return;

	d->hdr.id = id;
	d->hdr.type = RESCTRL_MON_DOMAIN;
	d->hdr.rid = r->rid;
	cpumask_set_cpu(cpu, &d->hdr.cpu_mask);
	list_add_tail_rcu(&d->hdr.list, add_pos);

	err = resctrl_online_mon_domain(r, &d->hdr);
	if (err) {
		list_del_rcu(&d->hdr.list);
		synchronize_rcu();
		kfree(d);
	}
}
