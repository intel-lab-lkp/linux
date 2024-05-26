/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef __LOONG_GPU_PCI_DRV_H__
#define __LOONG_GPU_PCI_DRV_H__

#include <linux/pci.h>

struct loong_gpu_device {
	struct pci_dev *pdev;
	void __iomem *reg_base;

	u32 ver_major;
	u32 ver_minor;
	u32 revision;
};

static inline u32 loong_rreg32(struct loong_gpu_device *ldev, u32 offset)
{
	return readl(ldev->reg_base + offset);
}

static inline void loong_wreg32(struct loong_gpu_device *ldev, u32 offset, u32 val)
{
	writel(val, ldev->reg_base + offset);
}

#endif
