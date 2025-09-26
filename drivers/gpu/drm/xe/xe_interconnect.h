/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2025 Intel Corporation
 */
#ifndef _XE_INTERCONNECT_H_
#define _XE_INTERCONNECT_H_

#include <linux/types.h>
#include <linux/dma-buf.h>

struct device_private_address;

/* This file needs to be shared between the importer and exporter of the interconnect */

extern const struct dma_buf_interconnect *xe_interconnect;

struct xe_interconnect_attach_ops {
	struct dma_buf_interconnect_attach_ops base;
	/*
	 * Here interconnect-private stuff can be added.
	 * Like a function to check interconnect possibility.
	 */
	bool allow_ic;
};

struct xe_interconnect_attach {
	struct dma_buf_interconnect_attach base;
	struct device_private_address *sg_list_replacement;
};

#endif
