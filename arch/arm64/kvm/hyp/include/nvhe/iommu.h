/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM64_KVM_NVHE_IOMMU_H__
#define __ARM64_KVM_NVHE_IOMMU_H__

struct pkvm_iommu_ops {
	int (*init)(void);
};

int pkvm_iommu_init(void);

#endif /* __ARM64_KVM_NVHE_IOMMU_H__ */
