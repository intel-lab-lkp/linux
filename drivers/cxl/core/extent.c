// SPDX-License-Identifier: GPL-2.0
/*  Copyright(c) 2024 Intel Corporation. All rights reserved. */

#include <linux/device.h>
#include <cxl.h>

#include "core.h"

static ssize_t offset_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct dc_extent *dc_extent = to_dc_extent(dev);

	return sysfs_emit(buf, "%#llx\n", dc_extent->hpa_range.start);
}
static DEVICE_ATTR_RO(offset);

static ssize_t length_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct dc_extent *dc_extent = to_dc_extent(dev);
	u64 length = range_len(&dc_extent->hpa_range);

	return sysfs_emit(buf, "%#llx\n", length);
}
static DEVICE_ATTR_RO(length);

static ssize_t uuid_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct dc_extent *dc_extent = to_dc_extent(dev);

	return sysfs_emit(buf, "%pUb\n", &dc_extent->group->uuid);
}
static DEVICE_ATTR_RO(uuid);

static struct attribute *dc_extent_attrs[] = {
	&dev_attr_offset.attr,
	&dev_attr_length.attr,
	&dev_attr_uuid.attr,
	NULL
};

static uuid_t empty_uuid = { 0 };

static umode_t dc_extent_visible(struct kobject *kobj,
				 struct attribute *a, int n)
{
	struct device *dev = kobj_to_dev(kobj);
	struct dc_extent *dc_extent = to_dc_extent(dev);

	if (a == &dev_attr_uuid.attr &&
	    uuid_equal(&dc_extent->group->uuid, &empty_uuid))
		return 0;

	return a->mode;
}

static const struct attribute_group dc_extent_attribute_group = {
	.attrs = dc_extent_attrs,
	.is_visible = dc_extent_visible,
};

__ATTRIBUTE_GROUPS(dc_extent_attribute);


static void cxled_release_extent(struct cxl_endpoint_decoder *cxled,
				 struct dc_extent *dc_extent)
{
	struct cxl_memdev_state *mds = cxled_to_mds(cxled);
	struct device *dev = &cxled->cxld.dev;

	dev_dbg(dev, "Remove extent %pra (%pU)\n",
		&dc_extent->dpa_range, &dc_extent->uuid);
	memdev_release_extent(mds, &dc_extent->dpa_range);
}

/*
 * Host-wide registry of live tag groups with non-null uuids.  Enforces
 * that within this host, a tag uuid identifies exactly one allocation
 * across all regions and memdevs — closing the gap left by the
 * per-region scans in cxlr_add_extent() and uuid_claim_tagged().  The
 * orchestrator (FM) owns tag-uuid allocation per spec; this is a
 * defense against firmware bugs and orchestrator misbehavior.  Untagged
 * (null uuid) allocations are not tracked: the spec defines no
 * cross-chain identity for them.
 */
static DEFINE_MUTEX(cxl_tag_lock);
static LIST_HEAD(cxl_tag_groups);

static int cxl_tag_register(struct cxl_dc_tag_group *grp)
{
	struct cxl_dc_tag_group *g;

	if (uuid_is_null(&grp->uuid))
		return 0;

	guard(mutex)(&cxl_tag_lock);
	list_for_each_entry(g, &cxl_tag_groups, registry_node)
		if (uuid_equal(&g->uuid, &grp->uuid))
			return -EBUSY;
	list_add_tail(&grp->registry_node, &cxl_tag_groups);
	return 0;
}

static void cxl_tag_unregister(struct cxl_dc_tag_group *grp)
{
	if (uuid_is_null(&grp->uuid))
		return;

	guard(mutex)(&cxl_tag_lock);
	list_del(&grp->registry_node);
}

bool cxl_tag_already_committed(const uuid_t *tag)
{
	struct cxl_dc_tag_group *g;

	if (uuid_is_null(tag))
		return false;

	guard(mutex)(&cxl_tag_lock);
	list_for_each_entry(g, &cxl_tag_groups, registry_node)
		if (uuid_equal(&g->uuid, tag))
			return true;
	return false;
}

static void free_tag_group(struct cxl_dc_tag_group *group)
{
	cxl_tag_unregister(group);
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
	.groups = dc_extent_attribute_groups,
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
	int rc;

	if (!group)
		return ERR_PTR(-ENOMEM);

	group->cxlr_dax = cxlr_dax;
	uuid_copy(&group->uuid, uuid);
	xa_init(&group->dc_extents);
	INIT_LIST_HEAD(&group->registry_node);

	rc = cxl_tag_register(group);
	if (rc)
		return ERR_PTR(rc);

	return no_free_ptr(group);
}

/*
 * Find the DC (Dynamic Capacity) partition that fully contains @ext_range,
 * or NULL if the extent falls outside every DC partition on this memdev.
 * The returned pointer is owned by mds->cxlds.part[] and lives for the
 * lifetime of the memdev.
 */
const struct cxl_dpa_partition *
cxl_extent_dc_partition(struct cxl_memdev_state *mds,
			struct cxl_extent *extent,
			struct range *ext_range)
{
	struct cxl_dev_state *cxlds = &mds->cxlds;
	struct device *dev = mds->cxlds.dev;

	for (int i = 0; i < cxlds->nr_partitions; i++) {
		struct cxl_dpa_partition *part = &cxlds->part[i];
		struct range partition_range = {
			.start = part->res.start,
			.end = part->res.end,
		};

		if (part->mode != CXL_PARTMODE_DYNAMIC_RAM_A)
			continue;

		if (range_contains(&partition_range, ext_range)) {
			dev_dbg(dev, "DC extent DPA %pra (DCR:%pra)(%pU)\n",
				ext_range, &partition_range, extent->uuid);
			return part;
		}
	}

	dev_err_ratelimited(dev,
			    "DC extent DPA %pra (%pU) is not in a valid DC partition\n",
			    ext_range, extent->uuid);
	return NULL;
}

/*
 * Stage 1 of the add pipeline: pure, no allocation.  Resolve the extent
 * to its region/endpoint decoder and ext_range, and enforce every
 * per-extent invariant the device must satisfy:
 *
 *   - DPA falls inside a Dynamic Capacity partition (cxl_extent_dc_partition).
 *   - CDAT-sharability rules:
 *       sharable:     tag must be non-null AND shared_extn_seq != 0
 *       non-sharable: shared_extn_seq must be 0  (tag is optional)
 *     Any cross-mixing is a device firmware bug.
 *   - DPA resolves to an endpoint decoder attached to a region.
 *   - The extent's range is fully contained in that ED's DPA resource.
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
	struct device *dev = mds->cxlds.dev;
	uuid_t *uuid = (uuid_t *)extent->uuid;
	u16 seq = le16_to_cpu(extent->shared_extn_seq);
	const struct cxl_dpa_partition *part;
	struct cxl_endpoint_decoder *cxled;
	struct cxl_region *cxlr;
	struct range ext_range = (struct range) {
		.start = start_dpa,
		.end = start_dpa + le64_to_cpu(extent->length) - 1,
	};
	struct range ed_range;

	part = cxl_extent_dc_partition(mds, extent, &ext_range);
	if (!part)
		return -ENXIO;

	if (part->perf.shareable) {
		if (uuid_is_null(uuid)) {
			dev_err_ratelimited(dev,
				"DC extent DPA %pra: sharable-partition extent has null tag (firmware bug)\n",
				&ext_range);
			return -ENXIO;
		}
		if (seq == 0) {
			dev_err_ratelimited(dev,
				"DC extent DPA %pra (%pU): sharable-partition extent missing shared_extn_seq (firmware bug)\n",
				&ext_range, uuid);
			return -ENXIO;
		}
	} else if (seq != 0) {
		dev_err_ratelimited(dev,
			"DC extent DPA %pra (%pU): non-sharable partition but shared_extn_seq=%u (firmware bug)\n",
			&ext_range, uuid, seq);
		return -ENXIO;
	}

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

static void rm_tag_group(struct cxl_dc_tag_group *group)
{
	struct device *region_dev = &group->cxlr_dax->dev;
	struct dc_extent *dc_extent;
	unsigned long index;

	/*
	 * Tagged allocations release atomically.  Invalidate caches once
	 * for the whole group (no mappings exist at this point — partial
	 * release is not supported, so all members are leaving use
	 * together) before tearing down each dc_extent device.
	 *
	 * Pin @group across the walk: each devm_release_action runs the
	 * dc_extent_unregister action synchronously, which drops the last
	 * reference on the dc_extent device and fires dc_extent_release.
	 * The release decrements group->nr_extents and, on the final
	 * decrement, frees @group.  Without the pin the next iteration's
	 * xa_find_after() dereferences a freed xarray.
	 */
	cxl_region_invalidate_memregion(group->cxlr_dax->cxlr);

	group->nr_extents++;
	xa_for_each(&group->dc_extents, index, dc_extent)
		devm_release_action(region_dev, dc_extent_unregister, dc_extent);
	group->nr_extents--;
	if (!group->nr_extents)
		free_tag_group(group);
}

int cxl_rm_extent(struct cxl_memdev_state *mds, struct cxl_extent *extent)
{
	u64 start_dpa = le64_to_cpu(extent->start_dpa);
	struct cxl_memdev *cxlmd = mds->cxlds.cxlmd;
	struct cxl_endpoint_decoder *cxled;
	struct cxl_dax_region *cxlr_dax;
	struct cxl_dc_tag_group *group;
	struct dc_extent *dc_extent;
	struct cxl_region *cxlr;
	struct range dpa_range;
	unsigned long idx;
	uuid_t tag;

	dpa_range = (struct range) {
		.start = start_dpa,
		.end = start_dpa + le64_to_cpu(extent->length) - 1,
	};

	guard(rwsem_read)(&cxl_rwsem.region);
	cxlr = cxl_dpa_to_region(cxlmd, start_dpa, &cxled);
	if (!cxlr) {
		/*
		 * No region can happen here for a few reasons:
		 *
		 * 1) Extents were accepted and the host crashed/rebooted
		 *    leaving them in an accepted state.  On reboot the host
		 *    has not yet created a region to own them.
		 *
		 * 2) Region destruction won the race with the device releasing
		 *    all the extents.  Here the release will be a duplicate of
		 *    the one sent via region destruction.
		 *
		 * 3) The device is confused and releasing extents for which no
		 *    region ever existed.
		 *
		 * In all these cases make sure the device knows we are not
		 * using this extent.
		 */
		memdev_release_extent(mds, &dpa_range);
		return -ENXIO;
	}

	cxlr_dax = cxlr->cxlr_dax;
	import_uuid(&tag, extent->uuid);

	/*
	 * Find the dc_extent whose DPA range covers the released range and
	 * whose tag matches.  The release targets the entire containing
	 * tag group atomically; partial release is not supported.
	 */
	group = NULL;
	xa_for_each(&cxlr_dax->dc_extents, idx, dc_extent) {
		if (dc_extent->cxled != cxled)
			continue;
		if (!range_contains(&dc_extent->dpa_range, &dpa_range))
			continue;
		if (!uuid_equal(&dc_extent->group->uuid, &tag))
			continue;
		group = dc_extent->group;
		break;
	}
	if (!group) {
		dev_err(&cxlr_dax->dev,
			"release DPA %pra (%pU) matches no dc_extent\n",
			&dpa_range, &tag);
		return -EINVAL;
	}

	rm_tag_group(group);
	return 0;
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
