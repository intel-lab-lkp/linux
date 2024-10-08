/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * PPS generator API kernel header
 *
 * Copyright (C) 2024   Rodolfo Giometti <giometti@enneenne.com>
 */

#ifndef LINUX_PPS_GEN_KERNEL_H
#define LINUX_PPS_GEN_KERNEL_H

#include <linux/pps_gen.h>
#include <linux/cdev.h>
#include <linux/device.h>

/*
 * Global defines
 */

struct pps_gen_device;

/* The specific PPS source info */
struct pps_gen_source_info {
	char name[PPS_GEN_MAX_NAME_LEN];	/* symbolic name */
	bool use_system_clock;

	int (*get_time)(struct pps_gen_device *pps_gen,
					struct timespec64 *time);
	int (*enable)(struct pps_gen_device *pps_gen, bool enable);

	struct module *owner;
	struct device *parent;			/* for device_create */
};

/* The main struct */
struct pps_gen_device {
	struct pps_gen_source_info info;	/* PSS generator info */
	bool enabled;				/* PSS generator status */

	unsigned int id;			/* PPS generator unique ID */
	struct device *dev;
};

/*
 * Global variables
 */

extern const struct attribute_group *pps_gen_groups[];

/*
 * Exported functions
 */

extern struct pps_gen_device *pps_gen_register_source(
		struct pps_gen_source_info *info);
extern void pps_gen_unregister_source(struct pps_gen_device *pps_gen);

#endif /* LINUX_PPS_GEN_KERNEL_H */
