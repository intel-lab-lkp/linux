/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright(c) 2024 Huawei
 * CXL Specification rev 3.2 Setion 8.2.8 (CHMU Register Interface)
 */
#ifndef CXL_HMU_H
#define CXL_HMU_H
#include <linux/device.h>

#define CXL_HMU_REGMAP_SIZE 0xe00 /* Table 8-32 CXL 3.0 specification */
struct cxl_hmu {
	struct device dev;
	void __iomem *base;
	int assoc_id;
	int index;
};

#define to_cxl_hmu(dev) container_of(dev, struct cxl_hmu, dev)
struct cxl_hmu_regs;
int devm_cxl_hmu_add(struct device *parent, struct cxl_hmu_regs *regs,
		     int assoc_id, int idx);

#endif
