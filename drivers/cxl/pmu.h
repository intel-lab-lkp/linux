/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright(c) 2023 Huawei
 * CXL Specification rev 3.0 Setion 8.2.7 (CPMU Register Interface)
 */
#ifndef CXL_PMU_H
#define CXL_PMU_H
#include <linux/device.h>

enum cxl_pmu_type {
	CXL_PMU_MEMDEV,
};

#define CXL_PMU_REGMAP_SIZE 0xe00 /* Table 8-32 CXL 3.0 specification */
struct cxl_pmu {
	struct device dev;
	void __iomem *base;
	int assoc_id;
	int index;
	enum cxl_pmu_type type;
};

#define to_cxl_pmu(dev) container_of(dev, struct cxl_pmu, dev)
struct cxl_pmu_regs;
int devm_cxl_pmu_add(struct device *parent, struct cxl_pmu_regs *regs,
		     int assoc_id, int idx, enum cxl_pmu_type type);

#define CXL_PMU_CAP_REG			0x0
#define   CXL_PMU_CAP_NUM_COUNTERS_MSK			GENMASK_ULL(5, 0)
#define   CXL_PMU_CAP_COUNTER_WIDTH_MSK			GENMASK_ULL(15, 8)
#define   CXL_PMU_CAP_NUM_EVN_CAP_REG_SUP_MSK		GENMASK_ULL(24, 20)
#define   CXL_PMU_CAP_FILTERS_SUP_MSK			GENMASK_ULL(39, 32)
#define     CXL_PMU_FILTER_HDM				BIT(0)
#define     CXL_PMU_FILTER_CHAN_RANK_BANK		BIT(1)
#define   CXL_PMU_CAP_MSI_N_MSK				GENMASK_ULL(47, 44)
#define   CXL_PMU_CAP_WRITEABLE_WHEN_FROZEN		BIT_ULL(48)
#define   CXL_PMU_CAP_FREEZE				BIT_ULL(49)
#define   CXL_PMU_CAP_INT				BIT_ULL(50)
#define   CXL_PMU_CAP_VERSION_MSK			GENMASK_ULL(63, 60)

#define CXL_PMU_OVERFLOW_REG		0x10
#define CXL_PMU_FREEZE_REG		0x18
#define CXL_PMU_EVENT_CAP_REG(n)	(0x100 + 8 * (n))
#define   CXL_PMU_EVENT_CAP_SUPPORTED_EVENTS_MSK	GENMASK_ULL(31, 0)
#define   CXL_PMU_EVENT_CAP_GROUP_ID_MSK		GENMASK_ULL(47, 32)
#define   CXL_PMU_EVENT_CAP_VENDOR_ID_MSK		GENMASK_ULL(63, 48)

#define CXL_PMU_COUNTER_CFG_REG(n)	(0x200 + 8 * (n))
#define   CXL_PMU_COUNTER_CFG_TYPE_MSK			GENMASK_ULL(1, 0)
#define     CXL_PMU_COUNTER_CFG_TYPE_FREE_RUN		0
#define     CXL_PMU_COUNTER_CFG_TYPE_FIXED_FUN		1
#define     CXL_PMU_COUNTER_CFG_TYPE_CONFIGURABLE	2
#define   CXL_PMU_COUNTER_CFG_ENABLE			BIT_ULL(8)
#define   CXL_PMU_COUNTER_CFG_INT_ON_OVRFLW		BIT_ULL(9)
#define   CXL_PMU_COUNTER_CFG_FREEZE_ON_OVRFLW		BIT_ULL(10)
#define   CXL_PMU_COUNTER_CFG_EDGE			BIT_ULL(11)
#define   CXL_PMU_COUNTER_CFG_INVERT			BIT_ULL(12)
#define   CXL_PMU_COUNTER_CFG_THRESHOLD_MSK		GENMASK_ULL(23, 16)
#define   CXL_PMU_COUNTER_CFG_EVENTS_MSK		GENMASK_ULL(55, 24)
#define   CXL_PMU_COUNTER_CFG_EVENT_GRP_ID_IDX_MSK	GENMASK_ULL(63, 59)

#define CXL_PMU_FILTER_CFG_REG(n, f)	(0x400 + 4 * ((f) + (n) * 8))
#define   CXL_PMU_FILTER_CFG_VALUE_MSK			GENMASK(31, 0)

#define CXL_PMU_COUNTER_REG(n)		(0xc00 + 8 * (n))

/* CXL rev 3.0 Table 13-5 Events under CXL Vendor ID */
#define CXL_PMU_GID_CLOCK_TICKS		0x00
#define CXL_PMU_GID_D2H_REQ		0x0010
#define CXL_PMU_GID_D2H_RSP		0x0011
#define CXL_PMU_GID_H2D_REQ		0x0012
#define CXL_PMU_GID_H2D_RSP		0x0013
#define CXL_PMU_GID_CACHE_DATA		0x0014
#define CXL_PMU_GID_M2S_REQ		0x0020
#define CXL_PMU_GID_M2S_RWD		0x0021
#define CXL_PMU_GID_M2S_BIRSP		0x0022
#define CXL_PMU_GID_S2M_BISNP		0x0023
#define CXL_PMU_GID_S2M_NDR		0x0024
#define CXL_PMU_GID_S2M_DRS		0x0025
#define CXL_PMU_GID_DDR			0x8000

#endif
