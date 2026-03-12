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
		if (msr_param.res)
			smp_call_function_any(&d->hdr.cpu_mask, rdt_ctrl_update, &msr_param, 1);
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

/*
 * IPI callback: write MSR_IA32_PQR_PLZA_ASSOC on this CPU (AMD PLZA).
 */
static void resctrl_kmode_set_one_amd(void *arg)
{
	union qos_pqr_plza_assoc *plza = arg;

	wrmsrl(MSR_IA32_PQR_PLZA_ASSOC, plza->full);
}

/**
 * resctrl_arch_configure_kmode() - x86/AMD: program PLZA per control domain
 *
 * For each control domain, first sets per-CPU state (closid, rmid, enable=0)
 * on all CPUs in the domain, then writes MSR_IA32_PQR_PLZA_ASSOC on each CPU
 * so that closid/closid_en (and optionally rmid/rmid_en) are programmed
 * before PLZA_EN is set, per the PLZA programming sequence.
 */
void resctrl_arch_configure_kmode(struct rdt_resource *r, struct resctrl_kmode_cfg *kcfg,
				  u32 closid, u32 rmid)
{
	union qos_pqr_plza_assoc plza = { 0 };
	struct rdt_ctrl_domain *d;
	int cpu;

	if (kcfg->kmode_cur & INHERIT_CTRL_AND_MON)
		return;

	if (kcfg->kmode_cur & GLOBAL_ASSIGN_CTRL_ASSIGN_MON) {
		plza.split.rmid = rmid;
		plza.split.rmid_en = 1;
	}
	plza.split.closid = closid;
	plza.split.closid_en = 1;

	list_for_each_entry(d, &r->ctrl_domains, hdr.list) {
		for_each_cpu(cpu, &d->hdr.cpu_mask)
			resctrl_arch_set_cpu_kmode(cpu, closid, rmid, 0);
		on_each_cpu_mask(&d->hdr.cpu_mask, resctrl_kmode_set_one_amd, &plza, 1);
	}
}

/**
 * resctrl_arch_set_kmode() - x86/AMD: set PLZA enable/disable on a set of CPUs
 * @cpu_mask:	CPUs to update (e.g. a control domain's cpu_mask).
 * @kcfg:	Current kernel mode configuration.
 * @closid:	CLOSID to use for kernel work when a global assign mode is active.
 * @rmid:	RMID to use for kernel work when GLOBAL_ASSIGN_CTRL_ASSIGN_MON is active.
 * @enable:	True to set MSR_IA32_PQR_PLZA_ASSOC.PLZA_EN; false to clear it.
 *
 * Writes MSR_IA32_PQR_PLZA_ASSOC on each CPU in @cpu_mask (via IPI) and updates
 * per-CPU state. No-op when kmode_cur is INHERIT_CTRL_AND_MON. Call after
 * resctrl_arch_configure_kmode() so that closid/rmid are programmed before
 * PLZA_EN is set.
 */
void resctrl_arch_set_kmode(cpumask_var_t cpu_mask, struct resctrl_kmode_cfg *kcfg,
			    u32 closid, u32 rmid, bool enable)
{
	int cpu;
	union qos_pqr_plza_assoc plza = { 0 };

	if (kcfg->kmode_cur & INHERIT_CTRL_AND_MON)
		return;

	if (kcfg->kmode_cur & GLOBAL_ASSIGN_CTRL_ASSIGN_MON) {
		plza.split.rmid = rmid;
		plza.split.rmid_en = 1;
	}
	plza.split.closid = closid;
	plza.split.closid_en = 1;
	plza.split.plza_en = enable;

	on_each_cpu_mask(cpu_mask, resctrl_kmode_set_one_amd, &plza, 1);
	for_each_cpu(cpu, cpu_mask)
		resctrl_arch_set_cpu_kmode(cpu, closid, rmid, enable);
}
