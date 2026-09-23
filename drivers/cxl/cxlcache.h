/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CXL_CACHE_H__
#define __CXL_CACHE_H__

#include "cxl.h"

/**
 * struct cxl_cachedev - CXL bus object representing the cache capabilities of
 * a CXL device
 * @dev: driver core device object
 * @cxlds: device state backing this device
 * @detach_work: active cachedev lost a port in its ancestry
 * @endpoint: connection to the CXL port topology for this device
 * @id: id number of this cachedev instance
 * @depth: endpoint port depth in hierarchy
 */
struct cxl_cachedev {
	struct device dev;
	struct cxl_dev_state *cxlds;
	struct work_struct detach_work;
	struct cxl_port *endpoint;
	int id;
	int depth;
};

bool is_cxl_cachedev(const struct device *dev);

static inline struct cxl_cachedev *to_cxl_cachedev(struct device *dev)
{
	if (dev_WARN_ONCE(dev, !is_cxl_cachedev(dev),
			  "to_cxl_cachedev() called on non cxl_cachedev device"))
		return NULL;

	return container_of(dev, struct cxl_cachedev, dev);
}

struct cxl_cachedev *__devm_cxl_add_cachedev(struct cxl_dev_state *cxlds,
					     void *attach);
int cxl_accel_read_cache_info(struct cxl_dev_state *cxlds);
void cxl_clear_cache_enable(struct cxl_dev_state *cxlds);
int devm_cxl_enable_cache(struct device *host, struct cxl_dev_state *cxlds);
#endif
