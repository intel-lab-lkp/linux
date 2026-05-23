// SPDX-License-Identifier: GPL-2.0
/*  Copyright(c) 2024 Intel Corporation. All rights reserved. */

#include <linux/device.h>
#include <cxl.h>

#include "core.h"


static void cxled_release_extent(struct cxl_endpoint_decoder *cxled,
				 struct dc_extent *dc_extent)
{
	struct cxl_memdev_state *mds = cxled_to_mds(cxled);
	struct device *dev = &cxled->cxld.dev;

	dev_dbg(dev, "Remove extent %pra (%pU)\n",
		&dc_extent->dpa_range, &dc_extent->uuid);
	memdev_release_extent(mds, &dc_extent->dpa_range);
}

static void free_tag_group(struct cxl_dc_tag_group *group)
{
	xa_destroy(&group->dc_extents);
	kfree(group);
}

static void dc_extent_release(struct device *dev)
{
	struct dc_extent *dc_extent = to_dc_extent(dev);
	struct cxl_dc_tag_group *group = dc_extent->group;

	cxled_release_extent(dc_extent->cxled, dc_extent);
	xa_erase(&group->cxlr_dax->dc_extents, dc_extent->dev.id);
	xa_erase(&group->dc_extents, dc_extent->seq_num);
	group->nr_extents--;
	if (!group->nr_extents)
		free_tag_group(group);
	kfree(dc_extent);
}

static const struct device_type dc_extent_type = {
	.name = "extent",
	.release = dc_extent_release,
};

bool is_dc_extent(struct device *dev)
{
	return dev->type == &dc_extent_type;
}
EXPORT_SYMBOL_NS_GPL(is_dc_extent, "CXL");

static struct cxl_dc_tag_group *
alloc_tag_group(struct cxl_dax_region *cxlr_dax, uuid_t *uuid)
{
	struct cxl_dc_tag_group *group __free(kfree) =
				kzalloc(sizeof(*group), GFP_KERNEL);
	if (!group)
		return ERR_PTR(-ENOMEM);

	group->cxlr_dax = cxlr_dax;
	uuid_copy(&group->uuid, uuid);
	xa_init(&group->dc_extents);
	return no_free_ptr(group);
}

/*
 * Stage 1 of the add pipeline: pure, no allocation.  Resolve the extent
 * to its region/endpoint decoder and ext_range, and verify the range
 * fits in the resolved endpoint decoder's DPA resource.  Further
 * per-extent invariants layer into this function in subsequent commits.
 *
 * Caller must hold cxl_rwsem.region for read (cxl_dpa_to_region()).
 * On success, @out_cxled / @out_cxlr_dax / @out_ext_range carry the
 * resolved handles consumed by the rest of the pipeline.
 */
static int cxl_validate_extent(struct cxl_memdev_state *mds,
			       struct cxl_extent *extent,
			       struct cxl_endpoint_decoder **out_cxled,
			       struct cxl_dax_region **out_cxlr_dax,
			       struct range *out_ext_range)
{
	u64 start_dpa = le64_to_cpu(extent->start_dpa);
	struct cxl_memdev *cxlmd = mds->cxlds.cxlmd;
	struct cxl_endpoint_decoder *cxled;
	struct cxl_region *cxlr;
	struct range ext_range = (struct range) {
		.start = start_dpa,
		.end = start_dpa + le64_to_cpu(extent->length) - 1,
	};
	struct range ed_range;

	cxlr = cxl_dpa_to_region(cxlmd, start_dpa, &cxled);
	if (!cxlr)
		return -ENXIO;

	ed_range = (struct range) {
		.start = cxled->dpa_res->start,
		.end = cxled->dpa_res->end,
	};
	if (!range_contains(&ed_range, &ext_range)) {
		dev_err_ratelimited(&cxled->cxld.dev,
				    "DC extent DPA %pra (%pU) is not fully in ED %pra\n",
				    &ext_range, extent->uuid, &ed_range);
		return -ENXIO;
	}

	*out_cxled = cxled;
	*out_cxlr_dax = cxlr->cxlr_dax;
	*out_ext_range = ext_range;
	return 0;
}

enum cxl_extent_class {
	CXL_EXT_NEW,
	CXL_EXT_DUPLICATE,
	CXL_EXT_OVERLAP,
};

/*
 * Stage 2: classify @ext_range against extents already accepted on this
 * cxlr_dax+cxled.  Walks cxlr_dax->dc_extents once: a stored extent that
 * fully contains @ext_range means a duplicate accept (idempotent, fine);
 * a stored extent that only overlaps means an inconsistent offer.
 */
static enum cxl_extent_class
cxlr_dax_classify_extent(struct cxl_dax_region *cxlr_dax,
			 struct cxl_endpoint_decoder *cxled,
			 const struct range *ext_range)
{
	struct dc_extent *entry;
	unsigned long i;

	xa_for_each(&cxlr_dax->dc_extents, i, entry) {
		if (entry->cxled != cxled)
			continue;
		if (range_contains(&entry->dpa_range, ext_range))
			return CXL_EXT_DUPLICATE;
		if (range_overlaps(&entry->dpa_range, ext_range))
			return CXL_EXT_OVERLAP;
	}
	return CXL_EXT_NEW;
}

/*
 * Stage 3: allocate and populate a dc_extent for an already-validated,
 * already-classified-as-new @ext_range.  Only -ENOMEM can fail here.
 */
static struct dc_extent *
dc_extent_build(struct cxl_endpoint_decoder *cxled,
		struct cxl_dax_region *cxlr_dax,
		struct cxl_extent *extent,
		const struct range *ext_range, u16 seq_num)
{
	resource_size_t dpa_offset = ext_range->start - cxled->dpa_res->start;
	resource_size_t hpa = cxled->cxld.hpa_range.start + dpa_offset;
	struct dc_extent *dc_extent;

	dc_extent = kzalloc(sizeof(*dc_extent), GFP_KERNEL);
	if (!dc_extent)
		return ERR_PTR(-ENOMEM);

	dc_extent->cxled = cxled;
	dc_extent->dpa_range = *ext_range;
	dc_extent->hpa_range.start = hpa - cxlr_dax->hpa_range.start;
	dc_extent->hpa_range.end = dc_extent->hpa_range.start +
				   range_len(ext_range) - 1;
	dc_extent->seq_num = seq_num;
	import_uuid(&dc_extent->uuid, extent->uuid);
	return dc_extent;
}

/*
 * Stage 4: insert @dc_extent into the pending tag group.  All extents in
 * one More-chain group share a UUID — enforced here as the group is
 * either being created (first extent) or appended to.  On any failure
 * the dc_extent is freed.
 */
static int cxlr_add_extent(struct cxl_memdev_state *mds,
			   struct cxl_dax_region *cxlr_dax,
			   struct dc_extent *dc_extent)
{
	struct cxl_dc_tag_group **group = &mds->add_ctx.group;
	int rc;

	if (*group && !uuid_equal(&(*group)->uuid, &dc_extent->uuid)) {
		kfree(dc_extent);
		return -EINVAL;
	}

	if (!*group) {
		dev_dbg(&cxlr_dax->dev, "Alloc new tag group\n");
		*group = alloc_tag_group(cxlr_dax, &dc_extent->uuid);
		if (IS_ERR(*group)) {
			rc = PTR_ERR(*group);
			*group = NULL;
			kfree(dc_extent);
			return rc;
		}
	} else {
		dev_dbg(&cxlr_dax->dev, "Append dc_extent to tag group\n");
	}

	dc_extent->group = *group;

	/*
	 * Key by @seq_num so iteration order equals assembly order, in both
	 * the sharable case (device-stamped 1..n) and the non-sharable case
	 * (host-assigned arrival-order 1..n).  A collision here signals a
	 * cxl-side validation gap.
	 */
	rc = xa_insert(&(*group)->dc_extents, dc_extent->seq_num,
		       dc_extent, GFP_KERNEL);
	if (rc) {
		dev_WARN_ONCE(&cxlr_dax->dev, rc == -EBUSY,
			"duplicate seq_num %u in tag %pUb\n",
			dc_extent->seq_num, &dc_extent->uuid);
		kfree(dc_extent);
		return rc;
	}

	return 0;
}

int cxl_add_extent(struct cxl_memdev_state *mds, struct cxl_extent *extent,
		   u16 seq_num)
{
	struct cxl_endpoint_decoder *cxled;
	struct cxl_dax_region *cxlr_dax;
	struct dc_extent *dc_extent;
	struct range ext_range;
	int rc;

	guard(rwsem_read)(&cxl_rwsem.region);

	rc = cxl_validate_extent(mds, extent, &cxled, &cxlr_dax, &ext_range);
	if (rc)
		return rc;

	switch (cxlr_dax_classify_extent(cxlr_dax, cxled, &ext_range)) {
	case CXL_EXT_DUPLICATE:
		/*
		 * Idempotent accept simplifies the dax-side scan for existing
		 * extents on region creation; reply success without duplicating.
		 */
		dev_warn_ratelimited(&cxled->cxld.dev,
				     "Extent %pra exists; accept again\n",
				     &ext_range);
		return 0;
	case CXL_EXT_OVERLAP:
		return -ENXIO;
	case CXL_EXT_NEW:
		break;
	}

	dc_extent = dc_extent_build(cxled, cxlr_dax, extent, &ext_range,
				    seq_num);
	if (IS_ERR(dc_extent))
		return PTR_ERR(dc_extent);

	dev_dbg(&cxled->cxld.dev, "Add extent %pra (%pU)\n",
		&dc_extent->dpa_range, &dc_extent->uuid);

	return cxlr_add_extent(mds, cxlr_dax, dc_extent);
}

static void dc_extent_unregister(void *ext)
{
	struct dc_extent *dc_extent = ext;

	dev_dbg(&dc_extent->dev, "DAX region rm extent HPA %pra\n",
		&dc_extent->hpa_range);
	device_unregister(&dc_extent->dev);
}

static void cleanup_pending_dc_extent(struct dc_extent *dc_extent)
{
	struct cxl_dc_tag_group *group = dc_extent->group;

	cxled_release_extent(dc_extent->cxled, dc_extent);
	xa_erase(&group->dc_extents, dc_extent->seq_num);
	group->nr_extents--;
	if (!group->nr_extents)
		free_tag_group(group);
	kfree(dc_extent);
}

int online_tag_group(struct cxl_dc_tag_group *group)
{
	struct cxl_dax_region *cxlr_dax = group->cxlr_dax;
	struct dc_extent *dc_extent;
	unsigned long index;
	int rc = 0;

	/*
	 * Seed nr_extents with the full group size plus a +1 pin held by
	 * this function.  The size counts every dc_extent that might
	 * decrement nr_extents on cleanup; the pin keeps @group alive
	 * across the body even if every dc_extent release fires inside
	 * the loop (e.g. devm_add_action_or_reset failure on the only
	 * pending extent).  The pin is dropped at the end of the function.
	 */
	xa_for_each(&group->dc_extents, index, dc_extent)
		group->nr_extents++;
	group->nr_extents++;

	xa_for_each(&group->dc_extents, index, dc_extent) {
		struct device *dev = &dc_extent->dev;
		u32 id;

		device_initialize(dev);
		device_set_pm_not_required(dev);
		dev->parent = &cxlr_dax->dev;
		dev->type = &dc_extent_type;

		rc = xa_alloc(&cxlr_dax->dc_extents, &id, dc_extent,
			      xa_limit_32b, GFP_KERNEL);
		if (rc < 0) {
			put_device(dev);
			break;
		}
		dev->id = id;

		rc = dev_set_name(dev, "extent%d.%d", cxlr_dax->cxlr->id,
				  dev->id);
		if (rc) {
			xa_erase(&cxlr_dax->dc_extents, dev->id);
			put_device(dev);
			break;
		}

		rc = device_add(dev);
		if (rc) {
			xa_erase(&cxlr_dax->dc_extents, dev->id);
			put_device(dev);
			break;
		}

		dev_dbg(dev, "dc_extent HPA %pra (%pU)\n",
			&dc_extent->hpa_range, &group->uuid);

		rc = devm_add_action_or_reset(&cxlr_dax->dev,
					      dc_extent_unregister, dc_extent);
		if (rc)
			break;
	}

	if (rc) {
		/*
		 * Unwind every remaining dc_extent in the group.  The pin
		 * above keeps @group alive across this walk.  Distinguish
		 * onlined dc_extents (have a devm action) from pending ones
		 * via devm_remove_action_nowarn(): a 0 return means the
		 * action was installed and is now consumed, so we run the
		 * unregister ourselves; -ENOENT means pending.
		 */
		xa_for_each(&group->dc_extents, index, dc_extent) {
			int r = devm_remove_action_nowarn(&cxlr_dax->dev,
							  dc_extent_unregister,
							  dc_extent);
			if (r == 0)
				dc_extent_unregister(dc_extent);
			else
				cleanup_pending_dc_extent(dc_extent);
		}
	}

	/* Drop the pin; if nothing else still references @group, free it. */
	group->nr_extents--;
	if (!group->nr_extents)
		free_tag_group(group);
	return rc;
}
