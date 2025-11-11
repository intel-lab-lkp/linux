/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CXL_CACHE_H__
#define __CXL_CACHE_H__
#include "cxl.h"

/**
 * struct cxl_cachedev - CXL bus object representing a cache-capable CXL device
 * @dev: driver core device object
 * @cxlds: device state backing this device
 * @endpoint: connection to the CXL port topology for this device
 * @ops: caller specific probe routine
 * @id: id number of this cachedev instance
 * @depth: endpoint port depth in hierarchy
 */
struct cxl_cachedev {
	struct device dev;
	struct cxl_dev_state *cxlds;
	struct cxl_port *endpoint;
	const struct cxl_dev_ops *ops;
	int id;
	int depth;
};

static inline struct cxl_cachedev *to_cxl_cachedev(struct device *dev)
{
	return container_of(dev, struct cxl_cachedev, dev);
}

bool is_cxl_cachedev(const struct device *dev);

int cxl_accel_read_cache_info(struct cxl_dev_state *cxlds);
#endif
