/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Intel Elkhart Lake PSE I/O Auxiliary Device
 *
 * Copyright (c) 2025 Intel Corporation.
 *
 * Author: Raag Jadav <raag.jadav@intel.com>
 */

#ifndef _EHL_PSE_IO_AUX_H_
#define _EHL_PSE_IO_AUX_H_

#include <linux/auxiliary_bus.h>
#include <linux/container_of.h>
#include <linux/ioport.h>

#define EHL_PSE_IO_NAME		"intel_ehl_pse_io"
#define EHL_PSE_GPIO_NAME	"gpio"
#define EHL_PSE_TIO_NAME	"pps_tio"

struct ehl_pse_io_dev {
	struct auxiliary_device aux_dev;
	struct resource mem;
	int irq;
};

#define auxiliary_dev_to_ehl_pse_io_dev(auxiliary_dev) \
	container_of(auxiliary_dev, struct ehl_pse_io_dev, aux_dev)

#endif /* _EHL_PSE_IO_AUX_H_ */
