/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MFD auxiliary device
 *
 * Copyright (c) 2025 Raag Jadav <raag.jadav@intel.com>
 */

#ifndef MFD_AUX_H
#define MFD_AUX_H

#include <linux/auxiliary_bus.h>
#include <linux/container_of.h>
#include <linux/ioport.h>

/*
 * Common structure between MFD parent and auxiliary child device.
 * To be used by leaf drivers to access child device resources.
 */
struct maux_device {
	struct auxiliary_device auxdev;
	u32 num_resources;
	struct resource	*resource;
};

#define auxiliary_dev_to_maux_dev(auxiliary_dev) \
	container_of(auxiliary_dev, struct maux_device, auxdev)

struct resource *maux_get_resource(struct maux_device *maux, unsigned int type, unsigned int num);
int maux_get_irq_optional(struct maux_device *maux, unsigned int num);
int maux_get_irq(struct maux_device *maux, unsigned int num);

#ifdef CONFIG_HAS_IOMEM
void __iomem *devm_maux_get_and_ioremap_resource(struct maux_device *maux, unsigned int index,
						 struct resource **res);
void __iomem *devm_maux_ioremap_resource(struct maux_device *maux, unsigned int index);
#endif

#endif
