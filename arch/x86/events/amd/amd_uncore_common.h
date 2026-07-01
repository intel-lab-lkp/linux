/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _X86_EVENTS_AMD_UNCORE_COMMON_H
#define _X86_EVENTS_AMD_UNCORE_COMMON_H

#include <linux/cpumask.h>
#include <linux/device.h>
#include <linux/hrtimer.h>
#include <linux/perf_event.h>
#include <linux/percpu.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#define UNCORE_NAME_LEN		16
#define UNCORE_GROUP_MAX	256
#define NUM_COUNTERS_MAX	64
#define COUNTER_SHIFT		16

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

union amd_uncore_common_info {
	struct {
		u64 aux_data:32;
		u64 num_pmcs:8;
		u64 gid:8;
		u64 cid:8;
		u64 private:8;
	} split;
	u64 full;
};

struct amd_uncore_common_ctx {
	int refcnt;
	int cpu;
	struct perf_event **events;
	unsigned long active_mask[BITS_TO_LONGS(NUM_COUNTERS_MAX)];
	int nr_active;
	struct hrtimer hrtimer;
	u64 hrtimer_duration;
};

struct amd_uncore_common_pmu {
	char name[UNCORE_NAME_LEN];
	int num_counters;
	int rdpmc_base;
	u32 msr_base;
	int group;
	cpumask_t active_mask;
	struct pmu pmu;
	struct amd_uncore_common_ctx * __percpu *ctx;
	void *private;
};

struct amd_uncore_common {
	union amd_uncore_common_info __percpu *info;
	struct amd_uncore_common_pmu *pmus;
	unsigned int num_pmus;
	bool init_done;
	void (*scan)(struct amd_uncore_common *uncore, unsigned int cpu);
	int (*init)(struct amd_uncore_common *uncore, unsigned int cpu);
	void (*move)(struct amd_uncore_common *uncore, unsigned int cpu);
	void (*free)(struct amd_uncore_common *uncore, unsigned int cpu);
};

extern struct attribute_group amd_uncore_common_attr_group;

static inline int amd_uncore_common_ctx_cid(struct amd_uncore_common *uncore,
					    unsigned int cpu)
{
	union amd_uncore_common_info *info = per_cpu_ptr(uncore->info, cpu);

	return info->split.cid;
}

static inline int amd_uncore_common_ctx_gid(struct amd_uncore_common *uncore,
					    unsigned int cpu)
{
	union amd_uncore_common_info *info = per_cpu_ptr(uncore->info, cpu);

	return info->split.gid;
}

static inline int amd_uncore_common_ctx_num_pmcs(struct amd_uncore_common *uncore,
						 unsigned int cpu)
{
	union amd_uncore_common_info *info = per_cpu_ptr(uncore->info, cpu);

	return info->split.num_pmcs;
}

struct amd_uncore_common_pmu *event_to_amd_uncore_common_pmu(struct perf_event *event);

void amd_uncore_common_set_update_interval(unsigned int interval);
int amd_uncore_common_event_init(struct perf_event *event);
int amd_uncore_common_add(struct perf_event *event, int flags);
void amd_uncore_common_del(struct perf_event *event, int flags);
void amd_uncore_common_start(struct perf_event *event, int flags);
void amd_uncore_common_stop(struct perf_event *event, int flags);
void amd_uncore_common_read(struct perf_event *event);

int amd_uncore_common_ctx_init(struct amd_uncore_common *uncore, unsigned int cpu);
void amd_uncore_common_ctx_free(struct amd_uncore_common *uncore, unsigned int cpu);
void amd_uncore_common_ctx_move(struct amd_uncore_common *uncore, unsigned int cpu);
void amd_uncore_common_start_hrtimer(struct amd_uncore_common_ctx *ctx);

#endif /* _X86_EVENTS_AMD_UNCORE_COMMON_H */
