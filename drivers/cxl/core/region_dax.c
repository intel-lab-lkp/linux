// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright(c) 2022 Intel Corporation. All rights reserved.
 * Copyright(c) 2026 Meta Technologies Inc. All rights reserved.
 */
#include <linux/device.h>
#include <linux/slab.h>
#include <cxlmem.h>
#include <cxl.h>
#include "core.h"

static void cxl_dax_region_release(struct device *dev)
{
	struct cxl_dax_region *cxlr_dax = to_cxl_dax_region(dev);

	if (cxlr_dax->cxled)
		put_device(&cxlr_dax->cxled->cxld.dev);
	xa_destroy(&cxlr_dax->dc_extents);
	ida_destroy(&cxlr_dax->tag_group_ida);
	kfree(cxlr_dax);
}

static const struct attribute_group *cxl_dax_region_attribute_groups[] = {
	&cxl_base_attribute_group,
	NULL
};

const struct device_type cxl_dax_region_type = {
	.name = "cxl_dax_region",
	.release = cxl_dax_region_release,
	.groups = cxl_dax_region_attribute_groups,
};

static void cxl_dax_untagged_release(struct device *dev)
{
	struct cxl_dax_region *cxlr_dax =
		container_of(dev, struct cxl_dax_region, untagged);

	/* Drop the pin taken in cxl_dax_region_alloc(). */
	put_device(&cxlr_dax->dev);
}

static const struct device_type cxl_dax_untagged_type = {
	.name = "untagged",
	.release = cxl_dax_untagged_release,
};

static bool is_cxl_dax_region(struct device *dev)
{
	return dev->type == &cxl_dax_region_type;
}

struct cxl_dax_region *to_cxl_dax_region(struct device *dev)
{
	if (dev_WARN_ONCE(dev, !is_cxl_dax_region(dev),
			  "not a cxl_dax_region device\n"))
		return NULL;
	return container_of(dev, struct cxl_dax_region, dev);
}
EXPORT_SYMBOL_NS_GPL(to_cxl_dax_region, "CXL");

static struct lock_class_key cxl_dax_region_key;

static struct cxl_dax_region *cxl_dax_region_alloc(struct cxl_region *cxlr)
{
	struct cxl_region_params *p = &cxlr->params;
	struct cxl_dax_region *cxlr_dax;
	struct device *dev;

	/* Publishes cxlr->cxlr_dax, which the event paths read under the read lock */
	guard(rwsem_write)(&cxl_rwsem.region);
	if (p->state != CXL_CONFIG_COMMIT)
		return ERR_PTR(-ENXIO);

	cxlr_dax = kzalloc_obj(*cxlr_dax);
	if (!cxlr_dax)
		return ERR_PTR(-ENOMEM);

	xa_init_flags(&cxlr_dax->dc_extents, XA_FLAGS_ALLOC1);
	ida_init(&cxlr_dax->tag_group_ida);
	cxlr_dax->hpa_range.start = p->res->start;
	cxlr_dax->hpa_range.end = p->res->end;

	/*
	 * DC regions are not interleaved; the probe-time scan uses this decoder
	 * and may outlive its attachment to the region, so hold a reference.
	 */
	if (cxlr->mode == CXL_PARTMODE_DYNAMIC_RAM)
		cxlr_dax->cxled = p->targets[0];
	if (cxlr_dax->cxled)
		get_device(&cxlr_dax->cxled->cxld.dev);

	dev = &cxlr_dax->dev;
	cxlr_dax->cxlr = cxlr;
	cxlr->cxlr_dax = cxlr_dax;
	device_initialize(dev);
	lockdep_set_class(&dev->mutex, &cxl_dax_region_key);
	device_set_pm_not_required(dev);
	dev->parent = &cxlr->dev;
	dev->bus = &cxl_bus_type;
	dev->type = &cxl_dax_region_type;

	/* Parent of the untagged extents, see cxl_dax_untagged_add() */
	dev = &cxlr_dax->untagged;
	device_initialize(dev);
	device_set_pm_not_required(dev);
	dev->parent = &cxlr_dax->dev;
	dev->type = &cxl_dax_untagged_type;
	get_device(&cxlr_dax->dev);

	return cxlr_dax;
}

/*
 * Clearing cxlr->cxlr_dax here cannot race an Add or Release event.
 * Event paths find cxlr_dax through cxl_dpa_to_region() and use it with
 * cxl_rwsem.region held for read, from resolving an extent through
 * onlining its group.  Taking the write lock waits those paths out, and
 * later lookups find NULL, so the teardown that follows sees no events.
 */
static void cxlr_dax_unpublish(struct cxl_dax_region *cxlr_dax)
{
	guard(rwsem_write)(&cxl_rwsem.region);
	cxlr_dax->cxlr->cxlr_dax = NULL;
}

static void cxlr_dax_unregister(void *_cxlr_dax)
{
	struct cxl_dax_region *cxlr_dax = _cxlr_dax;

	cxlr_dax_unpublish(cxlr_dax);
	/* Unbind first: the driver's teardown empties untagged and the groups */
	device_release_driver(&cxlr_dax->dev);
	if (device_is_registered(&cxlr_dax->untagged))
		device_del(&cxlr_dax->untagged);
	put_device(&cxlr_dax->untagged);
	device_unregister(&cxlr_dax->dev);
}

/*
 * The dax_region's driver may probe, and its extent scan run, before
 * devm_cxl_add_dax_region() returns, so the untagged device is registered
 * on first use instead.  Callers are serialized by the memdev's
 * dc_chain.lock; a DC region has a single memdev behind it.
 */
int cxl_dax_untagged_add(struct cxl_dax_region *cxlr_dax)
{
	struct device *dev = &cxlr_dax->untagged;
	int rc;

	if (device_is_registered(dev))
		return 0;

	rc = dev_set_name(dev, "untagged");
	if (rc)
		return rc;
	return device_add(dev);
}

DEFINE_FREE(unpublish_cxl_dax_region, struct cxl_dax_region *,
	if (!IS_ERR_OR_NULL(_T)) {
		cxlr_dax_unpublish(_T);
		put_device(&_T->untagged);
		put_device(&_T->dev);
	})

/*
 * Runs from cxl_dax_region_probe().  The region's parameters may not be
 * read here: a concurrent detach clears p->targets[] under cxl_rwsem.region
 * before it waits for this probe.  Use the decoder recorded at allocation
 * instead; cxlr_dax holds a reference on it.
 */
int cxl_region_add_existing_extents(struct cxl_dax_region *cxlr_dax)
{
	struct cxl_endpoint_decoder *cxled = cxlr_dax->cxled;
	int rc;

	rc = cxl_process_extent_list(cxled);
	if (rc) {
		dev_err(&cxled->cxld.dev, "Existing extent processing failed %d\n",
			rc);
		return rc;
	}

	/* Pairs with the acquire in cxl_validate_extent(); adds may proceed */
	smp_store_release(&cxlr_dax->extents_scanned, true);
	return 0;
}
EXPORT_SYMBOL_NS_GPL(cxl_region_add_existing_extents, "CXL");

/*
 * Driver unbind returns the held extents to the device (devres), so refuse
 * new adds until the next probe has read the device's list again.
 */
void cxl_region_reset_extent_scan(struct cxl_dax_region *cxlr_dax)
{
	/* Adds test the gate under cxl_rwsem.region; none is mid-flight after this */
	guard(rwsem_write)(&cxl_rwsem.region);
	cxlr_dax->extents_scanned = false;
}
EXPORT_SYMBOL_NS_GPL(cxl_region_reset_extent_scan, "CXL");

int devm_cxl_add_dax_region(struct cxl_region *cxlr)
{
	struct device *dev;
	int rc;

	if (cxlr->mode == CXL_PARTMODE_DYNAMIC_RAM &&
	    cxlr->params.interleave_ways != 1) {
		dev_dbg(&cxlr->dev, "Interleaving DC not supported\n");
		return -EINVAL;
	}

	struct cxl_dax_region *cxlr_dax __free(unpublish_cxl_dax_region) =
		cxl_dax_region_alloc(cxlr);
	if (IS_ERR(cxlr_dax))
		return PTR_ERR(cxlr_dax);

	dev = &cxlr_dax->dev;
	rc = dev_set_name(dev, "dax_region%d", cxlr->id);
	if (rc)
		return rc;

	rc = device_add(dev);
	if (rc)
		return rc;

	dev_dbg(&cxlr->dev, "%s: register %s\n", dev_name(dev->parent),
		dev_name(dev));

	return devm_add_action_or_reset(&cxlr->dev, cxlr_dax_unregister,
					no_free_ptr(cxlr_dax));
}
