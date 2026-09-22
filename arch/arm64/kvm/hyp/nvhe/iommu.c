// SPDX-License-Identifier: GPL-2.0
/*
 * IOMMU operations for pKVM
 *
 * Copyright (C) 2022 Linaro Ltd.
 */
#include <nvhe/iommu.h>

/* Only one set of ops supported */
struct pkvm_iommu_ops *pkvm_iommu_ops;

int pkvm_iommu_init(void)
{
	/* Keep DMA isolation optional. */
	if (!pkvm_iommu_ops || !pkvm_iommu_ops->init)
		return 0;

	return pkvm_iommu_ops->init();
}
