// SPDX-License-Identifier: GPL-2.0-only
/*
 * Resource Director Technology(RDT)
 * - Cache Allocation code.
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Authors:
 *    Fenghua Yu <fenghua.yu@intel.com>
 *    Tony Luck <tony.luck@intel.com>
 *
 * More information about RDT be found in the Intel (R) x86 Architecture
 * Software Developer Manual June 2016, volume 3, section 17.17.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#include <linux/cpu.h>

#include "internal.h"

int resctrl_arch_update_one(struct rdt_resource *r, struct rdt_ctrl_domain *d,
			    u32 closid, enum resctrl_conf_type t, u32 cfg_val)
{
	struct rdt_hw_ctrl_domain *hw_dom = resctrl_to_arch_ctrl_dom(d);
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);
	u32 idx = resctrl_get_config_index(closid, t);
	struct msr_param msr_param;

	if (!cpumask_test_cpu(smp_processor_id(), &d->hdr.cpu_mask))
		return -EINVAL;

	hw_dom->ctrl_val[idx] = cfg_val;

	msr_param.res = r;
	msr_param.dom = d;
	msr_param.low = idx;
	msr_param.high = idx + 1;
	hw_res->msr_update(&msr_param);

	return 0;
}

/**
 * resctrl_get_l3_mask() - One CPU per distinct L3 within a resctrl domain
 * @domain_mask: Full domain CPU mask (typically &d->hdr.cpu_mask).
 * @l3_mask:	 Output mask. Cleared on entry, then populated with exactly
 *		 one CPU per unique L3 cacheinfo id observed in @domain_mask.
 *		 Always a subset of @domain_mask; may end up empty if no CPU
 *		 in @domain_mask has a valid L3 id.
 *
 * For %RESCTRL_NPS_NODE controls (e.g. AMD GMBA) the control MSRs are
 * instantiated per L3 complex, so a single IPI per resctrl domain is not
 * sufficient. Callers are expected to run rdt_ctrl_update() on each CPU in
 * @l3_mask to cover every L3 that participates in the domain
 * (see resctrl_arch_update_nps()).
 *
 * Return: @l3_mask on success, %NULL if the scratch L3-id bitmap could not
 *	   be allocated (in which case @l3_mask is left cleared).
 */
static struct cpumask *resctrl_get_l3_mask(const struct cpumask *domain_mask,
					   struct cpumask *l3_mask)
{
	unsigned long *l3_dom_id;
	int cpu, id;

	cpumask_clear(l3_mask);
	l3_dom_id = bitmap_zalloc(nr_cpu_ids, GFP_KERNEL);
	if (!l3_dom_id)
		return NULL;

	for_each_cpu(cpu, domain_mask) {
		id = get_cpu_cacheinfo_id(cpu, RESCTRL_L3_CACHE);
		if (id < 0 || id >= nr_cpu_ids)
			continue;
		if (test_bit(id, l3_dom_id))
			continue;
		set_bit(id, l3_dom_id);
		cpumask_set_cpu(cpu, l3_mask);
	}

	bitmap_free(l3_dom_id);
	return l3_mask;
}

/**
 * resctrl_arch_update_nps() - Apply staged ctrl MSRs for NPS-scoped resources
 * @mp:	Parameters describing the MSR index range, resource and domain
 *	passed through to rdt_ctrl_update().
 * @d:	Control domain whose CPUs must see the MSR update.
 *
 * %RESCTRL_NPS_NODE resources program control MSRs per L3 complex, so one
 * IPI per resctrl domain is not enough when the domain spans multiple L3s.
 * Build a per-L3 representative mask with resctrl_get_l3_mask() and issue
 * rdt_ctrl_update() via smp_call_function_many() on that mask.
 *
 * If the temporary cpumask or the scratch L3-id bitmap cannot be allocated,
 * or the resulting per-L3 mask is empty, fall back to invoking
 * smp_call_function_many() on the full domain CPU mask. This is
 * conservative (more IPIs than strictly needed) but guarantees every L3 in
 * the domain is covered.
 */
void resctrl_arch_update_nps(struct msr_param *mp, struct rdt_ctrl_domain *d)
{
	const struct cpumask *mask = &d->hdr.cpu_mask;
	struct cpumask *new_mask;
	cpumask_var_t l3_mask;
	bool l3_alloc;

	l3_alloc = zalloc_cpumask_var(&l3_mask, GFP_KERNEL);
	if (l3_alloc) {
		new_mask = resctrl_get_l3_mask(&d->hdr.cpu_mask, l3_mask);

		if (new_mask && !cpumask_empty(new_mask))
			mask = new_mask;
	}

	smp_call_function_many(mask, rdt_ctrl_update, mp, 1);

	if (l3_alloc)
		free_cpumask_var(l3_mask);
}

int resctrl_arch_update_domains(struct rdt_resource *r, u32 closid)
{
	struct resctrl_staged_config *cfg;
	struct rdt_hw_ctrl_domain *hw_dom;
	struct msr_param msr_param;
	struct rdt_ctrl_domain *d;
	enum resctrl_conf_type t;
	u32 idx;

	/* Walking r->domains, ensure it can't race with cpuhp */
	lockdep_assert_cpus_held();

	list_for_each_entry(d, &r->ctrl_domains, hdr.list) {
		hw_dom = resctrl_to_arch_ctrl_dom(d);
		msr_param.res = NULL;
		for (t = 0; t < CDP_NUM_TYPES; t++) {
			cfg = &hw_dom->d_resctrl.staged_config[t];
			if (!cfg->have_new_ctrl)
				continue;

			idx = resctrl_get_config_index(closid, t);
			if (cfg->new_ctrl == hw_dom->ctrl_val[idx])
				continue;
			hw_dom->ctrl_val[idx] = cfg->new_ctrl;

			if (!msr_param.res) {
				msr_param.low = idx;
				msr_param.high = msr_param.low + 1;
				msr_param.res = r;
				msr_param.dom = d;
			} else {
				msr_param.low = min(msr_param.low, idx);
				msr_param.high = max(msr_param.high, idx + 1);
			}
		}

		if (msr_param.res) {
			if (msr_param.res->ctrl_scope == RESCTRL_NPS_NODE)
				resctrl_arch_update_nps(&msr_param, d);
			else
				smp_call_function_any(&d->hdr.cpu_mask,
						      rdt_ctrl_update, &msr_param, 1);
		}
	}

	return 0;
}

u32 resctrl_arch_get_config(struct rdt_resource *r, struct rdt_ctrl_domain *d,
			    u32 closid, enum resctrl_conf_type type)
{
	struct rdt_hw_ctrl_domain *hw_dom = resctrl_to_arch_ctrl_dom(d);
	u32 idx = resctrl_get_config_index(closid, type);

	return hw_dom->ctrl_val[idx];
}

bool resctrl_arch_get_io_alloc_enabled(struct rdt_resource *r)
{
	return resctrl_to_arch_res(r)->sdciae_enabled;
}

static void resctrl_sdciae_set_one_amd(void *arg)
{
	bool *enable = arg;

	if (*enable)
		msr_set_bit(MSR_IA32_L3_QOS_EXT_CFG, SDCIAE_ENABLE_BIT);
	else
		msr_clear_bit(MSR_IA32_L3_QOS_EXT_CFG, SDCIAE_ENABLE_BIT);
}

static void _resctrl_sdciae_enable(struct rdt_resource *r, bool enable)
{
	struct rdt_ctrl_domain *d;

	/* Walking r->ctrl_domains, ensure it can't race with cpuhp */
	lockdep_assert_cpus_held();

	/* Update MSR_IA32_L3_QOS_EXT_CFG MSR on all the CPUs in all domains */
	list_for_each_entry(d, &r->ctrl_domains, hdr.list)
		on_each_cpu_mask(&d->hdr.cpu_mask, resctrl_sdciae_set_one_amd, &enable, 1);
}

int resctrl_arch_io_alloc_enable(struct rdt_resource *r, bool enable)
{
	struct rdt_hw_resource *hw_res = resctrl_to_arch_res(r);

	if (hw_res->r_resctrl.cache.io_alloc_capable &&
	    hw_res->sdciae_enabled != enable) {
		_resctrl_sdciae_enable(r, enable);
		hw_res->sdciae_enabled = enable;
	}

	return 0;
}
