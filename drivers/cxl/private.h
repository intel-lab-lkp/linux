// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2025 Intel Corporation. */

/*
 * Private interfaces betwen common drivers ("cxl_mem", "cxl_port") and
 * the cxl_core.
 */

#include "cxl.h"

#ifndef __CXL_PRIVATE_H__
#define __CXL_PRIVATE_H__
int devm_cxl_add_endpoint(struct device *host, struct device *ep_dev,
			  struct cxl_dport *parent_dport);

struct cxl_cachedev *cxl_cachedev_alloc(struct cxl_dev_state *cxlds);
struct cxl_cachedev *devm_cxl_cachedev_add_or_reset(struct device *host,
						    struct cxl_cachedev *cxlcd);
#endif /* __CXL_PRIVATE_H__ */
