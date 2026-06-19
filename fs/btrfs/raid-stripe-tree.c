// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023 Western Digital Corporation or its affiliates.
 */

#include <linux/btrfs_tree.h>
#include <linux/raid/xor.h>
#include "ctree.h"
#include "fs.h"
#include "accessors.h"
#include "transaction.h"
#include "disk-io.h"
#include "raid-stripe-tree.h"
#include "volumes.h"
#include "raid56.h"
#include "print-tree.h"
#include "zoned.h"
#include "ordered-data.h"

static int btrfs_partially_delete_raid_extent(struct btrfs_trans_handle *trans,
					       struct btrfs_path *path,
					       const struct btrfs_key *oldkey,
					       u64 newlen, u64 frontpad)
{
	struct btrfs_root *stripe_root = trans->fs_info->stripe_root;
	struct btrfs_stripe_extent *extent, AUTO_KFREE(newitem);
	struct extent_buffer *leaf;
	int slot;
	size_t item_size;
	struct btrfs_key newkey = {
		.objectid = oldkey->objectid + frontpad,
		.type = oldkey->type,
		.offset = newlen,
	};
	int ret;

	ASSERT(newlen > 0);
	ASSERT(oldkey->type == BTRFS_RAID_STRIPE_KEY ||
	       oldkey->type == BTRFS_RAID_STRIPE_PARITY_KEY);

	leaf = path->nodes[0];
	slot = path->slots[0];
	item_size = btrfs_item_size(leaf, slot);

	newitem = kzalloc(item_size, GFP_NOFS);
	if (!newitem)
		return -ENOMEM;

	extent = btrfs_item_ptr(leaf, slot, struct btrfs_stripe_extent);

	for (int i = 0; i < btrfs_num_raid_stripes(item_size); i++) {
		struct btrfs_raid_stride *stride = &extent->strides[i];
		u64 devid;
		u64 phys;

		devid = btrfs_raid_stride_devid(leaf, stride);
		btrfs_set_stack_raid_stride_devid(&newitem->strides[i], devid);
		phys = btrfs_raid_stride_physical(leaf, stride) + frontpad;
		btrfs_set_stack_raid_stride_physical(&newitem->strides[i], phys);
	}

	ret = btrfs_del_item(trans, stripe_root, path);
	if (ret)
		return ret;

	btrfs_release_path(path);
	return btrfs_insert_item(trans, stripe_root, &newkey, newitem, item_size);
}

static int __btrfs_delete_raid_extent(struct btrfs_trans_handle *trans,
				      u64 start, u64 length, u8 type)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_root *stripe_root = fs_info->stripe_root;
	BTRFS_PATH_AUTO_FREE(path);
	struct btrfs_key key;
	struct extent_buffer *leaf;
	u64 found_start;
	u64 found_end;
	u64 end = start + length;
	int slot;
	int ret;

	if (!btrfs_fs_incompat(fs_info, RAID_STRIPE_TREE) || !stripe_root)
		return 0;

	if (!btrfs_is_testing(fs_info)) {
		struct btrfs_chunk_map *map;
		bool use_rst;

		map = btrfs_find_chunk_map(fs_info, start, length);
		if (!map)
			return -EINVAL;
		use_rst = btrfs_need_stripe_tree_update(fs_info, map->type);
		btrfs_free_chunk_map(map);
		if (!use_rst)
			return 0;
	}

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	while (1) {
		key.objectid = start;
		key.type = type;
		key.offset = (u64)-1;

		ret = btrfs_search_slot(trans, stripe_root, &key, path, -1, 1);
		if (ret < 0)
			break;

		/*
		 * Search with offset=(u64)-1 ensures we land on the correct
		 * leaf even when the target entry is the first item on a leaf.
		 * Since no real entry has offset=(u64)-1, ret is always 1 and
		 * slot points past the last entry with objectid==start (or
		 * past the end of the leaf if that entry is the last item).
		 * Back up one slot to find the actual entry.
		 */
		if (path->slots[0] == 0) {
			/* No entry with objectid <= start exists. */
			ret = 0;
			break;
		}
		path->slots[0]--;

		leaf = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(leaf, &key, slot);
		found_start = key.objectid;
		found_end = found_start + key.offset;
		ret = 0;

		/*
		 * The stripe extent starts before the range we want to delete,
		 * but the range spans more than one stripe extent:
		 *
		 * |--- RAID Stripe Extent ---||--- RAID Stripe Extent ---|
		 *        |--- keep  ---|--- drop ---|
		 *
		 * This means we have to get the previous item, truncate its
		 * length and then restart the search.
		 */
		if (found_start > start) {
			if (slot == 0) {
				ret = btrfs_previous_item(stripe_root, path, 0,
							  type);
				if (ret) {
					if (ret > 0)
						ret = -ENOENT;
					break;
				}
			} else {
				path->slots[0]--;
			}

			leaf = path->nodes[0];
			slot = path->slots[0];
			btrfs_item_key_to_cpu(leaf, &key, slot);
			found_start = key.objectid;
			found_end = found_start + key.offset;
			if (found_start > start || found_end <= start) {
				ret = -ENOENT;
				break;
			}
		}

		if (key.type != type)
			break;

		/* That stripe ends before we start, we're done. */
		if (found_end <= start)
			break;

		trace_btrfs_raid_extent_delete(fs_info, start, end,
					       found_start, found_end);

		/*
		 * The stripe extent starts before the range we want to delete
		 * and ends after the range we want to delete, i.e. we're
		 * punching a hole in the stripe extent:
		 *
		 *  |--- RAID Stripe Extent ---|
		 *  | keep |--- drop ---| keep |
		 *
		 * This means we need to a) truncate the existing item and b)
		 * create a second item for the remaining range.
		 */
		if (found_start < start && found_end > end) {
			size_t item_size;
			u64 diff_start = start - found_start;
			u64 diff_end = found_end - end;
			struct btrfs_stripe_extent *extent;
			struct btrfs_key newkey = {
				.objectid = end,
				.type = type,
				.offset = diff_end,
			};

			/* The "right" item. */
			ret = btrfs_duplicate_item(trans, stripe_root, path, &newkey);
			if (ret == -EAGAIN) {
				btrfs_release_path(path);
				continue;
			}
			if (ret)
				break;

			/*
			 * btrfs_duplicate_item() may have triggered a leaf
			 * split via setup_leaf_for_split(), so we must refresh
			 * our leaf pointer from the path.
			 */
			leaf = path->nodes[0];
			item_size = btrfs_item_size(leaf, path->slots[0]);
			extent = btrfs_item_ptr(leaf, path->slots[0],
						struct btrfs_stripe_extent);

			for (int i = 0; i < btrfs_num_raid_stripes(item_size); i++) {
				struct btrfs_raid_stride *stride = &extent->strides[i];
				u64 phys;

				phys = btrfs_raid_stride_physical(leaf, stride);
				phys += diff_start + length;
				btrfs_set_raid_stride_physical(leaf, stride, phys);
			}

			/* The "left" item. */
			path->slots[0]--;
			btrfs_item_key_to_cpu(leaf, &key, path->slots[0]);
			ret = btrfs_partially_delete_raid_extent(trans, path,
								 &key,
								 diff_start, 0);
			break;
		}

		/*
		 * The stripe extent starts before the range we want to delete:
		 *
		 * |--- RAID Stripe Extent ---|
		 * |--- keep  ---|--- drop ---|
		 *
		 * This means we have to duplicate the tree item, truncate the
		 * length to the new size and then re-insert the item.
		 */
		if (found_start < start) {
			u64 diff_start = start - found_start;

			ret = btrfs_partially_delete_raid_extent(trans, path,
								 &key,
								 diff_start, 0);
			if (ret)
				break;

			start += (key.offset - diff_start);
			length -= (key.offset - diff_start);
			if (length == 0)
				break;

			btrfs_release_path(path);
			continue;
		}

		/*
		 * The stripe extent ends after the range we want to delete:
		 *
		 * |--- RAID Stripe Extent ---|
		 * |--- drop  ---|--- keep ---|
		 *
		 * This means we have to duplicate the tree item, truncate the
		 * length to the new size and then re-insert the item.
		 */
		if (found_end > end) {
			u64 diff_end = found_end - end;

			ret = btrfs_partially_delete_raid_extent(trans, path,
								 &key,
								 key.offset - length,
								 length);
			ASSERT(key.offset - diff_end == length,
			       "key.offset=%llu diff_end=%llu length=%llu",
			       key.offset, diff_end, length);
			break;
		}

		/* Finally we can delete the whole item, no more special cases. */
		ret = btrfs_del_item(trans, stripe_root, path);
		if (ret)
			break;

		start += key.offset;
		length -= key.offset;
		if (length == 0)
			break;

		btrfs_release_path(path);
	}

	return ret;
}

int btrfs_delete_raid_extent(struct btrfs_trans_handle *trans, u64 start, u64 length)
{
	struct btrfs_chunk_map *map;
	bool has_parity = false;
	int ret;

	map = btrfs_find_chunk_map(trans->fs_info, start, length);
	has_parity = map && btrfs_nr_parity_stripes(map->type);

	if (has_parity) {
		const u64 full_stripe = (u64)nr_data_stripes(map) * BTRFS_STRIPE_LEN;
		u64 chunk_start = map->start;
		u64 rel_start = start - chunk_start;
		u64 rel_end = start + length - chunk_start;
		u64 aligned_start, aligned_end;

		btrfs_free_chunk_map(map);

		/*
		 * On a parity profile, freeing only part of a full stripe (a
		 * hole punch, or freeing one of several extents that share a
		 * stripe) must NOT touch the stripe tree: the on-disk parity
		 * was computed over all of the stripe's data columns, so
		 * trimming the parity item or dropping a freed column's data
		 * item would make a degraded read of the *surviving* columns
		 * reconstruct garbage. Instead retain both the data and parity
		 * items and only delete them once the whole stripe is free.
		 *
		 * Restrict the deletion to the full stripes entirely covered by
		 * [start, start + length); the leftover items of partially freed
		 * stripes are reclaimed when the block group is removed (after
		 * relocation rewrites the surviving data as fresh full stripes).
		 */
		aligned_start = roundup(rel_start, full_stripe);
		aligned_end = rounddown(rel_end, full_stripe);
		if (aligned_start >= aligned_end)
			return 0;

		start = chunk_start + aligned_start;
		length = aligned_end - aligned_start;

		ret = __btrfs_delete_raid_extent(trans, start, length,
						 BTRFS_RAID_STRIPE_PARITY_KEY);
		if (ret)
			return ret;

		return __btrfs_delete_raid_extent(trans, start, length,
						  BTRFS_RAID_STRIPE_KEY);
	}

	btrfs_free_chunk_map(map);

	return __btrfs_delete_raid_extent(trans, start, length,
					  BTRFS_RAID_STRIPE_KEY);
}


static int update_raid_extent_item(struct btrfs_trans_handle *trans,
				   struct btrfs_key *key,
				   struct btrfs_stripe_extent *stripe_extent,
				   const size_t item_size)
{
	BTRFS_PATH_AUTO_FREE(path);
	struct extent_buffer *leaf;
	int ret;
	int slot;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	ret = btrfs_search_slot(trans, trans->fs_info->stripe_root, key, path,
				0, 1);
	if (ret)
		return (ret == 1 ? ret : -EINVAL);

	leaf = path->nodes[0];
	slot = path->slots[0];

	write_extent_buffer(leaf, stripe_extent, btrfs_item_ptr_offset(leaf, slot),
			    item_size);

	return ret;
}

static void fill_raid_stride(struct btrfs_io_stripe *stripe,
			     struct btrfs_raid_stride *raid_stride)
{
	u64 devid = stripe->dev->devid;
	u64 physical = stripe->physical;

	btrfs_set_stack_raid_stride_devid(raid_stride, devid);
	btrfs_set_stack_raid_stride_physical(raid_stride, physical);
}

EXPORT_FOR_TESTS
int btrfs_insert_one_raid_extent(struct btrfs_trans_handle *trans,
				 struct btrfs_io_context *bioc, u8 type)
{
	struct btrfs_fs_info *fs_info = trans->fs_info;
	struct btrfs_key stripe_key;
	struct btrfs_root *stripe_root = fs_info->stripe_root;
	const int num_stripes = (type == BTRFS_RAID_STRIPE_PARITY_KEY) ?
		btrfs_nr_parity_stripes(bioc->map_type) :
		btrfs_bg_type_to_factor(bioc->map_type);
	struct btrfs_stripe_extent AUTO_KFREE(stripe_extent);
	const size_t item_size = struct_size(stripe_extent, strides, num_stripes);
	int ret;

	stripe_extent = kzalloc(item_size, GFP_NOFS);
	if (unlikely(!stripe_extent)) {
		btrfs_abort_transaction(trans, -ENOMEM);
		btrfs_end_transaction(trans);
		return -ENOMEM;
	}

	trace_btrfs_insert_one_raid_extent(fs_info, bioc->logical, bioc->size,
					   num_stripes);

	if (type == BTRFS_RAID_STRIPE_PARITY_KEY) {
		const int nr_data = bioc->num_stripes - num_stripes;

		for (int i = 0; i < num_stripes; i++)
			fill_raid_stride(&bioc->stripes[nr_data + i],
					 &stripe_extent->strides[i]);
	} else if (bioc->map_type & BTRFS_BLOCK_GROUP_RAID56_MASK) {
		int stripe_nr = btrfs_bioc_to_stripe_nr(bioc);
		struct btrfs_raid_stride *raid_stride = &stripe_extent->strides[0];

		fill_raid_stride(&bioc->stripes[stripe_nr], raid_stride);
	} else {
		for (int i = 0; i < num_stripes; i++) {
			struct btrfs_io_stripe *stripe = &bioc->stripes[i];
			struct btrfs_raid_stride *raid_stride =
				&stripe_extent->strides[i];

			fill_raid_stride(stripe, raid_stride);
		}
	}

	stripe_key.objectid = bioc->logical;
	stripe_key.type = type;
	stripe_key.offset = bioc->size;

	ret = btrfs_insert_item(trans, stripe_root, &stripe_key, stripe_extent,
				item_size);
	if (ret == -EEXIST) {
		ret = update_raid_extent_item(trans, &stripe_key, stripe_extent,
					      item_size);
		if (ret)
			btrfs_abort_transaction(trans, ret);
	} else if (ret) {
		btrfs_abort_transaction(trans, ret);
	}

	return ret;
}

int btrfs_insert_raid_extent(struct btrfs_trans_handle *trans,
			     struct btrfs_ordered_extent *ordered_extent)
{
	struct btrfs_io_context *bioc;
	int ret;

	if (!btrfs_fs_incompat(trans->fs_info, RAID_STRIPE_TREE))
		return 0;

	list_for_each_entry(bioc, &ordered_extent->bioc_list, rst_ordered_entry) {
		ret = btrfs_insert_one_raid_extent(trans, bioc, BTRFS_RAID_STRIPE_KEY);
		if (ret)
			return ret;
	}

	while (!list_empty(&ordered_extent->bioc_list)) {
		bioc = list_first_entry(&ordered_extent->bioc_list,
					typeof(*bioc), rst_ordered_entry);
		list_del(&bioc->rst_ordered_entry);
		btrfs_put_bioc(bioc);
	}

	return 0;
}

static int btrfs_get_raid_extent(struct btrfs_fs_info *fs_info,
				 u64 logical, u64 *length, u64 map_type,
				 u32 stripe_index, u8 type,
				 struct btrfs_io_stripe *stripe)
{
	struct btrfs_root *stripe_root = fs_info->stripe_root;
	struct btrfs_stripe_extent *stripe_extent;
	struct btrfs_key stripe_key;
	struct btrfs_key found_key;
	BTRFS_PATH_AUTO_FREE(path);
	struct extent_buffer *leaf;
	const u64 end = logical + *length;
	int num_stripes;
	u64 offset;
	u64 found_logical;
	u64 found_length;
	u64 found_end;
	int slot;
	int ret;

	stripe_key.objectid = logical;
	stripe_key.type = type;
	stripe_key.offset = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	if (stripe->rst_search_commit_root) {
		path->skip_locking = true;
		path->search_commit_root = true;
	}

	ret = btrfs_search_slot(NULL, stripe_root, &stripe_key, path, 0, 0);
	if (ret < 0)
		return ret;
	if (ret > 0) {
		/*
		 * The exact key is not present, so the stripe extent that may
		 * contain @logical is the previous item of @type. Use
		 * btrfs_previous_item() to step back to it: it crosses into the
		 * previous leaf when we landed on slot 0 of a leaf (a plain
		 * slots[0]-- would stay in the current leaf and miss an item
		 * that is the last one of the previous leaf), and it skips items
		 * of a different type (e.g. the parity stripe).
		 *
		 * If there is no previous item of @type, the covering item (if
		 * any) is the one we landed on (its objectid == logical), so
		 * re-search to position there for the forward scan below.
		 */
		ret = btrfs_previous_item(stripe_root, path, 0, type);
		if (ret < 0)
			return ret;
		if (ret > 0) {
			btrfs_release_path(path);
			ret = btrfs_search_slot(NULL, stripe_root, &stripe_key,
						path, 0, 0);
			if (ret < 0)
				return ret;
		}
	}

	while (1) {
		leaf = path->nodes[0];
		slot = path->slots[0];

		btrfs_item_key_to_cpu(leaf, &found_key, slot);
		found_logical = found_key.objectid;
		found_length = found_key.offset;
		found_end = found_logical + found_length;

		if (found_logical > end) {
			ret = -ENODATA;
			goto out;
		}

		if (in_range(logical, found_logical, found_length) &&
		    found_key.type == type)
			break;

		ret = btrfs_next_item(stripe_root, path);
		if (ret)
			goto out;
	}

	offset = logical - found_logical;

	/*
	 * If we have a logically contiguous, but physically non-continuous
	 * range, we need to split the bio. Record the length after which we
	 * must split the bio.
	 */
	if (end > found_end)
		*length -= end - found_end;

	num_stripes = btrfs_num_raid_stripes(btrfs_item_size(leaf, slot));
	stripe_extent = btrfs_item_ptr(leaf, slot, struct btrfs_stripe_extent);

	if (map_type & BTRFS_BLOCK_GROUP_RAID56_MASK) {
		/*
		 * Unlike the other profiles, a RAID56 data (or parity) column is
		 * not placed on a device the caller can derive from the chunk's
		 * geometric rotation: with a zone append the device picks where
		 * the write lands, so the stripe tree records the device the
		 * column actually went to. Trust that recorded (devid, physical)
		 * rather than the caller's rotation-derived device - matching
		 * against the latter can spuriously miss and return -ENODATA for
		 * data that is present, just on a different device.
		 */
		struct btrfs_raid_stride *stride = &stripe_extent->strides[0];
		BTRFS_DEV_LOOKUP_ARGS(args);
		struct btrfs_device *dev;

		args.devid = btrfs_raid_stride_devid(leaf, stride);
		dev = btrfs_find_device(fs_info->fs_devices, &args);
		if (!dev) {
			ret = -ENODATA;
			goto out;
		}

		stripe->dev = dev;
		stripe->physical = btrfs_raid_stride_physical(leaf, stride) + offset;

		trace_btrfs_get_raid_extent_offset(fs_info, logical, *length,
						   stripe->physical, args.devid);

		return 0;
	}

	for (int i = 0; i < num_stripes; i++) {
		struct btrfs_raid_stride *stride = &stripe_extent->strides[i];
		u64 devid = btrfs_raid_stride_devid(leaf, stride);
		u64 physical = btrfs_raid_stride_physical(leaf, stride);

		if (devid != stripe->dev->devid)
			continue;

		if ((map_type & BTRFS_BLOCK_GROUP_DUP) && stripe_index != i)
			continue;

		stripe->physical = physical + offset;

		trace_btrfs_get_raid_extent_offset(fs_info, logical, *length,
						   stripe->physical, devid);

		return 0;
	}

	/* If we're here, we haven't found the requested devid in the stripe. */
	ret = -ENODATA;
out:
	if (ret > 0)
		ret = -ENODATA;
	if (ret && ret != -EIO && !stripe->rst_search_commit_root) {
		btrfs_debug(fs_info,
		"cannot find raid-stripe for logical [%llu, %llu] devid %llu, profile %s",
			  logical, logical + *length, stripe->dev->devid,
			  btrfs_bg_type_to_raid_name(map_type));
	}

	return ret;
}

int btrfs_get_raid_extent_offset(struct btrfs_fs_info *fs_info,
				 u64 logical, u64 *length, u64 map_type,
				 u32 stripe_index, struct btrfs_io_stripe *stripe)
{
	return btrfs_get_raid_extent(fs_info, logical, length, map_type,
				     stripe_index, BTRFS_RAID_STRIPE_KEY,
				     stripe);
}

int btrfs_get_parity_extent(struct btrfs_fs_info *fs_info,
			    u64 logical, u64 *length, u64 map_type,
			    struct btrfs_io_stripe *stripe)
{
	return btrfs_get_raid_extent(fs_info, logical, length, map_type,
				     0, BTRFS_RAID_STRIPE_PARITY_KEY,
				     stripe);
}

struct btrfs_stripe_unit {
	struct btrfs_io_stripe *pstripe;
	struct bio *bio;
	struct folio *folio;
};

struct btrfs_stripe_set {
	struct btrfs_fs_info *fs_info;
	struct work_struct work;
	struct list_head list;
	u64 full_stripe_logical;
	u64 full_stripe_len;
	u64 logical;
	u64 len;
	refcount_t refs;
	atomic_t pending_ios;
	spinlock_t lock;
	u32 covered;
	bool finalized;
	bool can_use_append;
	unsigned int nr_data;
	unsigned int npar;
	struct btrfs_stripe_unit stripe_units[] __counted_by(npar);
};

static void btrfs_stripe_set_get(struct btrfs_stripe_set *set)
{
	refcount_inc(&set->refs);
}

static void btrfs_stripe_set_put(struct btrfs_fs_info *fs_info,
				   struct btrfs_stripe_set *set)
{
	if (refcount_dec_and_test(&set->refs)) {
		unsigned long flags;

		ASSERT(atomic_read(&set->pending_ios) == 0);
		spin_lock_irqsave(&fs_info->stripe_set_list_lock, flags);
		list_del_init(&set->list);
		spin_unlock_irqrestore(&fs_info->stripe_set_list_lock, flags);

		kfree(set);
	}
}

static struct btrfs_stripe_set *btrfs_stripe_set_lookup(
					struct btrfs_io_context *bioc,
					u64 length)
{
	struct btrfs_fs_info *fs_info = bioc->fs_info;
	struct btrfs_stripe_set *set;
	unsigned long flags;
	bool found = false;

	spin_lock_irqsave(&fs_info->stripe_set_list_lock, flags);
	list_for_each_entry(set, &fs_info->stripe_sets, list) {
		if (set->finalized)
			continue;
		if (in_range(bioc->logical, set->full_stripe_logical,
			     set->full_stripe_len)) {
			found = true;
			break;
		}

	}
	if (found)
		btrfs_stripe_set_get(set);
	spin_unlock_irqrestore(&fs_info->stripe_set_list_lock, flags);

	if (!found)
		return NULL;

	return set;
}

static void insert_parity_stripe_work(struct work_struct *work)
{
	struct btrfs_stripe_set *set =
		container_of(work, struct btrfs_stripe_set, work);
	struct btrfs_fs_info *fs_info = set->fs_info;
	struct btrfs_trans_handle *trans;
	struct btrfs_key stripe_key;
	struct btrfs_root *stripe_root = fs_info->stripe_root;
	struct btrfs_stripe_extent *stripe_extent;
	const size_t item_size = struct_size(stripe_extent, strides, set->npar);
	int ret;

	trans = btrfs_join_transaction(stripe_root);
	if (IS_ERR(trans)) {
		btrfs_stripe_set_put(fs_info, set);
		return;
	}

	stripe_extent = kzalloc(item_size, GFP_NOFS);
	if (!stripe_extent) {
		btrfs_abort_transaction(trans, -ENOMEM);
		btrfs_end_transaction(trans);
		btrfs_stripe_set_put(fs_info, set);
		return;
	}

	trace_btrfs_insert_parity_stripe(fs_info, set->logical, set->len, set->npar);

	for (int i = 0; i < set->npar; i++) {
		struct btrfs_raid_stride *raid_stride = &stripe_extent->strides[i];

		fill_raid_stride(set->stripe_units[i].pstripe, raid_stride);
	}

	/* Parity covers the whole full stripe, keyed at the stripe start. */
	stripe_key.objectid = set->full_stripe_logical;
	stripe_key.type = BTRFS_RAID_STRIPE_PARITY_KEY;
	stripe_key.offset = set->len;

	ret = btrfs_insert_item(trans, stripe_root, &stripe_key, stripe_extent,
				item_size);
	if (ret == -EEXIST)
		ret = update_raid_extent_item(trans, &stripe_key, stripe_extent,
					      item_size);
	if (ret)
		btrfs_abort_transaction(trans, ret);

	btrfs_end_transaction(trans);
	kfree(stripe_extent);
	btrfs_stripe_set_put(fs_info, set);
}

static void raid56_write_endio(struct bio *bio)
{
	struct btrfs_stripe_set *set = bio->bi_private;
	struct btrfs_fs_info *fs_info = set->fs_info;
	struct folio_iter fi;

	if (bio_is_zone_append(bio) && !bio->bi_status)
		set->stripe_units[0].pstripe->physical =
			bio->bi_iter.bi_sector << SECTOR_SHIFT;

	if (atomic_dec_and_test(&set->pending_ios))
		queue_work(fs_info->endio_workers, &set->work);

	bio_for_each_folio_all(fi, bio)
		folio_put(fi.folio);

	btrfs_stripe_set_put(fs_info, set);
	bio_put(bio);
}

static struct btrfs_stripe_set *btrfs_stripe_set_alloc(
						struct btrfs_io_context *bioc,
						u64 len)
{
	struct btrfs_fs_info *fs_info = bioc->fs_info;
	const unsigned int nr_parity = btrfs_nr_parity_stripes(bioc->map_type);
	const unsigned int nr_data = bioc->num_stripes - nr_parity;
	struct btrfs_stripe_set *set;
	unsigned long flags;

	set = kzalloc(struct_size(set, stripe_units, nr_parity), GFP_NOFS);
	if (!set)
		return ERR_PTR(-ENOMEM);

	refcount_set(&set->refs, 1);
	atomic_set(&set->pending_ios, 0);
	spin_lock_init(&set->lock);
	set->covered = 0;
	set->finalized = false;
	INIT_LIST_HEAD(&set->list);
	INIT_WORK(&set->work, insert_parity_stripe_work);
	set->fs_info = fs_info;
	set->full_stripe_logical = bioc->full_stripe_logical;
	set->full_stripe_len = nr_data * BTRFS_STRIPE_LEN;
	set->logical = bioc->logical;
	set->len = len;
	set->npar = nr_parity;
	set->nr_data = nr_data;

	/*
	 * bioc->stripes[0 ... nr_data - 1] holds the data,
	 * bioc->stripes[nr_data ... nr_data + nr_parity] the parity.
	 *
	 * Each parity stripe is exactly BTRFS_STRIPE_LEN bytes, independent of
	 * the data length @len which spans all nr_data data stripes.
	 */
	for (int i = 0; i < nr_parity; i++) {
		struct btrfs_io_stripe *pstripe = bioc->stripes + nr_data + i;
		gfp_t gfp = GFP_NOFS | __GFP_ZERO | __GFP_NOFAIL;
		struct folio *folio;
		struct bio *bio;

		bio = bio_alloc(pstripe->dev->bdev, 1, REQ_OP_WRITE, gfp);
		folio = folio_alloc(gfp, get_order(BTRFS_STRIPE_LEN));

		set->stripe_units[i].bio = bio;
		set->stripe_units[i].folio = folio;
		set->stripe_units[i].pstripe = pstripe;
		bio_add_folio_nofail(bio, folio, BTRFS_STRIPE_LEN, 0);
	}

	spin_lock_irqsave(&fs_info->stripe_set_list_lock, flags);
	list_add_tail(&set->list, &fs_info->stripe_sets);
	spin_unlock_irqrestore(&fs_info->stripe_set_list_lock, flags);

	return set;
}

static bool btrfs_stripe_set_calc_parity_raid5(struct btrfs_stripe_set *set,
					       struct btrfs_bio *orig,
					       struct btrfs_io_context *bioc)
{
	const u32 stripe_len = BTRFS_STRIPE_LEN;
	u8 *parity = folio_address(set->stripe_units[0].folio);
	const u32 offset = bioc->logical - set->full_stripe_logical;
	const u32 poff = offset % stripe_len;
	const u32 to_fold = min_t(u32, orig->bio.bi_iter.bi_size,
				  stripe_len - poff);
	struct bio_vec bvec;
	struct bvec_iter iter;
	u32 done = 0;
	bool complete;

	spin_lock(&set->lock);
	bio_for_each_segment(bvec, &orig->bio, iter) {
		u8 *src = bvec_virt(&bvec);
		u32 chunk = min_t(u32, bvec.bv_len, to_fold - done);
		void *xsrc = src;

		if (!chunk)
			break;
		xor_gen(parity + poff + done, &xsrc, 1, chunk);
		done += chunk;
		if (done >= to_fold)
			break;
	}
	set->covered += done;
	complete = (set->covered >= set->full_stripe_len);
	spin_unlock(&set->lock);

	return complete;
}

static bool btrfs_stripe_set_calc_parity(struct btrfs_stripe_set *set,
					 struct btrfs_bio *orig,
					 struct btrfs_io_context *bioc)
{
	switch (set->npar) {
	case 1:
		return btrfs_stripe_set_calc_parity_raid5(set, orig, bioc);
	default:
		/* TODO: multiple parity (RAID6) calculation. */
		ASSERT(0, "unsupported number of parity stripes %u", set->npar);
		return false;
	}
}

static void btrfs_stripe_set_finalize(struct btrfs_stripe_set *set)
{
	struct btrfs_fs_info *fs_info = set->fs_info;
	struct btrfs_io_stripe *pstripe = set->stripe_units[0].pstripe;
	struct bio *bio = set->stripe_units[0].bio;
	u64 physical = pstripe->physical;
	unsigned long flags;

	ASSERT(set->npar == 1, "npar=%u", set->npar);

	spin_lock(&set->lock);
	if (set->finalized) {
		spin_unlock(&set->lock);
		return;
	}
	set->finalized = true;
	spin_unlock(&set->lock);

	spin_lock_irqsave(&fs_info->stripe_set_list_lock, flags);
	list_del_init(&set->list);
	spin_unlock_irqrestore(&fs_info->stripe_set_list_lock, flags);

	if (set->can_use_append && btrfs_dev_is_sequential(pstripe->dev, physical)) {
		bio->bi_opf &= ~REQ_OP_WRITE;
		bio->bi_opf |= REQ_OP_ZONE_APPEND;
		physical = round_down(physical, fs_info->zone_size);
	}

	bio_set_dev(bio, pstripe->dev->bdev);
	bio->bi_iter.bi_sector = physical >> SECTOR_SHIFT;
	bio->bi_end_io = raid56_write_endio;
	bio->bi_private = set;

	btrfs_stripe_set_get(set);
	atomic_inc(&set->pending_ios);
	submit_bio(bio);
}

int btrfs_rst_raid56_write(struct btrfs_bio *orig,
			   struct btrfs_io_context *bioc)
{
	struct btrfs_fs_info *fs_info = bioc->fs_info;
	const unsigned int nr_parity = btrfs_nr_parity_stripes(bioc->map_type);
	const unsigned int nr_data = bioc->num_stripes - nr_parity;
	const u64 full_stripe_len = (u64)nr_data * BTRFS_STRIPE_LEN;
	struct btrfs_stripe_set *set;
	bool created = false;
	bool complete;

	ASSERT(nr_parity == 1, "Only single-parity (RAID5) is implemented");

	/*
	 * Accumulate the parity for the full stripe bioc->logical falls into.
	 * Each data bbio folds its column into the shared stripe set; when all
	 * nr_data columns have been folded the parity is submitted eagerly. A
	 * full stripe split across ordered extents is completed when its last
	 * column arrives; a partial tail stripe is finalized from the ordered-
	 * extent completion in btrfs_rst_raid56_finish_ordered().
	 */
	set = btrfs_stripe_set_lookup(bioc, full_stripe_len);
	if (!set) {
		set = btrfs_stripe_set_alloc(bioc, full_stripe_len);
		if (IS_ERR(set))
			return PTR_ERR(set);
		set->can_use_append = orig->can_use_append;
		created = true;
	}

	complete = btrfs_stripe_set_calc_parity(set, orig, bioc);
	if (complete)
		btrfs_stripe_set_finalize(set);

	if (!created)
		btrfs_stripe_set_put(fs_info, set);

	return 0;
}

/*
 * Finalize the parity of a partial tail stripe once its data ordered extent has
 * completed (so the data columns are committed). Complete stripes are submitted
 * by btrfs_rst_raid56_write(). The only stripe that can still be pending here
 * is the one holding the last byte of ordered's data. If a data extent
 * continues right after ordered, that stripe spans two ordered extents and
 * will be completed by the next one's data, so it is left alone.
 */
void btrfs_rst_raid56_finish_ordered(struct btrfs_ordered_extent *ordered)
{
	struct btrfs_inode *inode = ordered->inode;
	struct btrfs_fs_info *fs_info = inode->root->fs_info;
	const u64 logical_end = ordered->disk_bytenr + ordered->disk_num_bytes;
	struct btrfs_stripe_set *set, *found = NULL;
	unsigned long flags;

	if (!fs_info->stripe_root)
		return;

	spin_lock_irqsave(&fs_info->stripe_set_list_lock, flags);
	list_for_each_entry(set, &fs_info->stripe_sets, list) {
		if (set->finalized)
			continue;
		if (in_range(logical_end - 1, set->full_stripe_logical,
			     set->full_stripe_len)) {
			found = set;
			btrfs_stripe_set_get(set);
			break;
		}
	}
	spin_unlock_irqrestore(&fs_info->stripe_set_list_lock, flags);

	if (!found)
		return;

	/*
	 * If the file extends past this ordered extent, more data
	 * will land in this stripe and complete it eagerly, so leave it alone.
	 * Only when this ordered extent reaches i_size is the stripe a genuine
	 * tail.
	 */
	if (ordered->file_offset + ordered->num_bytes <
	    i_size_read(&inode->vfs_inode)) {
		btrfs_stripe_set_put(fs_info, found);
		return;
	}

	btrfs_stripe_set_finalize(found);
	btrfs_stripe_set_put(fs_info, found);
}

void btrfs_rst_flush_stripe_sets(struct btrfs_fs_info *fs_info)
{
	struct btrfs_stripe_set *set, *tmp;
	unsigned long flags;
	LIST_HEAD(pending);

	if (!fs_info->stripe_root)
		return;

	spin_lock_irqsave(&fs_info->stripe_set_list_lock, flags);
	list_splice_init(&fs_info->stripe_sets, &pending);
	spin_unlock_irqrestore(&fs_info->stripe_set_list_lock, flags);

	list_for_each_entry_safe(set, tmp, &pending, list)
		btrfs_stripe_set_finalize(set);
}

/*
 * Free any stripe sets still pending at unmount of an aborted filesystem.
 *
 * When the filesystem is forced read-only mid-write, the data ordered extents
 * complete with an error and never reach btrfs_rst_raid56_finish_ordered(), so
 * the partial-tail stripe sets they allocated are never finalized: their parity
 * bios/folios are never submitted and the sets stay on fs_info->stripe_sets.
 * Tear them down here (we cannot submit parity on a read-only fs) so the list
 * is empty and close_ctree() does not trip its ASSERT.
 */
void btrfs_rst_destroy_stripe_sets(struct btrfs_fs_info *fs_info)
{
	struct btrfs_stripe_set *set, *tmp;
	unsigned long flags;
	LIST_HEAD(pending);

	if (!fs_info->stripe_root)
		return;

	spin_lock_irqsave(&fs_info->stripe_set_list_lock, flags);
	list_splice_init(&fs_info->stripe_sets, &pending);
	spin_unlock_irqrestore(&fs_info->stripe_set_list_lock, flags);

	list_for_each_entry_safe(set, tmp, &pending, list) {
		ASSERT(!set->finalized);
		ASSERT(atomic_read(&set->pending_ios) == 0);

		for (int i = 0; i < set->npar; i++) {
			struct btrfs_stripe_unit *unit = &set->stripe_units[i];

			if (unit->folio)
				folio_put(unit->folio);
			if (unit->bio)
				bio_io_error(unit->bio);
		}

		btrfs_stripe_set_put(fs_info, set);
	}
}

static int btrfs_rst_read_stripe(struct btrfs_device *dev, u64 physical,
				 struct folio *folio, u64 len)
{
	struct bio *bio;
	int ret;

	if (!dev->bdev)
		return -EIO;

	bio = bio_alloc(dev->bdev, 1, REQ_OP_READ, GFP_NOFS);
	if (!bio)
		return -ENOMEM;

	bio_add_folio_nofail(bio, folio, len, 0);
	bio->bi_iter.bi_sector = physical >> SECTOR_SHIFT;
	ret = submit_bio_wait(bio);
	bio_put(bio);

	return ret;
}

int btrfs_rst_raid56_read(struct btrfs_bio *orig,
			  struct btrfs_io_context *bioc)
{
	struct btrfs_fs_info *fs_info = bioc->fs_info;
	const unsigned int nr_parity = btrfs_nr_parity_stripes(bioc->map_type);
	const unsigned int nr_data = bioc->num_stripes - nr_parity;
	const unsigned int target = btrfs_bioc_to_stripe_nr(bioc);
	const u64 full = bioc->full_stripe_logical;
	const u64 off_in_stripe =
		bioc->logical - (full + (u64)target * BTRFS_STRIPE_LEN);
	const u64 len = orig->bio.bi_iter.bi_size;
	const int order = get_order(len);
	const bool commit_root = btrfs_is_data_reloc_root(orig->inode->root);
	struct btrfs_io_stripe pstripe = { 0 };
	struct folio **folios = NULL;
	struct bvec_iter iter;
	struct bio_vec bv;
	void **bufs = NULL;
	u64 plen = len;
	u64 off;
	unsigned int idx = 0;
	int ret;

	const bool target_present = bioc->stripes[target].dev->bdev != NULL;
	bool others_present = true;

	for (int i = 0; i < bioc->num_stripes; i++)
		if (i != target && !bioc->stripes[i].dev->bdev)
			others_present = false;

	if (target_present && (orig->mirror_num <= 1 || !others_present)) {
		struct btrfs_io_stripe dstripe = { 0 };
		u64 dlen = len;

		dstripe.dev = bioc->stripes[target].dev;
		dstripe.rst_search_commit_root = commit_root;
		ret = btrfs_get_raid_extent_offset(fs_info, bioc->logical, &dlen,
						   bioc->map_type, 0, &dstripe);
		if (ret)
			return ret;

		bio_set_dev(&orig->bio, dstripe.dev->bdev);
		orig->bio.bi_iter.bi_sector = dstripe.physical >> SECTOR_SHIFT;
		return submit_bio_wait(&orig->bio);
	}

	if (!others_present)
		return -EIO;

	folios = kcalloc(nr_data, sizeof(*folios), GFP_NOFS);
	if (!folios) {
		ret = -ENOMEM;
		goto out;
	}

	bufs = kcalloc(nr_data, sizeof(*bufs), GFP_NOFS);
	if (!bufs) {
		ret = -ENOMEM;
		goto out;
	}

	for (int i = 0; i < nr_data; i++) {
		struct btrfs_io_stripe s = { 0 };
		u64 slogical = full + (u64)i * BTRFS_STRIPE_LEN + off_in_stripe;
		u64 slen = len;

		if (i == target)
			continue;

		s.dev = bioc->stripes[i].dev;
		s.rst_search_commit_root = commit_root;
		ret = btrfs_get_raid_extent_offset(fs_info, slogical, &slen,
						   bioc->map_type, 0, &s);
		if (ret == -ENODATA) {
			ret = 0;
			continue;
		}
		if (ret)
			goto out;

		folios[idx] = folio_alloc(GFP_NOFS, order);
		if (!folios[idx]) {
			ret = -ENOMEM;
			goto out;
		}

		ret = btrfs_rst_read_stripe(s.dev, s.physical, folios[idx], len);
		if (ret)
			goto out;

		bufs[idx] = folio_address(folios[idx]);
		idx++;
	}

	folios[idx] = folio_alloc(GFP_NOFS, order);
	if (!folios[idx]) {
		ret = -ENOMEM;
		goto out;
	}

	pstripe.dev = bioc->stripes[nr_data].dev;
	pstripe.rst_search_commit_root = commit_root;
	ret = btrfs_get_parity_extent(fs_info, full + off_in_stripe, &plen,
				      bioc->map_type, &pstripe);
	if (ret)
		goto out;

	ret = btrfs_rst_read_stripe(pstripe.dev, pstripe.physical, folios[idx],
				    len);
	if (ret)
		goto out;

	bufs[idx] = folio_address(folios[idx]);
	idx++;

	xor_gen(bufs[idx - 1], bufs, idx - 1, len);

	off = 0;
	bio_for_each_segment(bv, &orig->bio, iter) {
		memcpy_to_bvec(&bv, (const char *)bufs[idx - 1] + off);
		off += bv.bv_len;
	}
	ret = 0;

out:
	if (folios) {
		for (unsigned int i = 0; i < nr_data; i++)
			if (folios[i])
				folio_put(folios[i]);
	}
	kfree(folios);
	kfree(bufs);

	return ret;
}
