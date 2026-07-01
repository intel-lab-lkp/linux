// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Advanced Micro Devices, Inc.
 *
 * Author: Jacob Shin <jacob.shin@amd.com>
 */

#include <linux/perf_event.h>
#include <linux/percpu.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufeature.h>
#include <linux/smp.h>

#include <asm/perf_event.h>
#include <asm/cpuid/api.h>
#include <asm/msr.h>

#include "uncore_common.h"

#define NUM_COUNTERS_NB		4
#define NUM_COUNTERS_L2		4
#define NUM_COUNTERS_L3		6
#define NUM_COUNTERS_MAX	64

#define RDPMC_BASE_NB		6
#define RDPMC_BASE_LLC		10

#define COUNTER_SHIFT		16
#define UNCORE_NAME_LEN		16
#define UNCORE_GROUP_MAX	256

#undef pr_fmt
#define pr_fmt(fmt)	"amd_uncore: " fmt

static int pmu_version;

enum {
	UNCORE_TYPE_DF,
	UNCORE_TYPE_L3,
	UNCORE_TYPE_UMC,

	UNCORE_TYPE_MAX
};

static struct uncore_common uncores[UNCORE_TYPE_MAX];

/* Interval for hrtimer, defaults to 60000 milliseconds */
static unsigned int update_interval = 60 * MSEC_PER_SEC;
module_param(update_interval, uint, 0444);

static umode_t
amd_f17h_uncore_is_visible(struct kobject *kobj, struct attribute *attr, int i)
{
	return boot_cpu_data.x86 >= 0x17 && boot_cpu_data.x86 < 0x19 ?
	       attr->mode : 0;
}

static umode_t
amd_f19h_uncore_is_visible(struct kobject *kobj, struct attribute *attr, int i)
{
	return boot_cpu_data.x86 >= 0x19 ? attr->mode : 0;
}

DEFINE_UNCORE_FORMAT_ATTR(event12,	event,		"config:0-7,32-35");
DEFINE_UNCORE_FORMAT_ATTR(event14,	event,		"config:0-7,32-35,59-60"); /* F17h+ DF */
DEFINE_UNCORE_FORMAT_ATTR(event14v2,	event,		"config:0-7,32-37");	   /* PerfMonV2 DF */
DEFINE_UNCORE_FORMAT_ATTR(event8,	event,		"config:0-7");		   /* F17h+ L3, PerfMonV2 UMC */
DEFINE_UNCORE_FORMAT_ATTR(umask8,	umask,		"config:8-15");
DEFINE_UNCORE_FORMAT_ATTR(umask12,	umask,		"config:8-15,24-27");	   /* PerfMonV2 DF */
DEFINE_UNCORE_FORMAT_ATTR(coreid,	coreid,		"config:42-44");	   /* F19h L3 */
DEFINE_UNCORE_FORMAT_ATTR(slicemask,	slicemask,	"config:48-51");	   /* F17h L3 */
DEFINE_UNCORE_FORMAT_ATTR(threadmask8,	threadmask,	"config:56-63");	   /* F17h L3 */
DEFINE_UNCORE_FORMAT_ATTR(threadmask2,	threadmask,	"config:56-57");	   /* F19h L3 */
DEFINE_UNCORE_FORMAT_ATTR(enallslices,	enallslices,	"config:46");		   /* F19h L3 */
DEFINE_UNCORE_FORMAT_ATTR(enallcores,	enallcores,	"config:47");		   /* F19h L3 */
DEFINE_UNCORE_FORMAT_ATTR(sliceid,	sliceid,	"config:48-50");	   /* F19h L3 */
DEFINE_UNCORE_FORMAT_ATTR(rdwrmask,	rdwrmask,	"config:8-9");		   /* PerfMonV2 UMC */

/* Common DF and NB attributes */
static struct attribute *amd_uncore_df_format_attr[] = {
	&format_attr_event12.attr,	/* event */
	&format_attr_umask8.attr,	/* umask */
	NULL,
};

/* Common L2 and L3 attributes */
static struct attribute *amd_uncore_l3_format_attr[] = {
	&format_attr_event12.attr,	/* event */
	&format_attr_umask8.attr,	/* umask */
	NULL,				/* threadmask */
	NULL,
};

/* Common UMC attributes */
static struct attribute *amd_uncore_umc_format_attr[] = {
	&format_attr_event8.attr,       /* event */
	&format_attr_rdwrmask.attr,     /* rdwrmask */
	NULL,
};

/* F17h unique L3 attributes */
static struct attribute *amd_f17h_uncore_l3_format_attr[] = {
	&format_attr_slicemask.attr,	/* slicemask */
	NULL,
};

/* F19h unique L3 attributes */
static struct attribute *amd_f19h_uncore_l3_format_attr[] = {
	&format_attr_coreid.attr,	/* coreid */
	&format_attr_enallslices.attr,	/* enallslices */
	&format_attr_enallcores.attr,	/* enallcores */
	&format_attr_sliceid.attr,	/* sliceid */
	NULL,
};

static struct attribute_group amd_uncore_df_format_group = {
	.name = "format",
	.attrs = amd_uncore_df_format_attr,
};

static struct attribute_group amd_uncore_l3_format_group = {
	.name = "format",
	.attrs = amd_uncore_l3_format_attr,
};

static struct attribute_group amd_f17h_uncore_l3_format_group = {
	.name = "format",
	.attrs = amd_f17h_uncore_l3_format_attr,
	.is_visible = amd_f17h_uncore_is_visible,
};

static struct attribute_group amd_f19h_uncore_l3_format_group = {
	.name = "format",
	.attrs = amd_f19h_uncore_l3_format_attr,
	.is_visible = amd_f19h_uncore_is_visible,
};

static struct attribute_group amd_uncore_umc_format_group = {
	.name = "format",
	.attrs = amd_uncore_umc_format_attr,
};

static const struct attribute_group *amd_uncore_df_attr_groups[] = {
	&uncore_common_attr_group,
	&amd_uncore_df_format_group,
	NULL,
};

static const struct attribute_group *amd_uncore_l3_attr_groups[] = {
	&uncore_common_attr_group,
	&amd_uncore_l3_format_group,
	NULL,
};

static const struct attribute_group *amd_uncore_l3_attr_update[] = {
	&amd_f17h_uncore_l3_format_group,
	&amd_f19h_uncore_l3_format_group,
	NULL,
};

static const struct attribute_group *amd_uncore_umc_attr_groups[] = {
	&uncore_common_attr_group,
	&amd_uncore_umc_format_group,
	NULL,
};

static int amd_uncore_cpu_starting(unsigned int cpu)
{
	struct uncore_common *uncore;
	int i;

	for (i = 0; i < UNCORE_TYPE_MAX; i++) {
		uncore = &uncores[i];
		uncore->scan(uncore, cpu);
	}

	return 0;
}

static int amd_uncore_cpu_online(unsigned int cpu)
{
	struct uncore_common *uncore;
	int i;

	for (i = 0; i < UNCORE_TYPE_MAX; i++) {
		uncore = &uncores[i];
		if (uncore->init(uncore, cpu))
			break;
	}

	return 0;
}

static int amd_uncore_cpu_down_prepare(unsigned int cpu)
{
	struct uncore_common *uncore;
	int i;

	for (i = 0; i < UNCORE_TYPE_MAX; i++) {
		uncore = &uncores[i];
		uncore->move(uncore, cpu);
	}

	return 0;
}

static int amd_uncore_cpu_dead(unsigned int cpu)
{
	struct uncore_common *uncore;
	int i;

	for (i = 0; i < UNCORE_TYPE_MAX; i++) {
		uncore = &uncores[i];
		uncore->free(uncore, cpu);
	}

	return 0;
}

static int amd_uncore_df_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	int ret = uncore_common_event_init(event);

	hwc->config = event->attr.config &
		      (pmu_version >= 2 ? AMD64_PERFMON_V2_RAW_EVENT_MASK_NB :
					  AMD64_RAW_EVENT_MASK_NB);

	return ret;
}

static int amd_uncore_df_add(struct perf_event *event, int flags)
{
	int ret = uncore_common_add(event, flags & ~PERF_EF_START);
	struct hw_perf_event *hwc = &event->hw;

	if (ret)
		return ret;

	/*
	 * The first four DF counters are accessible via RDPMC index 6 to 9
	 * followed by the L3 counters from index 10 to 15. For processors
	 * with more than four DF counters, the DF RDPMC assignments become
	 * discontiguous as the additional counters are accessible starting
	 * from index 16.
	 */
	if (hwc->idx >= NUM_COUNTERS_NB)
		hwc->event_base_rdpmc += NUM_COUNTERS_L3;

	/* Delayed start after rdpmc base update */
	if (flags & PERF_EF_START)
		uncore_common_start(event, PERF_EF_RELOAD);

	return 0;
}

static
void amd_uncore_df_ctx_scan(struct uncore_common *uncore, unsigned int cpu)
{
	union cpuid_0x80000022_ebx ebx;
	union uncore_common_info info;

	if (!boot_cpu_has(X86_FEATURE_PERFCTR_NB))
		return;

	info.split.aux_data = 0;
	info.split.num_pmcs = NUM_COUNTERS_NB;
	info.split.gid = 0;
	info.split.cid = topology_amd_node_id(cpu);

	if (pmu_version >= 2) {
		ebx.full = cpuid_ebx(EXT_PERFMON_DEBUG_FEATURES);
		info.split.num_pmcs = ebx.split.num_df_pmc;
	}

	*per_cpu_ptr(uncore->info, cpu) = info;
}

static
int amd_uncore_df_ctx_init(struct uncore_common *uncore, unsigned int cpu)
{
	struct attribute **df_attr = amd_uncore_df_format_attr;
	struct uncore_common_pmu *pmu;
	int num_counters;

	/* Run just once */
	if (uncore->init_done)
		return uncore_common_ctx_init(uncore, cpu);

	num_counters = uncore_common_ctx_num_pmcs(uncore, cpu);
	if (!num_counters)
		goto done;

	/* No grouping, single instance for a system */
	uncore->pmus = kzalloc_obj(*uncore->pmus);
	if (!uncore->pmus)
		goto done;

	/*
	 * For Family 17h and above, the Northbridge counters are repurposed
	 * as Data Fabric counters. The PMUs are exported based on family as
	 * either NB or DF.
	 */
	pmu = &uncore->pmus[0];
	strscpy(pmu->name, boot_cpu_data.x86 >= 0x17 ? "amd_df" : "amd_nb",
		sizeof(pmu->name));
	pmu->num_counters = num_counters;
	pmu->msr_base = MSR_F15H_NB_PERF_CTL;
	pmu->rdpmc_base = RDPMC_BASE_NB;
	pmu->group = uncore_common_ctx_gid(uncore, cpu);

	if (pmu_version >= 2) {
		*df_attr++ = &format_attr_event14v2.attr;
		*df_attr++ = &format_attr_umask12.attr;
	} else if (boot_cpu_data.x86 >= 0x17) {
		*df_attr = &format_attr_event14.attr;
	}

	pmu->ctx = alloc_percpu(struct uncore_common_ctx *);
	if (!pmu->ctx)
		goto done;

	pmu->pmu = (struct pmu) {
		.task_ctx_nr	= perf_invalid_context,
		.attr_groups	= amd_uncore_df_attr_groups,
		.name		= pmu->name,
		.event_init	= amd_uncore_df_event_init,
		.add		= amd_uncore_df_add,
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

	pr_info("%d %s counters detected\n", pmu->num_counters,
		pmu->pmu.name);

	uncore->num_pmus = 1;

done:
	uncore->init_done = true;

	return uncore_common_ctx_init(uncore, cpu);
}

static int amd_uncore_l3_event_init(struct perf_event *event)
{
	int ret = uncore_common_event_init(event);
	struct hw_perf_event *hwc = &event->hw;
	u64 config = event->attr.config;
	u64 mask;

	hwc->config = config & AMD64_RAW_EVENT_MASK_NB;

	/*
	 * SliceMask and ThreadMask need to be set for certain L3 events.
	 * For other events, the two fields do not affect the count.
	 */
	if (ret || boot_cpu_data.x86 < 0x17)
		return ret;

	mask = config & (AMD64_L3_F19H_THREAD_MASK | AMD64_L3_SLICEID_MASK |
			 AMD64_L3_EN_ALL_CORES | AMD64_L3_EN_ALL_SLICES |
			 AMD64_L3_COREID_MASK);

	if (boot_cpu_data.x86 <= 0x18)
		mask = ((config & AMD64_L3_SLICE_MASK) ? : AMD64_L3_SLICE_MASK) |
		       ((config & AMD64_L3_THREAD_MASK) ? : AMD64_L3_THREAD_MASK);

	/*
	 * If the user doesn't specify a ThreadMask, they're not trying to
	 * count core 0, so we enable all cores & threads.
	 * We'll also assume that they want to count slice 0 if they specify
	 * a ThreadMask and leave SliceId and EnAllSlices unpopulated.
	 */
	else if (!(config & AMD64_L3_F19H_THREAD_MASK))
		mask = AMD64_L3_F19H_THREAD_MASK | AMD64_L3_EN_ALL_SLICES |
		       AMD64_L3_EN_ALL_CORES;

	hwc->config |= mask;

	return 0;
}

static
void amd_uncore_l3_ctx_scan(struct uncore_common *uncore, unsigned int cpu)
{
	union uncore_common_info info;

	if (!boot_cpu_has(X86_FEATURE_PERFCTR_LLC))
		return;

	info.split.aux_data = 0;
	info.split.num_pmcs = NUM_COUNTERS_L2;
	info.split.gid = 0;
	info.split.cid = per_cpu_llc_id(cpu);

	if (boot_cpu_data.x86 >= 0x17)
		info.split.num_pmcs = NUM_COUNTERS_L3;

	*per_cpu_ptr(uncore->info, cpu) = info;
}

static
int amd_uncore_l3_ctx_init(struct uncore_common *uncore, unsigned int cpu)
{
	struct attribute **l3_attr = amd_uncore_l3_format_attr;
	struct uncore_common_pmu *pmu;
	int num_counters;

	/* Run just once */
	if (uncore->init_done)
		return uncore_common_ctx_init(uncore, cpu);

	num_counters = uncore_common_ctx_num_pmcs(uncore, cpu);
	if (!num_counters)
		goto done;

	/* No grouping, single instance for a system */
	uncore->pmus = kzalloc_obj(*uncore->pmus);
	if (!uncore->pmus)
		goto done;

	/*
	 * For Family 17h and above, L3 cache counters are available instead
	 * of L2 cache counters. The PMUs are exported based on family as
	 * either L2 or L3.
	 */
	pmu = &uncore->pmus[0];
	strscpy(pmu->name, boot_cpu_data.x86 >= 0x17 ? "amd_l3" : "amd_l2",
		sizeof(pmu->name));
	pmu->num_counters = num_counters;
	pmu->msr_base = MSR_F16H_L2I_PERF_CTL;
	pmu->rdpmc_base = RDPMC_BASE_LLC;
	pmu->group = uncore_common_ctx_gid(uncore, cpu);

	if (boot_cpu_data.x86 >= 0x17) {
		*l3_attr++ = &format_attr_event8.attr;
		*l3_attr++ = &format_attr_umask8.attr;
		*l3_attr++ = boot_cpu_data.x86 >= 0x19 ?
			     &format_attr_threadmask2.attr :
			     &format_attr_threadmask8.attr;
	}

	pmu->ctx = alloc_percpu(struct uncore_common_ctx *);
	if (!pmu->ctx)
		goto done;

	pmu->pmu = (struct pmu) {
		.task_ctx_nr	= perf_invalid_context,
		.attr_groups	= amd_uncore_l3_attr_groups,
		.attr_update	= amd_uncore_l3_attr_update,
		.name		= pmu->name,
		.event_init	= amd_uncore_l3_event_init,
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

	pr_info("%d %s counters detected\n", pmu->num_counters,
		pmu->pmu.name);

	uncore->num_pmus = 1;

done:
	uncore->init_done = true;

	return uncore_common_ctx_init(uncore, cpu);
}

static int amd_uncore_umc_event_init(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;

	int ret = uncore_common_event_init(event);
	if (ret)
		return ret;

	hwc->config = event->attr.config & AMD64_PERFMON_V2_RAW_EVENT_MASK_UMC;

	return 0;
}

static void amd_uncore_umc_start(struct perf_event *event, int flags)
{
	struct uncore_common_pmu *pmu = event_to_uncore_common_pmu(event);
	struct uncore_common_ctx *ctx = *per_cpu_ptr(pmu->ctx, event->cpu);
	struct hw_perf_event *hwc = &event->hw;

	if (!ctx->nr_active++)
		uncore_common_start_hrtimer(ctx);

	if (flags & PERF_EF_RELOAD)
		wrmsrq(hwc->event_base, (u64)local64_read(&hwc->prev_count));

	hwc->state = 0;
	__set_bit(hwc->idx, ctx->active_mask);
	wrmsrq(hwc->config_base, (hwc->config | AMD64_PERFMON_V2_ENABLE_UMC));
	perf_event_update_userpage(event);
}

static void amd_uncore_umc_read(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 prev, new, shift;
	s64 delta;

	shift = COUNTER_SHIFT + 1;
	prev = local64_read(&hwc->prev_count);

	/*
	 * UMC counters do not have RDPMC assignments. Read counts directly
	 * from the corresponding PERF_CTR.
	 */
	rdmsrq(hwc->event_base, new);

	/*
	 * Unlike the other uncore counters, UMC counters saturate and set the
	 * Overflow bit (bit 48) on overflow. Since they do not roll over,
	 * proactively reset the corresponding PERF_CTR when bit 47 is set so
	 * that the counter never gets a chance to saturate.
	 */
	if (new & BIT_ULL(63 - COUNTER_SHIFT)) {
		wrmsrq(hwc->event_base, 0);
		local64_set(&hwc->prev_count, 0);
	} else {
		local64_set(&hwc->prev_count, new);
	}

	delta = (new << shift) - (prev << shift);
	delta >>= shift;
	local64_add(delta, &event->count);
}

static
void amd_uncore_umc_ctx_scan(struct uncore_common *uncore, unsigned int cpu)
{
	union cpuid_0x80000022_ebx ebx;
	union uncore_common_info info;
	unsigned int eax, ecx, edx;

	if (pmu_version < 2)
		return;

	cpuid(EXT_PERFMON_DEBUG_FEATURES, &eax, &ebx.full, &ecx, &edx);
	info.split.aux_data = ecx;	/* stash active mask */
	info.split.num_pmcs = ebx.split.num_umc_pmc;
	info.split.gid = topology_amd_node_id(cpu);
	info.split.cid = topology_amd_node_id(cpu);
	*per_cpu_ptr(uncore->info, cpu) = info;
}

static
int amd_uncore_umc_ctx_init(struct uncore_common *uncore, unsigned int cpu)
{
	DECLARE_BITMAP(gmask, UNCORE_GROUP_MAX) = { 0 };
	u8 group_num_pmus[UNCORE_GROUP_MAX] = { 0 };
	u8 group_num_pmcs[UNCORE_GROUP_MAX] = { 0 };
	union uncore_common_info info;
	struct uncore_common_pmu *pmu;
	int gid, i;
	u16 index = 0;

	if (pmu_version < 2)
		return 0;

	/* Run just once */
	if (uncore->init_done)
		return uncore_common_ctx_init(uncore, cpu);

	/* Find unique groups */
	for_each_online_cpu(i) {
		info = *per_cpu_ptr(uncore->info, i);
		gid = info.split.gid;

		if (test_bit(gid, gmask))
			continue;

		__set_bit(gid, gmask);
		group_num_pmus[gid] = hweight32(info.split.aux_data);
		group_num_pmcs[gid] = info.split.num_pmcs;
		uncore->num_pmus += group_num_pmus[gid];
	}

	uncore->pmus = kzalloc(sizeof(*uncore->pmus) * uncore->num_pmus,
			       GFP_KERNEL);
	if (!uncore->pmus) {
		uncore->num_pmus = 0;
		goto done;
	}

	for_each_set_bit(gid, gmask, UNCORE_GROUP_MAX) {
		for (i = 0; i < group_num_pmus[gid]; i++) {
			pmu = &uncore->pmus[index];
			snprintf(pmu->name, sizeof(pmu->name), "amd_umc_%hu", index);
			pmu->num_counters = group_num_pmcs[gid] / group_num_pmus[gid];
			pmu->msr_base = MSR_F19H_UMC_PERF_CTL + i * pmu->num_counters * 2;
			pmu->rdpmc_base = -1;
			pmu->group = gid;
			pmu->ctx = alloc_percpu(struct uncore_common_ctx *);
			if (!pmu->ctx)
				goto done;

			pmu->pmu = (struct pmu) {
				.task_ctx_nr	= perf_invalid_context,
				.attr_groups	= amd_uncore_umc_attr_groups,
				.name		= pmu->name,
				.event_init	= amd_uncore_umc_event_init,
				.add		= uncore_common_add,
				.del		= uncore_common_del,
				.start		= amd_uncore_umc_start,
				.stop		= uncore_common_stop,
				.read		= amd_uncore_umc_read,
				.capabilities	= PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_NO_INTERRUPT,
				.module		= THIS_MODULE,
			};

			if (perf_pmu_register(&pmu->pmu, pmu->pmu.name, -1)) {
				free_percpu(pmu->ctx);
				pmu->ctx = NULL;
				goto done;
			}

			pr_info("%d %s counters detected\n", pmu->num_counters,
				pmu->pmu.name);

			index++;
		}
	}

done:
	uncore->num_pmus = index;
	uncore->init_done = true;

	return uncore_common_ctx_init(uncore, cpu);
}

static struct uncore_common uncores[UNCORE_TYPE_MAX] = {
	/* UNCORE_TYPE_DF */
	{
		.scan = amd_uncore_df_ctx_scan,
		.init = amd_uncore_df_ctx_init,
		.move = uncore_common_ctx_move,
		.free = uncore_common_ctx_free,
	},
	/* UNCORE_TYPE_L3 */
	{
		.scan = amd_uncore_l3_ctx_scan,
		.init = amd_uncore_l3_ctx_init,
		.move = uncore_common_ctx_move,
		.free = uncore_common_ctx_free,
	},
	/* UNCORE_TYPE_UMC */
	{
		.scan = amd_uncore_umc_ctx_scan,
		.init = amd_uncore_umc_ctx_init,
		.move = uncore_common_ctx_move,
		.free = uncore_common_ctx_free,
	},
};

static int __init amd_uncore_init(void)
{
	struct uncore_common *uncore;
	int ret = -ENODEV;
	int i;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD)
		return -ENODEV;

	if (!boot_cpu_has(X86_FEATURE_TOPOEXT))
		return -ENODEV;

	if (boot_cpu_has(X86_FEATURE_PERFMON_V2))
		pmu_version = 2;

	uncore_common_set_update_interval(update_interval);

	for (i = 0; i < UNCORE_TYPE_MAX; i++) {
		uncore = &uncores[i];

		BUG_ON(!uncore->scan);
		BUG_ON(!uncore->init);
		BUG_ON(!uncore->move);
		BUG_ON(!uncore->free);

		uncore->info = alloc_percpu(union uncore_common_info);
		if (!uncore->info) {
			ret = -ENOMEM;
			goto fail;
		}
	};

	/*
	 * Install callbacks. Core will call them for each online cpu.
	 */
	ret = cpuhp_setup_state(CPUHP_PERF_X86_AMD_UNCORE_PREP,
				"perf/x86/amd/uncore:prepare",
				NULL, amd_uncore_cpu_dead);
	if (ret)
		goto fail;

	ret = cpuhp_setup_state(CPUHP_AP_PERF_X86_AMD_UNCORE_STARTING,
				"perf/x86/amd/uncore:starting",
				amd_uncore_cpu_starting, NULL);
	if (ret)
		goto fail_prep;

	ret = cpuhp_setup_state(CPUHP_AP_PERF_X86_AMD_UNCORE_ONLINE,
				"perf/x86/amd/uncore:online",
				amd_uncore_cpu_online,
				amd_uncore_cpu_down_prepare);
	if (ret)
		goto fail_start;

	return 0;

fail_start:
	cpuhp_remove_state(CPUHP_AP_PERF_X86_AMD_UNCORE_STARTING);
fail_prep:
	cpuhp_remove_state(CPUHP_PERF_X86_AMD_UNCORE_PREP);
fail:
	for (i = 0; i < UNCORE_TYPE_MAX; i++) {
		uncore = &uncores[i];
		if (uncore->info) {
			free_percpu(uncore->info);
			uncore->info = NULL;
		}
	}

	return ret;
}

static void __exit amd_uncore_exit(void)
{
	struct uncore_common *uncore;
	struct uncore_common_pmu *pmu;
	int i, j;

	cpuhp_remove_state(CPUHP_AP_PERF_X86_AMD_UNCORE_ONLINE);
	cpuhp_remove_state(CPUHP_AP_PERF_X86_AMD_UNCORE_STARTING);
	cpuhp_remove_state(CPUHP_PERF_X86_AMD_UNCORE_PREP);

	for (i = 0; i < UNCORE_TYPE_MAX; i++) {
		uncore = &uncores[i];
		if (!uncore->info)
			continue;

		free_percpu(uncore->info);
		uncore->info = NULL;

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
	}
}

module_init(amd_uncore_init);
module_exit(amd_uncore_exit);

MODULE_DESCRIPTION("AMD Uncore Driver");
MODULE_LICENSE("GPL v2");
