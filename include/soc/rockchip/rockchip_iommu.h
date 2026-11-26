/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef __SOC_ROCKCHIP_IOMMU_H__
#define __SOC_ROCKCHIP_IOMMU_H__

struct device;

#ifdef CONFIG_ROCKCHIP_IOMMU
int rockchip_iommu_restore(struct device *dev);
#else
static inline int rockchip_iommu_restore(struct device *dev)
{
	return 0;
}
#endif

#endif /* __SOC_ROCKCHIP_IOMMU_H__ */
