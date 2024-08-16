// SPDX-License-Identifier: GPL-2.0
/*  Copyright(c) 2024 Intel Corporation. All rights reserved. */

#include <linux/device.h>
#include <cxl.h>

#include "core.h"

static ssize_t offset_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct region_extent *region_extent = to_region_extent(dev);

	return sysfs_emit(buf, "%#llx\n", region_extent->hpa_range.start);
}
static DEVICE_ATTR_RO(offset);

static ssize_t length_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct region_extent *region_extent = to_region_extent(dev);
	u64 length = range_len(&region_extent->hpa_range);

	return sysfs_emit(buf, "%#llx\n", length);
}
static DEVICE_ATTR_RO(length);

static ssize_t tag_show(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	struct region_extent *region_extent = to_region_extent(dev);

	return sysfs_emit(buf, "%pUb\n", &region_extent->tag);
}
static DEVICE_ATTR_RO(tag);

static struct attribute *region_extent_attrs[] = {
	&dev_attr_offset.attr,
	&dev_attr_length.attr,
	&dev_attr_tag.attr,
	NULL,
};

static uuid_t empty_tag = { 0 };

static umode_t region_extent_visible(struct kobject *kobj,
				     struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct region_extent *region_extent = to_region_extent(dev);

	if (a == &dev_attr_tag.attr &&
	    uuid_equal(&region_extent->tag, &empty_tag))
		return 0;

	return a->mode;
}

static const struct attribute_group region_extent_attribute_group = {
	.attrs = region_extent_attrs,
	.is_visible = region_extent_visible,
};

__ATTRIBUTE_GROUPS(region_extent_attribute);

static void cxled_release_extent(struct cxl_endpoint_decoder *cxled,
				 struct cxled_extent *ed_extent)
{
	struct cxl_memdev_state *mds = cxled_to_mds(cxled);
	struct device *dev = &cxled->cxld.dev;

	dev_dbg(dev, "Remove extent %par (%*phC)\n", &ed_extent->dpa_range,
		CXL_EXTENT_TAG_LEN, ed_extent->tag);
	memdev_release_extent(mds, &ed_extent->dpa_range);
	kfree(ed_extent);
}

static void free_region_extent(struct region_extent *region_extent)
{
	struct cxled_extent *ed_extent;
	unsigned long index;

	/*
	 * Remove from each endpoint decoder the extent which backs this region
	 * extent
	 */
	xa_for_each(&region_extent->decoder_extents, index, ed_extent)
		cxled_release_extent(ed_extent->cxled, ed_extent);
	xa_destroy(&region_extent->decoder_extents);
	ida_free(&region_extent->cxlr_dax->extent_ida, region_extent->dev.id);
	kfree(region_extent);
}

static void region_extent_release(struct device *dev)
{
	struct region_extent *region_extent = to_region_extent(dev);

	free_region_extent(region_extent);
}

static const struct device_type region_extent_type = {
	.name = "extent",
	.release = region_extent_release,
	.groups = region_extent_attribute_groups,
};

bool is_region_extent(struct device *dev)
{
	return dev->type == &region_extent_type;
}
EXPORT_SYMBOL_NS_GPL(is_region_extent, CXL);

static void region_extent_unregister(void *ext)
{
	struct region_extent *region_extent = ext;

	dev_dbg(&region_extent->dev, "DAX region rm extent HPA %par\n",
		&region_extent->hpa_range);
	device_unregister(&region_extent->dev);
}

static void region_rm_extent(struct region_extent *region_extent)
{
	struct device *region_dev = region_extent->dev.parent;

	devm_release_action(region_dev, region_extent_unregister, region_extent);
}

static struct region_extent *
alloc_region_extent(struct cxl_dax_region *cxlr_dax, struct range *hpa_range, u8 *tag)
{
	int id;

	struct region_extent *region_extent __free(kfree) =
				kzalloc(sizeof(*region_extent), GFP_KERNEL);
	if (!region_extent)
		return ERR_PTR(-ENOMEM);

	id = ida_alloc(&cxlr_dax->extent_ida, GFP_KERNEL);
	if (id < 0)
		return ERR_PTR(-ENOMEM);

	region_extent->hpa_range = *hpa_range;
	region_extent->cxlr_dax = cxlr_dax;
	import_uuid(&region_extent->tag, tag);
	region_extent->dev.id = id;
	xa_init(&region_extent->decoder_extents);
	return no_free_ptr(region_extent);
}

static int online_region_extent(struct region_extent *region_extent)
{
	struct cxl_dax_region *cxlr_dax = region_extent->cxlr_dax;
	struct device *dev;
	int rc;

	dev = &region_extent->dev;
	device_initialize(dev);
	device_set_pm_not_required(dev);
	dev->parent = &cxlr_dax->dev;
	dev->type = &region_extent_type;
	rc = dev_set_name(dev, "extent%d.%d", cxlr_dax->cxlr->id, dev->id);
	if (rc)
		goto err;

	rc = device_add(dev);
	if (rc)
		goto err;

	dev_dbg(dev, "region extent HPA %par\n", &region_extent->hpa_range);
	return devm_add_action_or_reset(&cxlr_dax->dev, region_extent_unregister,
					region_extent);

err:
	dev_err(&cxlr_dax->dev, "Failed to initialize region extent HPA %par\n",
		&region_extent->hpa_range);

	put_device(dev);
	return rc;
}

struct match_data {
	struct cxl_endpoint_decoder *cxled;
	struct range *new_range;
};

static int match_contains(struct device *dev, void *data)
{
	struct region_extent *region_extent = to_region_extent(dev);
	struct match_data *md = data;
	struct cxled_extent *entry;
	unsigned long index;

	if (!region_extent)
		return 0;

	xa_for_each(&region_extent->decoder_extents, index, entry) {
		if (md->cxled == entry->cxled &&
		    range_contains(&entry->dpa_range, md->new_range))
			return true;
	}
	return false;
}

static bool extents_contain(struct cxl_dax_region *cxlr_dax,
			    struct cxl_endpoint_decoder *cxled,
			    struct range *new_range)
{
	struct device *extent_device;
	struct match_data md = {
		.cxled = cxled,
		.new_range = new_range,
	};

	extent_device = device_find_child(&cxlr_dax->dev, &md, match_contains);
	if (!extent_device)
		return false;

	put_device(extent_device);
	return true;
}

static int match_overlaps(struct device *dev, void *data)
{
	struct region_extent *region_extent = to_region_extent(dev);
	struct match_data *md = data;
	struct cxled_extent *entry;
	unsigned long index;

	if (!region_extent)
		return 0;

	xa_for_each(&region_extent->decoder_extents, index, entry) {
		if (md->cxled == entry->cxled &&
		    range_overlaps(&entry->dpa_range, md->new_range))
			return true;
	}

	return false;
}

static bool extents_overlap(struct cxl_dax_region *cxlr_dax,
			    struct cxl_endpoint_decoder *cxled,
			    struct range *new_range)
{
	struct device *extent_device;
	struct match_data md = {
		.cxled = cxled,
		.new_range = new_range,
	};

	extent_device = device_find_child(&cxlr_dax->dev, &md, match_overlaps);
	if (!extent_device)
		return false;

	put_device(extent_device);
	return true;
}

static void calc_hpa_range(struct cxl_endpoint_decoder *cxled,
			   struct cxl_dax_region *cxlr_dax,
			   struct range *dpa_range,
			   struct range *hpa_range)
{
	resource_size_t dpa_offset, hpa;

	dpa_offset = dpa_range->start - cxled->dpa_res->start;
	hpa = cxled->cxld.hpa_range.start + dpa_offset;

	hpa_range->start = hpa - cxlr_dax->hpa_range.start;
	hpa_range->end = hpa_range->start + range_len(dpa_range) - 1;
}

static int cxlr_notify_extent(struct cxl_region *cxlr, enum dc_event event,
			      struct region_extent *region_extent)
{
	struct cxl_dax_region *cxlr_dax;
	struct device *dev;
	int rc = 0;

	cxlr_dax = cxlr->cxlr_dax;
	dev = &cxlr_dax->dev;
	dev_dbg(dev, "Trying notify: type %d HPA %par\n",
		event, &region_extent->hpa_range);

	/*
	 * NOTE the lack of a driver indicates a notification has failed.  No
	 * user space coordiantion was possible.
	 */
	device_lock(dev);
	if (dev->driver) {
		struct cxl_driver *driver = to_cxl_drv(dev->driver);
		struct cxl_notify_data notify_data = (struct cxl_notify_data) {
			.event = event,
			.region_extent = region_extent,
		};

		if (driver->notify) {
			dev_dbg(dev, "Notify: type %d HPA %par\n",
				event, &region_extent->hpa_range);
			rc = driver->notify(dev, &notify_data);
		}
	}
	device_unlock(dev);
	return rc;
}

struct rm_data {
	struct cxl_region *cxlr;
	struct range *range;
};

static int cxlr_rm_extent(struct device *dev, void *data)
{
	struct region_extent *region_extent = to_region_extent(dev);
	struct rm_data *rm_data = data;
	int rc;

	if (!region_extent)
		return 0;

	/*
	 * Any extent which 'touches' the released range is attempted to be
	 * removed.
	 */
	if (range_overlaps(rm_data->range, &region_extent->hpa_range)) {
		struct cxl_region *cxlr = rm_data->cxlr;

		dev_dbg(dev, "Remove region extent HPA %par\n",
			&region_extent->hpa_range);
		rc = cxlr_notify_extent(cxlr, DCD_RELEASE_CAPACITY, region_extent);
		if (rc == -EBUSY)
			return 0;
		/* Extent not in use or error, remove it */
		region_rm_extent(region_extent);
	}
	return 0;
}

int cxl_rm_extent(struct cxl_memdev_state *mds, struct cxl_extent *extent)
{
	u64 start_dpa = le64_to_cpu(extent->start_dpa);
	struct cxl_memdev *cxlmd = mds->cxlds.cxlmd;
	struct cxl_endpoint_decoder *cxled;
	struct range hpa_range, dpa_range;
	struct cxl_region *cxlr;

	dpa_range = (struct range) {
		.start = start_dpa,
		.end = start_dpa + le64_to_cpu(extent->length) - 1,
	};

	guard(rwsem_read)(&cxl_region_rwsem);
	cxlr = cxl_dpa_to_region(cxlmd, start_dpa, &cxled);
	if (!cxlr) {
		memdev_release_extent(mds, &dpa_range);
		return -ENXIO;
	}

	calc_hpa_range(cxled, cxlr->cxlr_dax, &dpa_range, &hpa_range);

	struct rm_data rm_data = {
		.cxlr = cxlr,
		.range = &hpa_range,
	};

	/* Remove region extents which overlap */
	return device_for_each_child(&cxlr->cxlr_dax->dev, &rm_data,
				     cxlr_rm_extent);
}

static int cxlr_add_extent(struct cxl_dax_region *cxlr_dax,
			   struct cxl_endpoint_decoder *cxled,
			   struct cxled_extent *ed_extent)
{
	struct region_extent *region_extent;
	struct range hpa_range;
	int rc;

	calc_hpa_range(cxled, cxlr_dax, &ed_extent->dpa_range, &hpa_range);

	region_extent = alloc_region_extent(cxlr_dax, &hpa_range, ed_extent->tag);
	if (IS_ERR(region_extent))
		return PTR_ERR(region_extent);

	rc = xa_insert(&region_extent->decoder_extents, (unsigned long)ed_extent, ed_extent,
		       GFP_KERNEL);
	if (rc) {
		free_region_extent(region_extent);
		return rc;
	}

	rc = online_region_extent(region_extent);
	/* device model handled freeing region_extent */
	if (rc)
		return rc;

	rc = cxlr_notify_extent(cxlr_dax->cxlr, DCD_ADD_CAPACITY, region_extent);
	/*
	 * The region device was breifly live but DAX layer ensures it was not
	 * used
	 */
	if (rc)
		region_rm_extent(region_extent);

	return rc;
}

/* Callers are expected to ensure cxled has been attached to a region */
int cxl_add_extent(struct cxl_memdev_state *mds, struct cxl_extent *extent)
{
	u64 start_dpa = le64_to_cpu(extent->start_dpa);
	struct cxl_memdev *cxlmd = mds->cxlds.cxlmd;
	struct cxl_endpoint_decoder *cxled;
	struct range ed_range, ext_range;
	struct cxl_dax_region *cxlr_dax;
	struct cxled_extent *ed_extent;
	struct cxl_region *cxlr;
	struct device *dev;

	ext_range = (struct range) {
		.start = start_dpa,
		.end = start_dpa + le64_to_cpu(extent->length) - 1,
	};

	guard(rwsem_read)(&cxl_region_rwsem);
	cxlr = cxl_dpa_to_region(cxlmd, start_dpa, &cxled);
	if (!cxlr)
		return -ENXIO;

	cxlr_dax = cxled->cxld.region->cxlr_dax;
	dev = &cxled->cxld.dev;
	ed_range = (struct range) {
		.start = cxled->dpa_res->start,
		.end = cxled->dpa_res->end,
	};

	dev_dbg(&cxled->cxld.dev, "Checking ED (%pr) for extent %par\n",
		cxled->dpa_res, &ext_range);

	if (!range_contains(&ed_range, &ext_range)) {
		dev_err_ratelimited(dev,
				    "DC extent DPA %par (%*phC) is not fully in ED %par\n",
				    &ext_range.start, CXL_EXTENT_TAG_LEN,
				    extent->tag, &ed_range);
		return -ENXIO;
	}

	if (extents_contain(cxlr_dax, cxled, &ext_range))
		return 0;

	if (extents_overlap(cxlr_dax, cxled, &ext_range))
		return -ENXIO;

	ed_extent = kzalloc(sizeof(*ed_extent), GFP_KERNEL);
	if (!ed_extent)
		return -ENOMEM;

	ed_extent->cxled = cxled;
	ed_extent->dpa_range = ext_range;
	memcpy(ed_extent->tag, extent->tag, CXL_EXTENT_TAG_LEN);

	dev_dbg(dev, "Add extent %par (%*phC)\n", &ed_extent->dpa_range,
		CXL_EXTENT_TAG_LEN, ed_extent->tag);

	return cxlr_add_extent(cxlr_dax, cxled, ed_extent);
}
