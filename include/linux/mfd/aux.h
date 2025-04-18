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
struct mfd_aux_device {
	struct auxiliary_device auxdev;
	struct resource	mem;
	struct resource	irq;
	/* Place holder for other types */
	struct resource	ext;
};

#define auxiliary_dev_to_mfd_aux_dev(auxiliary_dev) \
	container_of(auxiliary_dev, struct mfd_aux_device, auxdev)

#endif
