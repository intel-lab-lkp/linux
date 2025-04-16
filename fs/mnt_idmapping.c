// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2022 Christian Brauner <brauner@kernel.org> */

#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/mnt_idmapping.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>
#include <linux/seq_file.h>

#include "internal.h"

/*
 * Carries the initial idmapping of 0:0:4294967295 which is an identity
 * mapping. This means that {g,u}id 0 is mapped to {g,u}id 0, {g,u}id 1 is
 * mapped to {g,u}id 1, [...], {g,u}id 1000 to {g,u}id 1000, [...].
 */
struct mnt_idmap nop_mnt_idmap = {
	.count	= REFCOUNT_INIT(1),
};
EXPORT_SYMBOL_GPL(nop_mnt_idmap);

/*
 * Carries the invalid idmapping of a full 0-4294967295 {g,u}id range.
 * This means that all {g,u}ids are mapped to INVALID_VFS{G,U}ID.
 */
struct mnt_idmap invalid_mnt_idmap = {
	.count	= REFCOUNT_INIT(1),
};
EXPORT_SYMBOL_GPL(invalid_mnt_idmap);

#ifdef CONFIG_MULTIUSER
/**
 * vfsgid_in_group_p() - check whether a vfsuid matches the caller's groups
 * @vfsgid: the mnt gid to match
 *
 * This function can be used to determine whether @vfsuid matches any of the
 * caller's groups.
 *
 * Return: 1 if vfsuid matches caller's groups, 0 if not.
 */
int vfsgid_in_group_p(vfsgid_t vfsgid)
{
	return in_group_p(AS_KGIDT(vfsgid));
}
#else
int vfsgid_in_group_p(vfsgid_t vfsgid)
{
	return 1;
}
#endif
EXPORT_SYMBOL_GPL(vfsgid_in_group_p);

static int copy_mnt_idmap(struct uid_gid_map *map_from,
			  struct uid_gid_map *map_to)
{
	struct uid_gid_extent *forward, *reverse;
	u32 nr_extents = READ_ONCE(map_from->nr_extents);
	/* Pairs with smp_wmb() when writing the idmapping. */
	smp_rmb();

	/*
	 * Don't blindly copy @map_to into @map_from if nr_extents is
	 * smaller or equal to UID_GID_MAP_MAX_BASE_EXTENTS. Since we
	 * read @nr_extents someone could have written an idmapping and
	 * then we might end up with inconsistent data. So just don't do
	 * anything at all.
	 */
	if (nr_extents == 0)
		return -EINVAL;

	/*
	 * Here we know that nr_extents is greater than zero which means
	 * a map has been written. Since idmappings can't be changed
	 * once they have been written we know that we can safely copy
	 * from @map_to into @map_from.
	 */

	if (nr_extents <= UID_GID_MAP_MAX_BASE_EXTENTS) {
		*map_to = *map_from;
		return 0;
	}

	forward = kmemdup_array(map_from->forward, nr_extents,
				sizeof(struct uid_gid_extent),
				GFP_KERNEL_ACCOUNT);
	if (!forward)
		return -ENOMEM;

	reverse = kmemdup_array(map_from->reverse, nr_extents,
				sizeof(struct uid_gid_extent),
				GFP_KERNEL_ACCOUNT);
	if (!reverse) {
		kfree(forward);
		return -ENOMEM;
	}

	/*
	 * The idmapping isn't exposed anywhere so we don't need to care
	 * about ordering between extent pointers and @nr_extents
	 * initialization.
	 */
	map_to->forward = forward;
	map_to->reverse = reverse;
	map_to->nr_extents = nr_extents;
	return 0;
}

static void free_mnt_idmap(struct mnt_idmap *idmap)
{
	if (idmap->uid_map.nr_extents > UID_GID_MAP_MAX_BASE_EXTENTS) {
		kfree(idmap->uid_map.forward);
		kfree(idmap->uid_map.reverse);
	}
	if (idmap->gid_map.nr_extents > UID_GID_MAP_MAX_BASE_EXTENTS) {
		kfree(idmap->gid_map.forward);
		kfree(idmap->gid_map.reverse);
	}
	kfree(idmap);
}

struct mnt_idmap *alloc_mnt_idmap(struct user_namespace *mnt_userns)
{
	struct mnt_idmap *idmap;
	int ret;

	idmap = kzalloc(sizeof(struct mnt_idmap), GFP_KERNEL_ACCOUNT);
	if (!idmap)
		return ERR_PTR(-ENOMEM);

	refcount_set(&idmap->count, 1);
	ret = copy_mnt_idmap(&mnt_userns->uid_map, &idmap->uid_map);
	if (!ret)
		ret = copy_mnt_idmap(&mnt_userns->gid_map, &idmap->gid_map);
	if (ret) {
		free_mnt_idmap(idmap);
		idmap = ERR_PTR(ret);
	}
	return idmap;
}

/**
 * mnt_idmap_get - get a reference to an idmapping
 * @idmap: the idmap to bump the reference on
 *
 * If @idmap is not the @nop_mnt_idmap bump the reference count.
 *
 * Return: @idmap with reference count bumped if @not_mnt_idmap isn't passed.
 */
struct mnt_idmap *mnt_idmap_get(struct mnt_idmap *idmap)
{
	if (idmap != &nop_mnt_idmap && idmap != &invalid_mnt_idmap)
		refcount_inc(&idmap->count);

	return idmap;
}
EXPORT_SYMBOL_GPL(mnt_idmap_get);

/**
 * mnt_idmap_put - put a reference to an idmapping
 * @idmap: the idmap to put the reference on
 *
 * If this is a non-initial idmapping, put the reference count when a mount is
 * released and free it if we're the last user.
 */
void mnt_idmap_put(struct mnt_idmap *idmap)
{
	if (idmap != &nop_mnt_idmap && idmap != &invalid_mnt_idmap &&
	    refcount_dec_and_test(&idmap->count))
		free_mnt_idmap(idmap);
}
EXPORT_SYMBOL_GPL(mnt_idmap_put);

int statmount_mnt_idmap(struct mnt_idmap *idmap, struct seq_file *seq, bool uid_map)
{
	struct uid_gid_map *map, *map_up;
	u32 idx, nr_mappings;

	if (!is_valid_mnt_idmap(idmap))
		return 0;

	/*
	 * Idmappings are shown relative to the caller's idmapping.
	 * This is both the most intuitive and most useful solution.
	 */
	if (uid_map) {
		map = &idmap->uid_map;
		map_up = &current_user_ns()->uid_map;
	} else {
		map = &idmap->gid_map;
		map_up = &current_user_ns()->gid_map;
	}

	for (idx = 0, nr_mappings = 0; idx < map->nr_extents; idx++) {
		uid_t lower;
		struct uid_gid_extent *extent;

		if (map->nr_extents <= UID_GID_MAP_MAX_BASE_EXTENTS)
			extent = &map->extent[idx];
		else
			extent = &map->forward[idx];

		/*
		 * Verify that the whole range of the mapping can be
		 * resolved in the caller's idmapping. If it cannot be
		 * resolved skip the mapping.
		 */
		lower = map_id_range_up(map_up, extent->lower_first, extent->count);
		if (lower == (uid_t) -1)
			continue;

		seq_printf(seq, "%u %u %u", extent->first, lower, extent->count);

		seq->count++; /* mappings are separated by \0 */
		if (seq_has_overflowed(seq))
			return -EAGAIN;

		nr_mappings++;
	}

	return nr_mappings;
}
