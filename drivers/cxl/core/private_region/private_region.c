// SPDX-License-Identifier: GPL-2.0
/*
 * CXL Private Region - dispatch and lifecycle management
 *
 * This file implements the main registration and unregistration dispatch
 * for CXL private regions. It handles common initialization and delegates
 * to type-specific implementations.
 */

#include <linux/device.h>
#include <linux/cleanup.h>
#include "../../cxl.h"
#include "../core.h"
#include "private_region.h"

static const char *private_type_to_string(enum cxl_private_region_type type)
{
	switch (type) {
	default:
		return "";
	}
}

static enum cxl_private_region_type string_to_private_type(const char *str)
{
	return CXL_PRIVATE_NONE;
}

static ssize_t private_type_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct cxl_region *cxlr = to_cxl_region(dev);

	return sysfs_emit(buf, "%s\n", private_type_to_string(cxlr->private_type));
}

static ssize_t private_type_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len)
{
	struct cxl_region *cxlr = to_cxl_region(dev);
	struct cxl_region_params *p = &cxlr->params;
	enum cxl_private_region_type type;
	ssize_t rc;

	type = string_to_private_type(buf);
	if (type == CXL_PRIVATE_NONE)
		return -EINVAL;

	ACQUIRE(rwsem_write_kill, rwsem)(&cxl_rwsem.region);
	if ((rc = ACQUIRE_ERR(rwsem_write_kill, &rwsem)))
		return rc;

	/* Can only change type before region is committed */
	if (p->state >= CXL_CONFIG_COMMIT)
		return -EBUSY;

	cxlr->private_type = type;

	return len;
}
DEVICE_ATTR_RW(private_type);

/*
 * Register a private CXL region based on its private_type.
 *
 * This function is called during commit. It validates the private_type,
 * initializes the private_ops, and dispatches to the appropriate
 * registration function which handles memtype, callbacks, and node
 * registration.
 */
int cxl_register_private_region(struct cxl_region *cxlr)
{
	int rc = 0;

	if (!cxlr->params.res)
		return -EINVAL;

	if (cxlr->private_type == CXL_PRIVATE_NONE) {
		dev_err(&cxlr->dev, "private_type must be set before commit\n");
		return -EINVAL;
	}

	/* Initialize the private_ops with region info */
	cxlr->private_ops.res_start = cxlr->params.res->start;
	cxlr->private_ops.res_end = cxlr->params.res->end;
	cxlr->private_ops.data = cxlr;

	/* Call type-specific registration which sets memtype and callbacks */
	switch (cxlr->private_type) {
	default:
		dev_dbg(&cxlr->dev, "unsupported private_type: %d\n",
			cxlr->private_type);
		rc = -EINVAL;
		break;
	}

	if (!rc)
		set_bit(CXL_REGION_F_PRIVATE_REGISTERED, &cxlr->flags);
	return rc;
}

/*
 * Unregister a private CXL region.
 *
 * This function is called during region reset or device release.
 * It dispatches to the appropriate type-specific cleanup function.
 */
void cxl_unregister_private_region(struct cxl_region *cxlr)
{
	if (!test_and_clear_bit(CXL_REGION_F_PRIVATE_REGISTERED, &cxlr->flags))
		return;

	/* Dispatch to type-specific cleanup */
	switch (cxlr->private_type) {
	default:
		break;
	}
}
