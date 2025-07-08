/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Samsung Platform Performance Measuring Unit (PPMU) headers
 *
 * Copyright (c) 2024-25 Samsung Electronics Co., Ltd.
 *
 * Authors: Vivek Yadav <vivek.2311@samsung.com>
 *          Ravi Patel <ravi.patel@samsung.com>
 */

#ifndef __SAMSUNG_PPMU_H__
#define __SAMSUNG_PPMU_H__

#include <linux/clk.h>

#define PPMU_MAX_COUNTERS	(5)

#define to_samsung_ppmu(p)	(container_of(p, struct samsung_ppmu, pmu))

#define SAMSUNG_PPMU_ATTR(_name, _func, _config)			\
	(&((struct dev_ext_attribute[]) {				\
		{ __ATTR(_name, 0444, _func, NULL), (void *)_config }	\
	})[0].attr.attr)

#define SAMSUNG_PPMU_FORMAT_ATTR(_name, _config)		\
	SAMSUNG_PPMU_ATTR(_name, samsung_ppmu_format_sysfs_show, (void *)_config)
#define SAMSUNG_PPMU_EVENT_ATTR(_name, _config)		\
	SAMSUNG_PPMU_ATTR(_name, samsung_ppmu_event_sysfs_show, (unsigned long)_config)

#define SAMSUNG_PPMU_GET_EVENTID(ev) ((ev)->hw.config_base & 0xff)

enum ppmu_clock_type {
	PPMU_ACLK,
	PPMU_PCLK,
	PPMU_CLK_COUNT,
};

enum ppmu_status {
	PPMU_STOP,
	PPMU_START,
};

struct samsung_ppmu;

struct samsung_ppmu_ops {
	void (*write_evtype)(struct samsung_ppmu *s_ppmu, int idx, u32 type);
	int (*get_event_idx)(struct perf_event *event);
	u64 (*read_counter)(struct samsung_ppmu *s_ppmu, struct hw_perf_event *event);
	void (*enable_counter)(struct samsung_ppmu *s_ppmu, struct hw_perf_event *event);
	void (*disable_counter)(struct samsung_ppmu *s_ppmu, struct hw_perf_event *event);
	void (*start_counters)(struct samsung_ppmu *s_ppmu);
	void (*stop_counters)(struct samsung_ppmu *s_ppmu);
	u32 (*get_int_status)(struct samsung_ppmu *s_ppmu);
	void (*clear_int_status)(struct samsung_ppmu *s_ppmu, int idx);
};

/* Describes the Samsung PPMU features information */
struct samsung_ppmu_dev_info {
	const char *name;
	const struct attribute_group **attr_groups;
	void *private;
};

struct samsung_ppmu_hwevents {
	struct perf_event *hw_events[PPMU_MAX_COUNTERS];
	DECLARE_BITMAP(used_mask, PPMU_MAX_COUNTERS);
	const struct attribute_group **attr_groups;
};

struct samsung_ppmu_drv_data {
	const struct attribute_group **ppmu_attr_group;
};

/* Generic pmu struct for different pmu types */
struct samsung_ppmu {
	struct pmu pmu;
	const struct samsung_ppmu_ops *ops;
	const struct samsung_ppmu_dev_info *dev_info;
	struct samsung_ppmu_hwevents pmu_events;
	const struct samsung_ppmu_drv_data *ppmu_data;
	u32 samsung_ppmu_version;
	u32 samsung_ppmu_master_id_val;
	u8 status;
	u8 id;
	/* CPU used for counting */
	int on_cpu;
	int irq0;
	int irq1;
	struct device *dev;
	struct hlist_node node;
	void __iomem *base;
	int num_counters;
	u32 counter_overflow[PPMU_MAX_COUNTERS];
	u64 prev_counter[PPMU_MAX_COUNTERS];
	/* check event code range */
	int check_event;
	u32 identifier;
	struct clk_bulk_data clks[PPMU_CLK_COUNT];
};

void samsung_ppmu_read(struct perf_event *event);
int samsung_ppmu_add(struct perf_event *event, int flags);
void samsung_ppmu_del(struct perf_event *event, int flags);
void samsung_ppmu_start(struct perf_event *event, int flags);
void samsung_ppmu_stop(struct perf_event *event, int flags);
void samsung_ppmu_set_event_period(struct perf_event *event);
void samsung_ppmu_event_update(struct perf_event *event);
int samsung_ppmu_event_init(struct perf_event *event);
void samsung_ppmu_enable(struct pmu *pmu);
void samsung_ppmu_disable(struct pmu *pmu);
ssize_t samsung_ppmu_event_sysfs_show(struct device *dev,
				      struct device_attribute *attr, char *buf);
ssize_t samsung_ppmu_format_sysfs_show(struct device *dev,
				       struct device_attribute *attr, char *buf);
ssize_t samsung_ppmu_cpumask_sysfs_show(struct device *dev,
					struct device_attribute *attr, char *buf);
int samsung_ppmu_online_cpu(unsigned int cpu, struct hlist_node *node);
int samsung_ppmu_offline_cpu(unsigned int cpu, struct hlist_node *node);

ssize_t samsung_ppmu_identifier_attr_show(struct device *dev,
					  struct device_attribute *attr,
					  char *page);
int samsung_ppmu_init_irq(struct samsung_ppmu *samsung_ppmu,
			  struct platform_device *pdev);

void samsung_ppmu_init(struct samsung_ppmu *samsung_ppmu, struct module *module);

#endif /* __SAMSUNG_PPMU_H__ */
