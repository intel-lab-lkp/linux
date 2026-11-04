// SPDX-License-Identifier: GPL-2.0
/*  Copyright(c) 2024 Intel Corporation. All rights reserved. */

#include <linux/device.h>
#include <cxl.h>

#include "core.h"

/*
 * Host-wide list of live tag groups with non-null uuids: a tag uuid
 * identifies exactly one allocation across all regions and memdevs.
 * The orchestrator owns tag allocation; this rejects a repeated tag
 * from a firmware bug or other issue.  Untagged (null uuid) extents are
 * not tracked: they are not associated with a single allocation.
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

static struct cxl_dc_tag_group *to_dc_tag_group(struct device *dev)
{
	return container_of(dev, struct cxl_dc_tag_group, dev);
}

static void dc_tag_group_release(struct device *dev)
{
	struct cxl_dc_tag_group *group = to_dc_tag_group(dev);

	cxl_tag_unregister(group);
	xa_destroy(&group->dc_extents);
	/* Drop the pin taken in alloc_tag_group(). */
	put_device(&group->cxlr_dax->dev);
	kfree(group);
}

static const struct device_type dc_tag_group_type = {
	.name = "tag_group",
	.release = dc_tag_group_release,
};

/* Last extent gone: drop the group's own reference, freeing it */
static void free_tag_group(struct cxl_dc_tag_group *group)
{
	put_device(&group->dev);
}

/* A tagged group is a tag_groupX.Y device, parent of its extents */
static int dc_tag_group_add(struct cxl_dc_tag_group *group)
{
	struct cxl_dax_region *cxlr_dax = group->cxlr_dax;
	struct device *dev = &group->dev;
	int id, rc;

	id = ida_alloc(&cxlr_dax->tag_group_ida, GFP_KERNEL);
	if (id < 0)
		return id;
	dev->id = id;

	rc = dev_set_name(dev, "tag_group%d.%d", cxlr_dax->cxlr->id, id);
	if (!rc)
		rc = device_add(dev);
	if (rc)
		ida_free(&cxlr_dax->tag_group_ida, id);
	return rc;
}

/* Called once the group's extents are gone; a no-op for untagged groups */
static void dc_tag_group_del(struct cxl_dc_tag_group *group)
{
	if (!device_is_registered(&group->dev))
		return;
	device_del(&group->dev);
	ida_free(&group->cxlr_dax->tag_group_ida, group->dev.id);
}

static void dc_extent_release(struct device *dev)
{
	struct dc_extent *dc_extent = to_dc_extent(dev);
	struct cxl_dc_tag_group *group;

	if (!dc_extent)
		return;

	group = dc_extent->group;
	xa_erase(&group->cxlr_dax->dc_extents, dc_extent->dev.id);
	xa_erase(&group->dc_extents, dc_extent->seq_num);
	group->nr_extents--;
	if (!group->nr_extents)
		free_tag_group(group);
	kfree_rcu(dc_extent, rcu);
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
	struct cxl_dc_tag_group *group __free(kfree) = kzalloc_obj(*group);
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

	/*
	 * Pin cxlr_dax: it is used after cxl_rwsem.region is dropped, so a
	 * refcount must keep it alive until the group is freed.
	 */
	get_device(&cxlr_dax->dev);

	/* Added by online_tag_group() for a tagged group, see dc_tag_group_add() */
	device_initialize(&group->dev);
	device_set_pm_not_required(&group->dev);
	group->dev.parent = &cxlr_dax->dev;
	group->dev.type = &dc_tag_group_type;

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

	/*
	 * A device-side error could cause end < start, which range_contains()
	 * would treat as contained in any partition.
	 */
	if (ext_range->end < ext_range->start) {
		dev_err_ratelimited(dev,
				    "DC extent DPA %pra (%pU) has invalid length (firmware bug)\n",
				    ext_range, extent->uuid);
		return NULL;
	}

	for (int i = 0; i < cxlds->nr_partitions; i++) {
		struct cxl_dpa_partition *part = &cxlds->part[i];
		struct range partition_range = {
			.start = part->res.start,
			.end = part->res.end,
		};

		if (part->mode != CXL_PARTMODE_DYNAMIC_RAM)
			continue;

		if (range_contains(&partition_range, ext_range))
			return part;
	}

	dev_err_ratelimited(dev,
			    "DC extent DPA %pra (%pU) is not in a valid DC partition\n",
			    ext_range, extent->uuid);
	return NULL;
}

/*
 * Stage 1 of the add pipeline: pure, no allocation.  Resolve the extent
 * to its region/endpoint decoder and ext_range, and verify the range
 * fits in the resolved endpoint decoder's DPA resource.
 *
 * Caller must hold cxl_rwsem.region and cxl_rwsem.dpa for read:
 * cxl_dpa_to_region() walks every decoder's dpa_res.
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
	const struct cxl_dpa_partition *part;
	struct cxl_endpoint_decoder *cxled;
	struct cxl_region *cxlr;
	struct range ext_range = (struct range) {
		.start = start_dpa,
		.end = start_dpa + le64_to_cpu(extent->length) - 1,
	};
	struct range ed_range;
	uuid_t uuid;

	import_uuid(&uuid, extent->uuid);

	part = cxl_extent_dc_partition(mds, extent, &ext_range);
	if (!part)
		return -ENXIO;
	dev_dbg(dev, "DC extent DPA %pra (DCR:%pR)(%pU)\n", &ext_range,
		&part->res, &uuid);

	if (part->shareable) {
		if (uuid_is_null(&uuid)) {
			dev_err_ratelimited(dev,
					    "DC extent DPA %pra: sharable-partition extent has null tag (firmware bug)\n",
					    &ext_range);
			return -ENXIO;
		}
	} else if (le16_to_cpu(extent->shared_extn_seq)) {
		dev_err_ratelimited(dev,
				    "DC extent DPA %pra (%pU): non-sharable partition but shared_extn_seq=%u (firmware bug)\n",
				    &ext_range, &uuid, le16_to_cpu(extent->shared_extn_seq));
		return -ENXIO;
	}

	cxlr = cxl_dpa_to_region(cxlmd, start_dpa, &cxled);
	if (!cxlr || !cxlr->cxlr_dax)
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
 * fully contains @ext_range means the device re-offered accepted capacity;
 * a stored extent that only overlaps means an inconsistent offer.
 */
static enum cxl_extent_class
cxlr_dax_classify_extent(struct cxl_dax_region *cxlr_dax,
			 struct cxl_endpoint_decoder *cxled,
			 const struct range *ext_range)
{
	struct dc_extent *entry;
	unsigned long i;

	/* kfree_rcu() defers the free; hold RCU so entries stay live while walked */
	guard(rcu)();
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

	dc_extent = kzalloc_obj(*dc_extent);
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
 *
 * Returns 0 on success, <0 on error.
 */
static int cxlr_add_extent(struct cxl_memdev_state *mds,
			   struct cxl_dax_region *cxlr_dax,
			   struct dc_extent *dc_extent)
{
	struct cxl_dc_tag_group **group = &mds->dc_chain.group;
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
	 * Key by @seq_num so iteration order equals assembly order.  @seq_num
	 * is a dense 0..n-1 index (see &struct dc_extent), so a collision
	 * here signals a cxl-side validation gap.
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

/*
 * Returns 0 for a successfully added extent and <0 on error.
 */
int cxl_add_extent(struct cxl_memdev_state *mds, struct cxl_extent *extent,
		   u16 seq_num)
{
	struct cxl_endpoint_decoder *cxled;
	struct cxl_dax_region *cxlr_dax;
	struct dc_extent *dc_extent;
	struct range ext_range;
	int rc;

	/* The caller holds both across the whole group, through online */
	lockdep_assert_held(&cxl_rwsem.region);
	lockdep_assert_held(&cxl_rwsem.dpa);

	rc = cxl_validate_extent(mds, extent, &cxled, &cxlr_dax, &ext_range);
	if (rc)
		return rc;

	switch (cxlr_dax_classify_extent(cxlr_dax, cxled, &ext_range)) {
	case CXL_EXT_DUPLICATE:
		/*
		 * Never re-reported: the device rejects a DPA range already
		 * added by a previous response (CXL r4.0 8.2.10.9.9.3).
		 */
		dev_warn_ratelimited(&cxled->cxld.dev,
				     "Extent %pra already accepted; dropping group\n",
				     &ext_range);
		return -EEXIST;
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

/* The group's device may already be visible: readers walk the xarray under RCU */
static void cleanup_pending_dc_extent(struct dc_extent *dc_extent)
{
	struct cxl_dc_tag_group *group = dc_extent->group;

	xa_erase(&group->dc_extents, dc_extent->seq_num);
	group->nr_extents--;
	if (!group->nr_extents)
		free_tag_group(group);
	kfree_rcu(dc_extent, rcu);
}

/*
 * Drop a staged group that never went online: no dc_extent device exists
 * yet, so there is nothing to unregister and nothing to tell the device.
 */
void discard_tag_group(struct cxl_dc_tag_group *group)
{
	struct dc_extent *dc_extent;
	unsigned long index;

	/* Seed and pin nr_extents as online_tag_group() would */
	xa_for_each(&group->dc_extents, index, dc_extent)
		group->nr_extents++;
	group->nr_extents++;

	xa_for_each(&group->dc_extents, index, dc_extent)
		cleanup_pending_dc_extent(dc_extent);

	group->nr_extents--;
	if (!group->nr_extents)
		free_tag_group(group);
}

/* Unregister every member; the group is freed with its last extent */
static void unregister_tag_group(struct cxl_dc_tag_group *group)
{
	struct dc_extent *dc_extent;
	unsigned long index;

	/*
	 * Pin @group across the walk: unregistering a dc_extent drops its
	 * last reference and fires dc_extent_release(), which frees @group
	 * on the final nr_extents decrement.  Without the pin the next
	 * iteration's xa_find_after() dereferences a freed xarray.
	 */
	group->nr_extents++;
	xa_for_each(&group->dc_extents, index, dc_extent) {
		dev_dbg(&dc_extent->dev, "DAX region rm extent HPA %pra\n",
			&dc_extent->hpa_range);
		device_unregister(&dc_extent->dev);
	}
	dc_tag_group_del(group);
	group->nr_extents--;
	if (!group->nr_extents)
		free_tag_group(group);
}

/*
 * Region teardown, a devres action on the dax_region device: the host gives
 * the group's capacity back to the device.  Flush the region's cached lines
 * first; once the Release DC goes out the device may reassign the range.
 * devres order puts the DAX devices created after the group ahead of it.
 */
static void dc_tag_group_teardown(void *_group)
{
	struct cxl_dc_tag_group *group = _group;

	/* Exclude a release flush, which holds the lock for read */
	guard(rwsem_write)(&cxl_rwsem.region);
	cxl_region_invalidate_memregion(group->cxlr_dax->cxlr);
	memdev_release_group(group);
	unregister_tag_group(group);
}

/*
 * Drop @group without telling the device anything: the caller has already
 * decided what, if anything, the device is told about this capacity.
 * Returns false when the dax region's unbind already owns the teardown.
 */
bool rm_tag_group(struct cxl_dc_tag_group *group)
{
	if (devm_remove_action_nowarn(&group->cxlr_dax->dev,
				      dc_tag_group_teardown, group))
		return false;
	unregister_tag_group(group);
	return true;
}

int online_tag_group(struct cxl_dc_tag_group *group)
{
	struct cxl_dax_region *cxlr_dax = group->cxlr_dax;
	struct device *parent = &cxlr_dax->untagged;
	struct dc_extent *dc_extent;
	unsigned long index;
	int rc = 0;

	/*
	 * Seed nr_extents with the full group size plus a +1 pin held by
	 * this function.  The size counts every dc_extent that might
	 * decrement nr_extents on cleanup; the pin keeps @group alive
	 * across the body even if every dc_extent release fires in the
	 * unwind (e.g. a devm_add_action() failure with a single extent).
	 * The pin is dropped at the end of the function.
	 */
	xa_for_each(&group->dc_extents, index, dc_extent)
		group->nr_extents++;
	group->nr_extents++;

	/* A tagged group parents its extents; untagged ones share @untagged */
	if (!uuid_is_null(&group->uuid)) {
		rc = dc_tag_group_add(group);
		if (rc)
			goto unwind;
		parent = &group->dev;
	} else {
		rc = cxl_dax_untagged_add(cxlr_dax);
		if (rc)
			goto unwind;
	}

	xa_for_each(&group->dc_extents, index, dc_extent) {
		struct device *dev = &dc_extent->dev;
		u32 id;

		device_initialize(dev);
		device_set_pm_not_required(dev);
		dev->parent = parent;
		dev->type = &dc_extent_type;

		rc = xa_alloc(&cxlr_dax->dc_extents, &id, dc_extent,
			      xa_limit_32b, GFP_KERNEL);
		/*
		 * put_device() fires dc_extent_release().  On xa_alloc failure
		 * dev->id is still its invalid init value (0), but the xarray is
		 * declared XA_FLAGS_ALLOC1 so 0 is never a valid id and erasing
		 * it cannot remove another extent.
		 */
		if (rc < 0) {
			put_device(dev);
			break;
		}
		dev->id = id;

		rc = dev_set_name(dev, "extent%d.%d", cxlr_dax->cxlr->id,
				  dev->id);
		if (rc) {
			put_device(dev);
			break;
		}

		rc = device_add(dev);
		if (rc) {
			put_device(dev);
			break;
		}

		dev_dbg(dev, "dc_extent HPA %pra (%pU)\n",
			&dc_extent->hpa_range, &group->uuid);
	}

	/* Give the capacity back when the region goes away */
	if (!rc)
		rc = devm_add_action(&cxlr_dax->dev, dc_tag_group_teardown, group);

unwind:
	if (rc) {
		/*
		 * Unwind every remaining dc_extent in the group.  The pin
		 * above keeps @group alive across this walk.  Onlined
		 * dc_extents are unregistered; pending ones were never
		 * initialized as devices.
		 */
		xa_for_each(&group->dc_extents, index, dc_extent) {
			if (device_is_registered(&dc_extent->dev))
				device_unregister(&dc_extent->dev);
			else
				cleanup_pending_dc_extent(dc_extent);
		}
		dc_tag_group_del(group);
	}

	/* Drop the pin; if nothing else still references @group, free it. */
	group->nr_extents--;
	if (!group->nr_extents)
		free_tag_group(group);
	return rc;
}
