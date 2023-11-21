/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Memory scrub controller driver support to configure
 * the parameters of the memory scrubbers and enable.
 *
 * Copyright (c) 2023 HiSilicon Limited.
 */

#ifndef __MEMORY_SCRUB_H
#define __MEMORY_SCRUB_H

#include <linux/types.h>

enum scrub_types {
	scrub_common,
	scrub_max,
};

enum scrub_attributes {
	/* scrub attributes - common */
	scrub_addr_base,
	scrub_addr_size,
	scrub_enable,
	scrub_speed,
	scrub_speed_available,
	/* scrub attributes - DDR5 ECS/common */
	scrub_ecs_log_entry_type,
	scrub_ecs_log_entry_type_per_dram,
	scrub_ecs_log_entry_type_per_memory_media,
	scrub_mode,
	scrub_mode_counts_rows,
	scrub_mode_counts_codewords,
	scrub_reset_counter,
	scrub_threshold,
	scrub_threshold_available,
	max_attrs,
};

/**
 * struct scrub_ops - scrub device operations
 * @is_visible: Callback to return attribute visibility. Mandatory.
 *		Parameters are:
 *		@drvdata:
 *			pointer to driver-private data structure passed
 *			as argument to scrub_device_register().
 *		@attr:	scrubber attribute
 *		@region_id:
 *			memory region id
 *		The function returns the file permissions.
 *		If the return value is 0, no attribute will be created.
 * @read:	Read callback for data attributes. Mandatory if readable
 *		data attributes are present.
 *		Parameters are:
 *		@dev:	pointer to hardware scrub device
 *		@attr:	scrubber attribute
 *		@region_id:
 *			memory region id
 *		@val:	pointer to returned value
 *		The function returns 0 on success or a negative error number.
 * @read_string: Read callback for string attributes. Mandatory if string
 *		attributes are present.
 *		Parameters are:
 *		@dev:	pointer to hardware scrub device
 *		@attr:	scrubber attribute
 *		@region_id:
 *			memory region id
 *		@buf:	pointer to buffer to copy string
 *		The function returns 0 on success or a negative error number.
 * @write:	Write callback for data attributes. Mandatory if writeable
 *		data attributes are present.
 *		Parameters are:
 *		@dev:	pointer to hardware scrub device
 *		@attr:	scrubber attribute
 *		@region_id:
 *			memory region id
 *		@val:	value to write
 *		The function returns 0 on success or a negative error number.
 */
struct scrub_ops {
	umode_t (*is_visible)(const void *drvdata, u32 attr, int region_id);
	int (*read)(struct device *dev, u32 attr, int region_id, u64 *val);
	int (*read_string)(struct device *dev, u32 attr, int region_id, char *buf);
	int (*write)(struct device *dev, u32 attr, int region_id, u64 val);
};

struct device *
devm_scrub_device_register(struct device *dev, const char *name,
			   void *drvdata, const struct scrub_ops *ops,
			   int nregions);
#endif /* __MEMORY_SCRUB_H */
