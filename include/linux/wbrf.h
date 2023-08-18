/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Wifi Band Exclusion Interface
 * Copyright (C) 2023 Advanced Micro Devices
 */

#ifndef _LINUX_WBRF_H
#define _LINUX_WBRF_H

#include <linux/device.h>

/* Maximum number of wbrf ranges */
#define MAX_NUM_OF_WBRF_RANGES		11

struct exclusion_range {
	/* start and end point of the frequency range in Hz */
	u64		start;
	u64		end;
};

struct wbrf_ranges_in {
	/* valid entry: `start` and `end` filled with non-zero values */
	struct exclusion_range	band_list[MAX_NUM_OF_WBRF_RANGES];
};

struct wbrf_ranges_out {
	u64			num_of_ranges;
	struct exclusion_range	band_list[MAX_NUM_OF_WBRF_RANGES];
};

enum wbrf_notifier_actions {
	WBRF_CHANGED,
};

bool wbrf_supported_producer(struct device *dev);
int wbrf_add_exclusion(struct device *adev,
		       struct wbrf_ranges_in *in);
int wbrf_remove_exclusion(struct device *dev,
			  struct wbrf_ranges_in *in);
int wbrf_retrieve_exclusions(struct device *dev,
			     struct wbrf_ranges_out *out);
bool wbrf_supported_consumer(struct device *dev);

int wbrf_register_notifier(struct notifier_block *nb);
int wbrf_unregister_notifier(struct notifier_block *nb);

#endif /* _LINUX_WBRF_H */
