/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Authors:
 *      Sui Jingfeng <sui.jingfeng@linux.dev>
 */

#ifndef __LOONGGPU_PCI_DRV_H__
#define __LOONGGPU_PCI_DRV_H__

#include <linux/component.h>
#include <linux/pci.h>

struct loonggpu_device {
	struct pci_dev *pdev;
	struct drm_device *drm;

	void __iomem *reg_base;
	int irq;

	u32 ver_major;
	u32 ver_minor;
	u32 revision;
};

static inline u32 loong_rreg32(struct loonggpu_device *ldev, u32 offset)
{
	return readl(ldev->reg_base + offset);
}

static inline void loong_wreg32(struct loonggpu_device *ldev, u32 offset, u32 val)
{
	writel(val, ldev->reg_base + offset);
}

#endif
