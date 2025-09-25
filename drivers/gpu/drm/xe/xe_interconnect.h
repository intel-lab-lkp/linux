/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */
#ifndef _XE_INTERCONNECT_H_
#define _XE_INTERCONNECT_H_

#include <linux/types.h>

/* This file needs to be shared between the importer and exporter of the interconnect */

extern void *xe_interconnect;

struct xe_interconnect_attach_ops {
	/*
	 * Here interconnect-private stuff can be added.
	 * Like a function to check interconnect possibility.
	 */
	bool allow_ic;
};

#endif
