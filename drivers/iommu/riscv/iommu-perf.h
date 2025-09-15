// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU hpm implementations
 * Copyright(c) 2025 Beijing Institute of Open Source Chip (BOSC)
 */

#ifndef __IOMMU_PERF_H__
#define __IOMMU_PERF_H__

#include <linux/sysfs.h>
#include <linux/perf_event.h>
#include <linux/bitops.h>

typedef union {
	struct {
		unsigned long eventID:15;
		unsigned long DMASK:1;
		unsigned long PID_PSCID:20;
		unsigned long DID_GSCID:24;
		unsigned long PV_PSCV:1;
		unsigned long DV_GSCV:1;
		unsigned long IDT:1;
		unsigned long OF:1;
	};
	unsigned long val;
} iohpmevt_t;

typedef union {
	struct {
		unsigned long pv_pscv:1;
		unsigned long dv_gscv:1;
		unsigned long idt:1;
		unsigned long reserved:17;
		unsigned long pid_pscid:20;
		unsigned long did_gscid:24;
	};
	unsigned long val;
} riscv_iommu_pmu_cfg1_t;

struct riscv_iommu_pmu_event_map {
	const char *compatible;
	const struct attribute_group **attr_group;
};

#define RISCV_IOMMU_IOHPMCTR_CNT 32

struct riscv_iommu_perf_event {
	int pv_pscv;
	int dv_gscv;
	int idt;
	int pid_pscid;
	int did_gscid;
	struct perf_event *perf_event;
};

struct riscv_iommu_pmu {
	struct riscv_iommu_device *iommu;
	struct pmu pmu;
};

static inline struct riscv_iommu_pmu *dev_to_riscv_iommu_pmu(struct device *dev)
{
	return container_of(dev_get_drvdata(dev), struct riscv_iommu_pmu, pmu);
}

static inline struct riscv_iommu_pmu *riscv_iommu_event_to_pmu(struct perf_event *event)
{
	return container_of(event->pmu, struct riscv_iommu_pmu, pmu);
}

#define RISCV_IOMMU_PMU_EXT_EVENT_ATTR(_name, _string)			\
	PMU_EVENT_ATTR_STRING(_name, event_attr_##_name, _string)	\
									\
static struct attribute *_name##_attr[] = {				\
	&event_attr_##_name.attr.attr,					\
	NULL								\
};									\
									\
static struct attribute_group _name = {					\
	.name		= "events",					\
	.attrs		= _name##_attr,					\
};

int riscv_iommu_pmu_alloc(struct riscv_iommu_device *iommu);
int riscv_iommu_pmu_register(struct riscv_iommu_device *iommu);
void riscv_iommu_pmu_unregister(struct riscv_iommu_device *iommu);

#endif
