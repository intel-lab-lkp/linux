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
#include <asm/msr.h>

#define NUM_COUNTERS_NB		4
#define NUM_COUNTERS_L2		4
#define NUM_COUNTERS_L3		6

#define RDPMC_BASE_NB		6
#define RDPMC_BASE_LLC		10

#define COUNTER_SHIFT		16
#define RAPL_CNTR_WIDTH	32

#undef pr_fmt
#define pr_fmt(fmt)	"amd_uncore: " fmt

static int pmu_version;
static int num_counters_llc;
static int num_counters_nb;
static bool l3_mask;

static int rapl_hw_unit __read_mostly;

static HLIST_HEAD(uncore_unused_list);

struct amd_uncore {
	int id;
	int refcnt;
	int cpu;
	int num_counters;
	int rdpmc_base;
	u32 msr_base;
	cpumask_t *active_mask;
	struct pmu *pmu;
	struct perf_event **events;
	struct hlist_node node;
};

static struct amd_uncore * __percpu *amd_uncore_nb;
static struct amd_uncore * __percpu *amd_uncore_llc;
static struct amd_uncore * __percpu *amd_uncore_rapl;

static struct pmu amd_nb_pmu;
static struct pmu amd_llc_pmu;
static struct pmu amd_rapl_pmu;

static cpumask_t amd_nb_active_mask;
static cpumask_t amd_llc_active_mask;
static cpumask_t amd_rapl_active_mask;

static bool is_nb_event(struct perf_event *event)
{
	return event->pmu->type == amd_nb_pmu.type;
}

static bool is_llc_event(struct perf_event *event)
{
	return event->pmu->type == amd_llc_pmu.type;
}

static bool is_rapl_event(struct perf_event *event)
{
	return event->pmu->type == amd_rapl_pmu.type;
}

static struct amd_uncore *event_to_amd_uncore(struct perf_event *event)
{
	if (is_nb_event(event) && amd_uncore_nb)
		return *per_cpu_ptr(amd_uncore_nb, event->cpu);
	else if (is_llc_event(event) && amd_uncore_llc)
		return *per_cpu_ptr(amd_uncore_llc, event->cpu);
	else if (is_rapl_event(event) && amd_uncore_rapl)
		return *per_cpu_ptr(amd_uncore_rapl, event->cpu);

	return NULL;
}

static inline u64 rapl_scale(u64 v)
{
	return v << (32 - rapl_hw_unit);
}

static void amd_uncore_read(struct perf_event *event)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 prev, new;
	s64 delta;
	int shift;

	/*
	 * since we do not enable counter overflow interrupts,
	 * we do not have to worry about prev_count changing on us
	 */

	if (is_rapl_event(event)) {
		shift = RAPL_CNTR_WIDTH;
again:
		prev = local64_read(&hwc->prev_count);
		rdmsrl(hwc->event_base, new);
		if (local64_cmpxchg(&hwc->prev_count, prev, new) != prev) {
			cpu_relax();
			goto again;
		}
		delta = (new << shift) - (prev << shift);
		delta >>= shift;
		delta = rapl_scale(delta);
	} else {
		prev = local64_read(&hwc->prev_count);
		rdpmcl(hwc->event_base_rdpmc, new);
		local64_set(&hwc->prev_count, new);
		delta = (new << COUNTER_SHIFT) - (prev << COUNTER_SHIFT);
		delta >>= COUNTER_SHIFT;
	}
	local64_add(delta, &event->count);
}

static void amd_uncore_start(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;
	u64 new;

	if (flags & PERF_EF_RELOAD)
		wrmsrl(hwc->event_base, (u64)local64_read(&hwc->prev_count));

	if (is_rapl_event(event)) {
		rdmsrl(hwc->event_base, new);
		local64_set(&hwc->prev_count, new);
	}

	hwc->state = 0;
	if (hwc->config_base)
		wrmsrl(hwc->config_base, (hwc->config | ARCH_PERFMON_EVENTSEL_ENABLE));
	perf_event_update_userpage(event);
}

static void amd_uncore_stop(struct perf_event *event, int flags)
{
	struct hw_perf_event *hwc = &event->hw;

	if (hwc->config_base)
		wrmsrl(hwc->config_base, hwc->config);
	hwc->state |= PERF_HES_STOPPED;

	if ((flags & PERF_EF_UPDATE) && !(hwc->state & PERF_HES_UPTODATE)) {
		amd_uncore_read(event);
		hwc->state |= PERF_HES_UPTODATE;
	}
}

static int amd_uncore_add(struct perf_event *event, int flags)
{
	int i;
	struct amd_uncore *uncore = event_to_amd_uncore(event);
	struct hw_perf_event *hwc = &event->hw;

	/* are we already assigned? */
	if (hwc->idx != -1 && uncore->events[hwc->idx] == event)
		goto out;

	for (i = 0; i < uncore->num_counters; i++) {
		if (uncore->events[i] == event) {
			hwc->idx = i;
			goto out;
		}
	}

	/* if not, take the first available counter */
	hwc->idx = -1;
	for (i = 0; i < uncore->num_counters; i++) {
		if (cmpxchg(&uncore->events[i], NULL, event) == NULL) {
			hwc->idx = i;
			break;
		}
	}

out:
	if (hwc->idx == -1)
		return -EBUSY;

	if (is_rapl_event(event)) {
		hwc->config_base = 0;
		hwc->event_base = uncore->msr_base;
		hwc->event_base_rdpmc = 0;
	} else {
		hwc->config_base = uncore->msr_base + (2 * hwc->idx);
		hwc->event_base = uncore->msr_base + 1 + (2 * hwc->idx);
		hwc->event_base_rdpmc = uncore->rdpmc_base + hwc->idx;
	}
	hwc->state = PERF_HES_UPTODATE | PERF_HES_STOPPED;

	/*
	 * The first four DF counters are accessible via RDPMC index 6 to 9
	 * followed by the L3 counters from index 10 to 15. For processors
	 * with more than four DF counters, the DF RDPMC assignments become
	 * discontiguous as the additional counters are accessible starting
	 * from index 16.
	 */
	if (is_nb_event(event) && hwc->idx >= NUM_COUNTERS_NB)
		hwc->event_base_rdpmc += NUM_COUNTERS_L3;

	if (flags & PERF_EF_START)
		amd_uncore_start(event, PERF_EF_RELOAD);

	return 0;
}

static void amd_uncore_del(struct perf_event *event, int flags)
{
	int i;
	struct amd_uncore *uncore = event_to_amd_uncore(event);
	struct hw_perf_event *hwc = &event->hw;

	amd_uncore_stop(event, PERF_EF_UPDATE);

	for (i = 0; i < uncore->num_counters; i++) {
		if (cmpxchg(&uncore->events[i], event, NULL) == event)
			break;
	}

	hwc->idx = -1;
}

/*
 * Return a full thread and slice mask unless user
 * has provided them
 */
static u64 l3_thread_slice_mask(u64 config)
{
	if (boot_cpu_data.x86 <= 0x18)
		return ((config & AMD64_L3_SLICE_MASK) ? : AMD64_L3_SLICE_MASK) |
		       ((config & AMD64_L3_THREAD_MASK) ? : AMD64_L3_THREAD_MASK);

	/*
	 * If the user doesn't specify a threadmask, they're not trying to
	 * count core 0, so we enable all cores & threads.
	 * We'll also assume that they want to count slice 0 if they specify
	 * a threadmask and leave sliceid and enallslices unpopulated.
	 */
	if (!(config & AMD64_L3_F19H_THREAD_MASK))
		return AMD64_L3_F19H_THREAD_MASK | AMD64_L3_EN_ALL_SLICES |
		       AMD64_L3_EN_ALL_CORES;

	return config & (AMD64_L3_F19H_THREAD_MASK | AMD64_L3_SLICEID_MASK |
			 AMD64_L3_EN_ALL_CORES | AMD64_L3_EN_ALL_SLICES |
			 AMD64_L3_COREID_MASK);
}

static int amd_uncore_event_init(struct perf_event *event)
{
	struct amd_uncore *uncore;
	struct hw_perf_event *hwc = &event->hw;
	u64 event_mask = AMD64_RAW_EVENT_MASK_NB;

	if (event->attr.type != event->pmu->type)
		return -ENOENT;

	if (pmu_version >= 2 && is_nb_event(event))
		event_mask = AMD64_PERFMON_V2_RAW_EVENT_MASK_NB;

	/*
	 * NB and Last level cache counters (MSRs) are shared across all cores
	 * that share the same NB / Last level cache.  On family 16h and below,
	 * Interrupts can be directed to a single target core, however, event
	 * counts generated by processes running on other cores cannot be masked
	 * out. So we do not support sampling and per-thread events via
	 * CAP_NO_INTERRUPT, and we do not enable counter overflow interrupts:
	 */
	hwc->config = event->attr.config & event_mask;
	hwc->idx = -1;

	if (event->cpu < 0)
		return -EINVAL;

	/*
	 * SliceMask and ThreadMask need to be set for certain L3 events.
	 * For other events, the two fields do not affect the count.
	 */
	if (l3_mask && is_llc_event(event))
		hwc->config |= l3_thread_slice_mask(event->attr.config);

	uncore = event_to_amd_uncore(event);
	if (!uncore)
		return -ENODEV;

	/*
	 * since request can come in to any of the shared cores, we will remap
	 * to a single common cpu.
	 */
	event->cpu = uncore->cpu;

	return 0;
}

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

static ssize_t amd_uncore_attr_show_cpumask(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	cpumask_t *active_mask;
	struct pmu *pmu = dev_get_drvdata(dev);

	if (pmu->type == amd_nb_pmu.type)
		active_mask = &amd_nb_active_mask;
	else if (pmu->type == amd_llc_pmu.type)
		active_mask = &amd_llc_active_mask;
	else if (pmu->type == amd_rapl_pmu.type)
		active_mask = &amd_rapl_active_mask;
	else
		return 0;

	return cpumap_print_to_pagebuf(true, buf, active_mask);
}
static DEVICE_ATTR(cpumask, S_IRUGO, amd_uncore_attr_show_cpumask, NULL);

static struct attribute *amd_uncore_attrs[] = {
	&dev_attr_cpumask.attr,
	NULL,
};

static struct attribute_group amd_uncore_attr_group = {
	.attrs = amd_uncore_attrs,
};

#define DEFINE_UNCORE_FORMAT_ATTR(_var, _name, _format)			\
static ssize_t __uncore_##_var##_show(struct device *dev,		\
				struct device_attribute *attr,		\
				char *page)				\
{									\
	BUILD_BUG_ON(sizeof(_format) >= PAGE_SIZE);			\
	return sprintf(page, _format "\n");				\
}									\
static struct device_attribute format_attr_##_var =			\
	__ATTR(_name, 0444, __uncore_##_var##_show, NULL)

DEFINE_UNCORE_FORMAT_ATTR(event12,	event,		"config:0-7,32-35");
DEFINE_UNCORE_FORMAT_ATTR(event14,	event,		"config:0-7,32-35,59-60"); /* F17h+ DF */
DEFINE_UNCORE_FORMAT_ATTR(event14v2,	event,		"config:0-7,32-37");	   /* PerfMonV2 DF */
DEFINE_UNCORE_FORMAT_ATTR(event8,	event,		"config:0-7");		   /* F17h+ L3 */
DEFINE_UNCORE_FORMAT_ATTR(umask8,	umask,		"config:8-15");
DEFINE_UNCORE_FORMAT_ATTR(umask12,	umask,		"config:8-15,24-27");	   /* PerfMonV2 DF */
DEFINE_UNCORE_FORMAT_ATTR(coreid,	coreid,		"config:42-44");	   /* F19h L3 */
DEFINE_UNCORE_FORMAT_ATTR(slicemask,	slicemask,	"config:48-51");	   /* F17h L3 */
DEFINE_UNCORE_FORMAT_ATTR(threadmask8,	threadmask,	"config:56-63");	   /* F17h L3 */
DEFINE_UNCORE_FORMAT_ATTR(threadmask2,	threadmask,	"config:56-57");	   /* F19h L3 */
DEFINE_UNCORE_FORMAT_ATTR(enallslices,	enallslices,	"config:46");		   /* F19h L3 */
DEFINE_UNCORE_FORMAT_ATTR(enallcores,	enallcores,	"config:47");		   /* F19h L3 */
DEFINE_UNCORE_FORMAT_ATTR(sliceid,	sliceid,	"config:48-50");	   /* F19h L3 */

#define RAPL_EVENT_ATTR_STR(_name, v, str)                                      \
static struct perf_pmu_events_attr event_attr_##v = {                           \
	.attr           = __ATTR(_name, 0444, perf_event_sysfs_show, NULL),     \
	.id             = 0,                                                    \
	.event_str      = str,                                                  \
};

RAPL_EVENT_ATTR_STR(energy-cores, rapl_cores, "event=0x01");
RAPL_EVENT_ATTR_STR(energy-cores.unit, rapl_cores_unit, "Joules");
RAPL_EVENT_ATTR_STR(energy-cores.scale, rapl_cores_scale, "2.3283064365386962890625e-10");

/*
 * There are no default events, but we need to create
 * "events" group (with empty attrs) before updating
 * it with detected events.
 */
static struct attribute *attrs_empty[] = {
	NULL,
};

static struct attribute_group rapl_pmu_events_group = {
	.name = "events",
	.attrs = attrs_empty,
};

PMU_FORMAT_ATTR(event, "config:0-7");
static struct attribute *rapl_formats_attr[] = {
	&format_attr_event.attr,
	NULL,
};

static struct attribute_group rapl_pmu_format_group = {
	.name = "format",
	.attrs = rapl_formats_attr,
};

static const struct attribute_group *rapl_attr_groups[] = {
	&amd_uncore_attr_group,
	&rapl_pmu_format_group,
	&rapl_pmu_events_group,
	NULL,
};

static struct attribute *rapl_events_cores[] = {
	&event_attr_rapl_cores.attr.attr,
	&event_attr_rapl_cores_unit.attr.attr,
	&event_attr_rapl_cores_scale.attr.attr,
	NULL,
};

static struct attribute_group rapl_events_cores_group = {
	.name  = "events",
	.attrs = rapl_events_cores,
};


static const struct attribute_group *rapl_attr_update[] = {
	&rapl_events_cores_group,
	NULL,
};



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

static const struct attribute_group *amd_uncore_df_attr_groups[] = {
	&amd_uncore_attr_group,
	&amd_uncore_df_format_group,
	NULL,
};

static const struct attribute_group *amd_uncore_l3_attr_groups[] = {
	&amd_uncore_attr_group,
	&amd_uncore_l3_format_group,
	NULL,
};

static const struct attribute_group *amd_uncore_l3_attr_update[] = {
	&amd_f17h_uncore_l3_format_group,
	&amd_f19h_uncore_l3_format_group,
	NULL,
};

static struct pmu amd_nb_pmu = {
	.task_ctx_nr	= perf_invalid_context,
	.attr_groups	= amd_uncore_df_attr_groups,
	.name		= "amd_nb",
	.event_init	= amd_uncore_event_init,
	.add		= amd_uncore_add,
	.del		= amd_uncore_del,
	.start		= amd_uncore_start,
	.stop		= amd_uncore_stop,
	.read		= amd_uncore_read,
	.capabilities	= PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_NO_INTERRUPT,
	.module		= THIS_MODULE,
};

static struct pmu amd_llc_pmu = {
	.task_ctx_nr	= perf_invalid_context,
	.attr_groups	= amd_uncore_l3_attr_groups,
	.attr_update	= amd_uncore_l3_attr_update,
	.name		= "amd_l2",
	.event_init	= amd_uncore_event_init,
	.add		= amd_uncore_add,
	.del		= amd_uncore_del,
	.start		= amd_uncore_start,
	.stop		= amd_uncore_stop,
	.read		= amd_uncore_read,
	.capabilities	= PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_NO_INTERRUPT,
	.module		= THIS_MODULE,
};

static struct pmu amd_rapl_pmu = {
	.task_ctx_nr	= perf_invalid_context,
	.attr_groups	= rapl_attr_groups,
	.attr_update	= rapl_attr_update,
	.name		= "amd_rapl",
	.event_init	= amd_uncore_event_init,
	.add		= amd_uncore_add,
	.del		= amd_uncore_del,
	.start		= amd_uncore_start,
	.stop		= amd_uncore_stop,
	.read		= amd_uncore_read,
	.capabilities	= PERF_PMU_CAP_NO_EXCLUDE | PERF_PMU_CAP_NO_INTERRUPT,
	.module		= THIS_MODULE,
};

static struct amd_uncore *amd_uncore_alloc(unsigned int cpu)
{
	return kzalloc_node(sizeof(struct amd_uncore), GFP_KERNEL,
			cpu_to_node(cpu));
}

static inline struct perf_event **
amd_uncore_events_alloc(unsigned int num, unsigned int cpu)
{
	return kzalloc_node(sizeof(struct perf_event *) * num, GFP_KERNEL,
			    cpu_to_node(cpu));
}

static int amd_uncore_cpu_up_prepare(unsigned int cpu)
{
	struct amd_uncore *uncore_nb = NULL, *uncore_llc = NULL, *uncore_rapl = NULL;

	if (amd_uncore_nb) {
		*per_cpu_ptr(amd_uncore_nb, cpu) = NULL;
		uncore_nb = amd_uncore_alloc(cpu);
		if (!uncore_nb)
			goto fail;
		uncore_nb->cpu = cpu;
		uncore_nb->num_counters = num_counters_nb;
		uncore_nb->rdpmc_base = RDPMC_BASE_NB;
		uncore_nb->msr_base = MSR_F15H_NB_PERF_CTL;
		uncore_nb->active_mask = &amd_nb_active_mask;
		uncore_nb->pmu = &amd_nb_pmu;
		uncore_nb->events = amd_uncore_events_alloc(num_counters_nb, cpu);
		if (!uncore_nb->events)
			goto fail;
		uncore_nb->id = -1;
		*per_cpu_ptr(amd_uncore_nb, cpu) = uncore_nb;
	}

	if (amd_uncore_llc) {
		*per_cpu_ptr(amd_uncore_llc, cpu) = NULL;
		uncore_llc = amd_uncore_alloc(cpu);
		if (!uncore_llc)
			goto fail;
		uncore_llc->cpu = cpu;
		uncore_llc->num_counters = num_counters_llc;
		uncore_llc->rdpmc_base = RDPMC_BASE_LLC;
		uncore_llc->msr_base = MSR_F16H_L2I_PERF_CTL;
		uncore_llc->active_mask = &amd_llc_active_mask;
		uncore_llc->pmu = &amd_llc_pmu;
		uncore_llc->events = amd_uncore_events_alloc(num_counters_llc, cpu);
		if (!uncore_llc->events)
			goto fail;
		uncore_llc->id = -1;
		*per_cpu_ptr(amd_uncore_llc, cpu) = uncore_llc;
	}

	if (amd_uncore_rapl) {
		*per_cpu_ptr(amd_uncore_rapl, cpu) = NULL;
		uncore_rapl = amd_uncore_alloc(cpu);
		if (!uncore_rapl)
			goto fail;
		uncore_rapl->cpu = cpu;
		uncore_rapl->num_counters = 1;
		uncore_rapl->msr_base = MSR_AMD_CORE_ENERGY_STATUS;
		uncore_rapl->active_mask = &amd_rapl_active_mask;
		uncore_rapl->pmu = &amd_rapl_pmu;
		uncore_rapl->events = amd_uncore_events_alloc(1, cpu);
		if (!uncore_nb->events)
			goto fail;
		uncore_rapl->id = -1;
		*per_cpu_ptr(amd_uncore_rapl, cpu) = uncore_rapl;
	}

	return 0;

fail:
	if (uncore_nb) {
		kfree(uncore_nb->events);
		kfree(uncore_nb);
	}

	if (uncore_llc) {
		kfree(uncore_llc->events);
		kfree(uncore_llc);
	}

	return -ENOMEM;
}

static struct amd_uncore *
amd_uncore_find_online_sibling(struct amd_uncore *this,
			       struct amd_uncore * __percpu *uncores)
{
	unsigned int cpu;
	struct amd_uncore *that;

	for_each_online_cpu(cpu) {
		that = *per_cpu_ptr(uncores, cpu);

		if (!that)
			continue;

		if (this == that)
			continue;

		if (this->id == that->id) {
			hlist_add_head(&this->node, &uncore_unused_list);
			this = that;
			break;
		}
	}

	this->refcnt++;
	return this;
}

static int amd_uncore_cpu_starting(unsigned int cpu)
{
	unsigned int eax, ebx, ecx, edx;
	struct amd_uncore *uncore;

	if (amd_uncore_nb) {
		uncore = *per_cpu_ptr(amd_uncore_nb, cpu);
		cpuid(0x8000001e, &eax, &ebx, &ecx, &edx);
		uncore->id = ecx & 0xff;

		uncore = amd_uncore_find_online_sibling(uncore, amd_uncore_nb);
		*per_cpu_ptr(amd_uncore_nb, cpu) = uncore;
	}

	if (amd_uncore_llc) {
		uncore = *per_cpu_ptr(amd_uncore_llc, cpu);
		uncore->id = get_llc_id(cpu);

		uncore = amd_uncore_find_online_sibling(uncore, amd_uncore_llc);
		*per_cpu_ptr(amd_uncore_llc, cpu) = uncore;
	}

	if (amd_uncore_rapl) {
		uncore = *per_cpu_ptr(amd_uncore_rapl, cpu);
		uncore->id = topology_core_id(cpu);

		uncore = amd_uncore_find_online_sibling(uncore, amd_uncore_rapl);
		*per_cpu_ptr(amd_uncore_rapl, cpu) = uncore;
	}

	return 0;
}

static void uncore_clean_online(void)
{
	struct amd_uncore *uncore;
	struct hlist_node *n;

	hlist_for_each_entry_safe(uncore, n, &uncore_unused_list, node) {
		hlist_del(&uncore->node);
		kfree(uncore->events);
		kfree(uncore);
	}
}

static void uncore_online(unsigned int cpu,
			  struct amd_uncore * __percpu *uncores)
{
	struct amd_uncore *uncore = *per_cpu_ptr(uncores, cpu);

	uncore_clean_online();

	if (cpu == uncore->cpu)
		cpumask_set_cpu(cpu, uncore->active_mask);
}

static int amd_uncore_cpu_online(unsigned int cpu)
{
	if (amd_uncore_nb)
		uncore_online(cpu, amd_uncore_nb);

	if (amd_uncore_llc)
		uncore_online(cpu, amd_uncore_llc);

	if (amd_uncore_rapl)
		uncore_online(cpu, amd_uncore_rapl);

	return 0;
}

static void uncore_down_prepare(unsigned int cpu,
				struct amd_uncore * __percpu *uncores)
{
	unsigned int i;
	struct amd_uncore *this = *per_cpu_ptr(uncores, cpu);

	if (this->cpu != cpu)
		return;

	/* this cpu is going down, migrate to a shared sibling if possible */
	for_each_online_cpu(i) {
		struct amd_uncore *that = *per_cpu_ptr(uncores, i);

		if (cpu == i)
			continue;

		if (this == that) {
			perf_pmu_migrate_context(this->pmu, cpu, i);
			cpumask_clear_cpu(cpu, that->active_mask);
			cpumask_set_cpu(i, that->active_mask);
			that->cpu = i;
			break;
		}
	}
}

static int amd_uncore_cpu_down_prepare(unsigned int cpu)
{
	if (amd_uncore_nb)
		uncore_down_prepare(cpu, amd_uncore_nb);

	if (amd_uncore_llc)
		uncore_down_prepare(cpu, amd_uncore_llc);

	if (amd_uncore_rapl)
		uncore_down_prepare(cpu, amd_uncore_rapl);

	return 0;
}

static void uncore_dead(unsigned int cpu, struct amd_uncore * __percpu *uncores)
{
	struct amd_uncore *uncore = *per_cpu_ptr(uncores, cpu);

	if (cpu == uncore->cpu)
		cpumask_clear_cpu(cpu, uncore->active_mask);

	if (!--uncore->refcnt) {
		kfree(uncore->events);
		kfree(uncore);
	}

	*per_cpu_ptr(uncores, cpu) = NULL;
}

static int amd_uncore_cpu_dead(unsigned int cpu)
{
	if (amd_uncore_nb)
		uncore_dead(cpu, amd_uncore_nb);

	if (amd_uncore_llc)
		uncore_dead(cpu, amd_uncore_llc);

	if (amd_uncore_rapl)
		uncore_dead(cpu, amd_uncore_rapl);

	return 0;
}

static int __init amd_uncore_init(void)
{
	struct attribute **df_attr = amd_uncore_df_format_attr;
	struct attribute **l3_attr = amd_uncore_l3_format_attr;
	union cpuid_0x80000022_ebx ebx;
	int ret = -ENODEV;
	u64 msr_rapl_power_unit_bits;

	if (boot_cpu_data.x86_vendor != X86_VENDOR_AMD &&
	    boot_cpu_data.x86_vendor != X86_VENDOR_HYGON)
		return -ENODEV;

	if (!boot_cpu_has(X86_FEATURE_TOPOEXT))
		return -ENODEV;

	if (boot_cpu_has(X86_FEATURE_PERFMON_V2))
		pmu_version = 2;

	num_counters_nb	= NUM_COUNTERS_NB;
	num_counters_llc = NUM_COUNTERS_L2;
	if (boot_cpu_data.x86 >= 0x17) {
		/*
		 * For F17h and above, the Northbridge counters are
		 * repurposed as Data Fabric counters. Also, L3
		 * counters are supported too. The PMUs are exported
		 * based on family as either L2 or L3 and NB or DF.
		 */
		num_counters_llc	  = NUM_COUNTERS_L3;
		amd_nb_pmu.name		  = "amd_df";
		amd_llc_pmu.name	  = "amd_l3";
		l3_mask			  = true;
	}

	if (boot_cpu_has(X86_FEATURE_PERFCTR_NB)) {
		if (pmu_version >= 2) {
			*df_attr++ = &format_attr_event14v2.attr;
			*df_attr++ = &format_attr_umask12.attr;
		} else if (boot_cpu_data.x86 >= 0x17) {
			*df_attr = &format_attr_event14.attr;
		}

		amd_uncore_nb = alloc_percpu(struct amd_uncore *);
		if (!amd_uncore_nb) {
			ret = -ENOMEM;
			goto fail_nb;
		}
		ret = perf_pmu_register(&amd_nb_pmu, amd_nb_pmu.name, -1);
		if (ret)
			goto fail_nb;

		if (pmu_version >= 2) {
			ebx.full = cpuid_ebx(EXT_PERFMON_DEBUG_FEATURES);
			num_counters_nb = ebx.split.num_df_pmc;
		}

		pr_info("%d %s %s counters detected\n", num_counters_nb,
			boot_cpu_data.x86_vendor == X86_VENDOR_HYGON ?  "HYGON" : "",
			amd_nb_pmu.name);

		ret = 0;
	}

	if (boot_cpu_has(X86_FEATURE_PERFCTR_LLC)) {
		if (boot_cpu_data.x86 >= 0x19) {
			*l3_attr++ = &format_attr_event8.attr;
			*l3_attr++ = &format_attr_umask8.attr;
			*l3_attr++ = &format_attr_threadmask2.attr;
		} else if (boot_cpu_data.x86 >= 0x17) {
			*l3_attr++ = &format_attr_event8.attr;
			*l3_attr++ = &format_attr_umask8.attr;
			*l3_attr++ = &format_attr_threadmask8.attr;
		}

		amd_uncore_llc = alloc_percpu(struct amd_uncore *);
		if (!amd_uncore_llc) {
			ret = -ENOMEM;
			goto fail_llc;
		}
		ret = perf_pmu_register(&amd_llc_pmu, amd_llc_pmu.name, -1);
		if (ret)
			goto fail_llc;

		pr_info("%d %s %s counters detected\n", num_counters_llc,
			boot_cpu_data.x86_vendor == X86_VENDOR_HYGON ?  "HYGON" : "",
			amd_llc_pmu.name);
		ret = 0;
	}

	if (boot_cpu_has(X86_FEATURE_RAPL)) {
		if (boot_cpu_data.x86 >= 0x19) {
			amd_uncore_rapl = alloc_percpu(struct amd_uncore *);
			if (!amd_uncore_rapl) {
				ret = -ENOMEM;
				goto fail_llc;
			}

			ret = perf_pmu_register(&amd_rapl_pmu, amd_rapl_pmu.name, -1);
			if (ret)
				goto fail_llc;
			pr_info("%s PMU detected\n", amd_rapl_pmu.name);

			if (rdmsrl_safe(MSR_AMD_RAPL_POWER_UNIT, &msr_rapl_power_unit_bits)) {
				pr_err("RAPL POWER unit reg read error\n");
				ret = -1;
			} else {
				rapl_hw_unit = (msr_rapl_power_unit_bits >> 8) & 0x1FULL;
				ret = 0;
			}
		}
	}

	/*
	 * Install callbacks. Core will call them for each online cpu.
	 */
	if (cpuhp_setup_state(CPUHP_PERF_X86_AMD_UNCORE_PREP,
			      "perf/x86/amd/uncore:prepare",
			      amd_uncore_cpu_up_prepare, amd_uncore_cpu_dead))
		goto fail_llc;

	if (cpuhp_setup_state(CPUHP_AP_PERF_X86_AMD_UNCORE_STARTING,
			      "perf/x86/amd/uncore:starting",
			      amd_uncore_cpu_starting, NULL))
		goto fail_prep;
	if (cpuhp_setup_state(CPUHP_AP_PERF_X86_AMD_UNCORE_ONLINE,
			      "perf/x86/amd/uncore:online",
			      amd_uncore_cpu_online,
			      amd_uncore_cpu_down_prepare))
		goto fail_start;
	return 0;

fail_start:
	cpuhp_remove_state(CPUHP_AP_PERF_X86_AMD_UNCORE_STARTING);
fail_prep:
	cpuhp_remove_state(CPUHP_PERF_X86_AMD_UNCORE_PREP);
fail_llc:
	if (boot_cpu_has(X86_FEATURE_PERFCTR_NB))
		perf_pmu_unregister(&amd_nb_pmu);
	free_percpu(amd_uncore_llc);
fail_nb:
	free_percpu(amd_uncore_nb);

	return ret;
}

static void __exit amd_uncore_exit(void)
{
	cpuhp_remove_state(CPUHP_AP_PERF_X86_AMD_UNCORE_ONLINE);
	cpuhp_remove_state(CPUHP_AP_PERF_X86_AMD_UNCORE_STARTING);
	cpuhp_remove_state(CPUHP_PERF_X86_AMD_UNCORE_PREP);

	if (boot_cpu_has(X86_FEATURE_PERFCTR_LLC)) {
		perf_pmu_unregister(&amd_llc_pmu);
		free_percpu(amd_uncore_llc);
		amd_uncore_llc = NULL;
	}

	if (boot_cpu_has(X86_FEATURE_PERFCTR_NB)) {
		perf_pmu_unregister(&amd_nb_pmu);
		free_percpu(amd_uncore_nb);
		amd_uncore_nb = NULL;
	}

	if (boot_cpu_has(X86_FEATURE_RAPL)) {
		perf_pmu_unregister(&amd_rapl_pmu);
		free_percpu(amd_uncore_rapl);
		amd_uncore_rapl = NULL;
	}
}

module_init(amd_uncore_init);
module_exit(amd_uncore_exit);

MODULE_DESCRIPTION("AMD Uncore Driver");
MODULE_LICENSE("GPL v2");
