// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Chengdu Haiguang IC Design Co., Ltd.
 */

#include <linux/cpu.h>
#include <linux/cpufeature.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/percpu.h>
#include <linux/perf_event.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/types.h>

#include <asm/amd/nb.h>
#include <asm/cpuid/api.h>
#include <asm/msr.h>
#include <asm/perf_event.h>

#include "uncore_common.h"

#define NUM_COUNTERS_DF		4

#undef pr_fmt
#define pr_fmt(fmt)	"hygon_uncore: " fmt

enum {
	HYGON_UNCORE_TYPE_DF,
	HYGON_UNCORE_TYPE_DF_IOD,
	HYGON_UNCORE_TYPE_MAX,
};

/* Interval for hrtimer, defaults to 60000 milliseconds */
static unsigned int update_interval = 60 * MSEC_PER_SEC;
module_param(update_interval, uint, 0444);

static struct uncore_common hygon_uncores[HYGON_UNCORE_TYPE_MAX];

static __always_inline bool hygon_uncore_is_df_iod(struct uncore_common_pmu *pmu)
{
	struct uncore_common *uncore = pmu->private;

	return uncore == &hygon_uncores[HYGON_UNCORE_TYPE_DF_IOD];
}

static __always_inline int hygon_uncore_num_iods(struct uncore_common *uncore, unsigned int cpu)
{
	union uncore_common_info *info = per_cpu_ptr(uncore->info, cpu);

	return info->split.private;
}

static u64 hygon_uncore_df_event_mask(void)
{
	if (boot_cpu_data.x86_model == 0x4 ||
	    boot_cpu_data.x86_model == 0x5)
		return HYGON_F18H_M4H_RAW_EVENT_MASK_DF;

	if (boot_cpu_data.x86_model >= 0x6 &&
	    boot_cpu_data.x86_model <= 0x18)
		return HYGON_F18H_M6H_RAW_EVENT_MASK_DF;

	return HYGON_F18H_RAW_EVENT_MASK_DF;
}

DEFINE_UNCORE_FORMAT_ATTR(event,	event,		"config:0-5");
DEFINE_UNCORE_FORMAT_ATTR(umask8,	umask,		"config:8-15");
DEFINE_UNCORE_FORMAT_ATTR(umask10,	umask,		"config:8-17");
DEFINE_UNCORE_FORMAT_ATTR(umask12,	umask,		"config:8-19");
DEFINE_UNCORE_FORMAT_ATTR(constid,	constid,	"config:6-7,32-35,61-62");
DEFINE_UNCORE_FORMAT_ATTR(iod,		iod,		"config1:0-1");

static struct attribute *hygon_uncore_df_format_attr[] = {
	&format_attr_event.attr,
	&format_attr_umask8.attr,
	&format_attr_constid.attr,
	NULL,
};

static struct attribute *hygon_uncore_df_iod_format_attr[] = {
	&format_attr_event.attr,
	&format_attr_umask10.attr,
	&format_attr_constid.attr,
	&format_attr_iod.attr,
	NULL,
};

static struct attribute_group hygon_uncore_df_format_group = {
	.name = "format",
	.attrs = hygon_uncore_df_format_attr,
};

static struct attribute_group hygon_uncore_df_iod_format_group = {
	.name = "format",
	.attrs = hygon_uncore_df_iod_format_attr,
};

static const struct attribute_group *hygon_uncore_df_attr_groups[] = {
	&uncore_common_attr_group,
	&hygon_uncore_df_format_group,
	NULL,
};

static const struct attribute_group *hygon_uncore_df_iod_attr_groups[] = {
	&uncore_common_attr_group,
	&hygon_uncore_df_iod_format_group,
	NULL,
};

static int hygon_uncore_df_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	struct uncore_common_pmu *pmu;
	struct uncore_common *uncore;
	u64 event_mask;
	int ret;

	ret = uncore_common_event_init(event);
	if (ret)
		return ret;

	pmu = event_to_uncore_common_pmu(event);
	uncore = pmu->private;

	if (hygon_uncore_is_df_iod(pmu) &&
	    event->attr.config1 >= hygon_uncore_num_iods(uncore, event->cpu))
		return -EINVAL;

	event_mask = hygon_uncore_df_event_mask();
	hwc->config = event->attr.config & event_mask;

	return 0;
}

static int hygon_uncore_df_iod_add(struct perf_event *event, int flags)
{
	struct uncore_common_pmu *pmu = event_to_uncore_common_pmu(event);
	struct uncore_common_ctx *ctx = *per_cpu_ptr(pmu->ctx, event->cpu);
	struct hw_perf_event *hwc = &event->hw;
	int iod_idx;
	int i;

	if (hwc->idx != -1 && ctx->events[hwc->idx] == event)
		goto out;

	for (i = 0; i < pmu->num_counters; i++) {
		if (ctx->events[i] == event) {
			hwc->idx = i;
			goto out;
		}
	}

	hwc->idx = -1;
	iod_idx = event->attr.config1;

	if (iod_idx >= pmu->num_counters / NUM_COUNTERS_DF)
		return -EINVAL;

	for (i = iod_idx * NUM_COUNTERS_DF; i < (iod_idx + 1) * NUM_COUNTERS_DF; i++) {
		struct perf_event *tmp = NULL;

		if (try_cmpxchg(&ctx->events[i], &tmp, event)) {
			hwc->idx = i;
			break;
		}
	}

out:
	if (hwc->idx == -1)
		return -EBUSY;

	hwc->config_base = pmu->msr_base + 2 * hwc->idx;
	hwc->event_base = pmu->msr_base + 1 + 2 * hwc->idx;
	hwc->event_base_rdpmc = -1;
	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;

	if (flags & PERF_EF_START)
		event->pmu->start(event, PERF_EF_RELOAD);

	return 0;
}

static int hygon_uncore_cpu_starting(unsigned int cpu)
{
	struct uncore_common *uncore;
	int i;

	for (i = 0; i < HYGON_UNCORE_TYPE_MAX; i++) {
		uncore = &hygon_uncores[i];
		uncore->scan(uncore, cpu);
	}

	return 0;
}

static int hygon_uncore_cpu_online(unsigned int cpu)
{
	struct uncore_common *uncore;
	int ret;
	int i;

	for (i = 0; i < HYGON_UNCORE_TYPE_MAX; i++) {
		uncore = &hygon_uncores[i];

		ret = uncore->init(uncore, cpu);
		if (ret)
			return ret;
	}

	return 0;
}

static int hygon_uncore_cpu_down_prepare(unsigned int cpu)
{
	struct uncore_common *uncore;
	int i;

	for (i = 0; i < HYGON_UNCORE_TYPE_MAX; i++) {
		uncore = &hygon_uncores[i];
		uncore->move(uncore, cpu);
	}

	return 0;
}

static int hygon_uncore_cpu_dead(unsigned int cpu)
{
	struct uncore_common *uncore;
	int i;

	for (i = 0; i < HYGON_UNCORE_TYPE_MAX; i++) {
		uncore = &hygon_uncores[i];
		uncore->free(uncore, cpu);
	}

	return 0;
}

static int hygon_uncore_df_ctx_init(struct uncore_common *uncore,
				    unsigned int cpu)
{
	struct attribute *df_attr;
	struct uncore_common_pmu *pmu;
	int num_counters;

	if (uncore->init_done)
		return uncore_common_ctx_init(uncore, cpu);

	num_counters = uncore_common_ctx_num_pmcs(uncore, cpu);
	if (!num_counters)
		goto done;

	uncore->pmus = kzalloc_obj(*uncore->pmus);
	if (!uncore->pmus)
		goto done;

	pmu = &uncore->pmus[0];
	strscpy(pmu->name, "hygon_df", sizeof(pmu->name));
	pmu->num_counters = num_counters;
	pmu->msr_base = MSR_HYGON_F18H_DF_CTL;
	pmu->rdpmc_base = -1;
	pmu->group = uncore_common_ctx_gid(uncore, cpu);
	pmu->private = uncore;

	df_attr = &format_attr_umask8.attr;
	if (boot_cpu_data.x86_model == 0x4 ||
	    boot_cpu_data.x86_model == 0x5)
		df_attr = &format_attr_umask10.attr;
	else if (boot_cpu_data.x86_model >= 0x6 &&
		 boot_cpu_data.x86_model <= 0x18)
		df_attr = &format_attr_umask12.attr;
	hygon_uncore_df_format_attr[1] = df_attr;

	pmu->ctx = alloc_percpu(struct uncore_common_ctx *);
	if (!pmu->ctx)
		goto done;

	pmu->pmu = (struct pmu) {
		.task_ctx_nr	= perf_invalid_context,
		.attr_groups	= hygon_uncore_df_attr_groups,
		.name		= pmu->name,
		.event_init	= hygon_uncore_df_event_init,
		.add		= uncore_common_add,
		.del		= uncore_common_del,
		.start		= uncore_common_start,
		.stop		= uncore_common_stop,
		.read		= uncore_common_read,
		.capabilities	= PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_NO_INTERRUPT,
		.module		= THIS_MODULE,
	};

	if (perf_pmu_register(&pmu->pmu, pmu->pmu.name, -1)) {
		free_percpu(pmu->ctx);
		pmu->ctx = NULL;
		goto done;
	}

	pr_info("%d %s counters detected\n", pmu->num_counters, pmu->pmu.name);
	uncore->num_pmus = 1;

done:
	uncore->init_done = true;
	return uncore_common_ctx_init(uncore, cpu);
}

static void hygon_uncore_df_ctx_scan(struct uncore_common *uncore,
				     unsigned int cpu)
{
	unsigned int eax, ebx, ecx, edx;
	union uncore_common_info info = {};

	if (!boot_cpu_has(X86_FEATURE_PERFCTR_NB))
		return;

	info.split.gid = 0;
	info.split.aux_data = 0;
	info.split.num_pmcs = NUM_COUNTERS_DF;

	cpuid(0x8000001e, &eax, &ebx, &ecx, &edx);
	info.split.cid = ecx & 0xff;

	*per_cpu_ptr(uncore->info, cpu) = info;
}

static void hygon_uncore_df_iod_ctx_scan(struct uncore_common *uncore,
					 unsigned int cpu)
{
	int num_packages, iods_per_package;
	union uncore_common_info info = {};

	if (!boot_cpu_has(X86_FEATURE_PERFCTR_NB))
		return;

	if (boot_cpu_data.x86_model < 0x4 || boot_cpu_data.x86_model == 0x6)
		return;

	num_packages = topology_max_packages();
	iods_per_package = amd_nb_num() / num_packages - topology_max_dies_per_package();
	/* Hardware does not support more than 2 IODs per package. */
	if (iods_per_package <= 0 || iods_per_package > 2)
		return;

	info.split.cid = topology_physical_package_id(cpu);
	info.split.gid = 0;
	info.split.private = iods_per_package;
	info.split.num_pmcs = NUM_COUNTERS_DF * iods_per_package;

	*per_cpu_ptr(uncore->info, cpu) = info;
}

static int hygon_uncore_df_iod_ctx_init(struct uncore_common *uncore,
					unsigned int cpu)
{
	struct uncore_common_pmu *pmu;
	int num_counters;

	if (uncore->init_done)
		return uncore_common_ctx_init(uncore, cpu);

	num_counters = uncore_common_ctx_num_pmcs(uncore, cpu);
	if (!num_counters)
		goto done;

	uncore->pmus = kzalloc_obj(*uncore->pmus);
	if (!uncore->pmus)
		goto done;

	pmu = &uncore->pmus[0];
	strscpy(pmu->name, "hygon_df_iod", sizeof(pmu->name));
	pmu->num_counters = num_counters;
	pmu->msr_base = MSR_HYGON_F18H_DF_IOD_CTL;
	pmu->rdpmc_base = -1;
	pmu->group = uncore_common_ctx_gid(uncore, cpu);
	pmu->private = uncore;

	if (boot_cpu_data.x86_model >= 0x6 &&
	    boot_cpu_data.x86_model <= 0x18)
		hygon_uncore_df_iod_format_attr[1] = &format_attr_umask12.attr;

	pmu->ctx = alloc_percpu(struct uncore_common_ctx *);
	if (!pmu->ctx)
		goto done;

	pmu->pmu = (struct pmu) {
		.task_ctx_nr	= perf_invalid_context,
		.attr_groups	= hygon_uncore_df_iod_attr_groups,
		.name		= pmu->name,
		.event_init	= hygon_uncore_df_event_init,
		.add		= hygon_uncore_df_iod_add,
		.del		= uncore_common_del,
		.start		= uncore_common_start,
		.stop		= uncore_common_stop,
		.read		= uncore_common_read,
		.capabilities	= PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_NO_INTERRUPT,
		.module		= THIS_MODULE,
	};

	if (perf_pmu_register(&pmu->pmu, pmu->pmu.name, -1)) {
		free_percpu(pmu->ctx);
		pmu->ctx = NULL;
		goto done;
	}

	pr_info("%d %s counters detected\n", pmu->num_counters, pmu->pmu.name);
	uncore->num_pmus = 1;

done:
	uncore->init_done = true;
	return uncore_common_ctx_init(uncore, cpu);
}

static struct uncore_common hygon_uncores[HYGON_UNCORE_TYPE_MAX] = {
	/* HYGON_UNCORE_TYPE_DF */
	{
		.scan = hygon_uncore_df_ctx_scan,
		.init = hygon_uncore_df_ctx_init,
		.move = uncore_common_ctx_move,
		.free = uncore_common_ctx_free,
	},
	/* HYGON_UNCORE_TYPE_DF IOD */
	{
		.scan = hygon_uncore_df_iod_ctx_scan,
		.init = hygon_uncore_df_iod_ctx_init,
		.move = uncore_common_ctx_move,
		.free = uncore_common_ctx_free,
	},
};

static void hygon_uncore_pmus_unregister(void)
{
	struct uncore_common *uncore;
	struct uncore_common_pmu *pmu;
	int i, j;

	for (i = 0; i < HYGON_UNCORE_TYPE_MAX; i++) {
		uncore = &hygon_uncores[i];

		for (j = 0; j < uncore->num_pmus; j++) {
			pmu = &uncore->pmus[j];
			if (!pmu->ctx)
				continue;

			perf_pmu_unregister(&pmu->pmu);
			free_percpu(pmu->ctx);
			pmu->ctx = NULL;
		}

		kfree(uncore->pmus);
		uncore->pmus = NULL;
		uncore->num_pmus = 0;
		uncore->init_done = false;
	}
}

static void hygon_uncore_infos_free(void)
{
	struct uncore_common *uncore;
	int i;

	for (i = 0; i < HYGON_UNCORE_TYPE_MAX; i++) {
		uncore = &hygon_uncores[i];
		if (!uncore->info)
			continue;

		free_percpu(uncore->info);
		uncore->info = NULL;
	}
}

static int __init hygon_uncore_init(void)
{
	struct uncore_common *uncore;
	int ret = -ENODEV;
	int i;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_HYGON)
		return -ENODEV;

	if (!boot_cpu_has(X86_FEATURE_TOPOEXT))
		return -ENODEV;

	uncore_common_set_update_interval(update_interval);

	for (i = 0; i < HYGON_UNCORE_TYPE_MAX; i++) {
		uncore = &hygon_uncores[i];

		if (WARN_ON_ONCE(!uncore->scan ||
				 !uncore->init ||
				 !uncore->move ||
				 !uncore->free)) {
			ret = -EINVAL;
			goto fail;
		}

		uncore->info = alloc_percpu(union uncore_common_info);
		if (!uncore->info) {
			ret = -ENOMEM;
			goto fail;
		}
	}

	ret = cpuhp_setup_state(CPUHP_PERF_X86_HYGON_UNCORE_PREP,
				"perf/x86/hygon/uncore:prepare",
				NULL, hygon_uncore_cpu_dead);
	if (ret)
		goto fail;

	ret = cpuhp_setup_state(CPUHP_AP_PERF_X86_HYGON_UNCORE_STARTING,
				"perf/x86/hygon/uncore:starting",
				hygon_uncore_cpu_starting, NULL);
	if (ret)
		goto fail_prep;

	ret = cpuhp_setup_state(CPUHP_AP_PERF_X86_HYGON_UNCORE_ONLINE,
				"perf/x86/hygon/uncore:online",
				hygon_uncore_cpu_online,
				hygon_uncore_cpu_down_prepare);
	if (ret)
		goto fail_start;

	return 0;

fail_start:
	cpuhp_remove_state(CPUHP_AP_PERF_X86_HYGON_UNCORE_STARTING);
	cpuhp_remove_state(CPUHP_PERF_X86_HYGON_UNCORE_PREP);
	hygon_uncore_pmus_unregister();
	goto fail;
fail_prep:
	cpuhp_remove_state(CPUHP_PERF_X86_HYGON_UNCORE_PREP);
fail:
	hygon_uncore_infos_free();

	return ret;
}

static void __exit hygon_uncore_exit(void)
{
	cpuhp_remove_state(CPUHP_AP_PERF_X86_HYGON_UNCORE_ONLINE);
	cpuhp_remove_state(CPUHP_AP_PERF_X86_HYGON_UNCORE_STARTING);
	cpuhp_remove_state(CPUHP_PERF_X86_HYGON_UNCORE_PREP);

	hygon_uncore_pmus_unregister();
	hygon_uncore_infos_free();
}

module_init(hygon_uncore_init);
module_exit(hygon_uncore_exit);

MODULE_DESCRIPTION("Hygon Uncore Driver");
MODULE_LICENSE("GPL");
