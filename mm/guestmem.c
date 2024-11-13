// SPDX-License-Identifier: GPL-2.0-only
/*
 * guestmem library
 *
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/fs.h>
#include <linux/guestmem.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

struct guestmem {
	const struct guestmem_ops *ops;
};

static inline struct guestmem *folio_to_guestmem(struct folio *folio)
{
	struct address_space *const mapping = folio->mapping;

	return mapping->i_private_data;
}

static inline bool __guestmem_release_folio(struct address_space *const mapping,
					    struct folio *folio)
{
	struct guestmem *gmem = mapping->i_private_data;
	struct list_head *entry;

	if (gmem->ops->release_folio) {
		list_for_each(entry, &mapping->i_private_list) {
			if (!gmem->ops->release_folio(entry, folio))
				return false;
		}
	}

	return true;
}

static inline int
__guestmem_invalidate_begin(struct address_space *const mapping, pgoff_t start,
			    pgoff_t end)
{
	struct guestmem *gmem = mapping->i_private_data;
	struct list_head *entry;
	int ret = 0;

	list_for_each(entry, &mapping->i_private_list) {
		ret = gmem->ops->invalidate_begin(entry, start, end);
		if (ret)
			return ret;
	}

	return 0;
}

static inline void
__guestmem_invalidate_end(struct address_space *const mapping, pgoff_t start,
			  pgoff_t end)
{
	struct guestmem *gmem = mapping->i_private_data;
	struct list_head *entry;

	if (gmem->ops->invalidate_end) {
		list_for_each(entry, &mapping->i_private_list)
			gmem->ops->invalidate_end(entry, start, end);
	}
}

static bool guestmem_release_folio(struct folio *folio, gfp_t gfp)
{
	return __guestmem_release_folio(folio->mapping, folio);
}

static void guestmem_invalidate_folio(struct folio *folio, size_t offset,
				      size_t len)
{
	WARN_ON_ONCE(offset != 0);
	WARN_ON_ONCE(len != folio_size(folio));

	if (offset == 0 && len == folio_size(folio))
		WARN_ON_ONCE(filemap_release_folio(folio, 0));
}

static int guestmem_error_folio(struct address_space *mapping,
				struct folio *folio)
{
	pgoff_t start, end;
	int ret;

	filemap_invalidate_lock_shared(mapping);

	start = folio->index;
	end = start + folio_nr_pages(folio);

	ret = __guestmem_invalidate_begin(mapping, start, end);
	if (ret)
		goto out;

	/*
	 * Do not truncate the range, what action is taken in response to the
	 * error is userspace's decision (assuming the architecture supports
	 * gracefully handling memory errors).  If/when the guest attempts to
	 * access a poisoned page, kvm_gmem_get_pfn() will return -EHWPOISON,
	 * at which point KVM can either terminate the VM or propagate the
	 * error to userspace.
	 */

	__guestmem_invalidate_end(mapping, start, end);

out:
	filemap_invalidate_unlock_shared(mapping);
	return ret ? MF_DELAYED : MF_FAILED;
}

static int guestmem_migrate_folio(struct address_space *mapping,
				  struct folio *dst, struct folio *src,
				  enum migrate_mode mode)
{
	WARN_ON_ONCE(1);
	return -EINVAL;
}

static const struct address_space_operations guestmem_aops = {
	.dirty_folio = noop_dirty_folio,
	.release_folio = guestmem_release_folio,
	.invalidate_folio = guestmem_invalidate_folio,
	.error_remove_folio = guestmem_error_folio,
	.migrate_folio = guestmem_migrate_folio,
};

/**
 * guestmem_attach_mapping() - Attach/create a guestmem mapping
 * @mapping: The address space to attach to
 * @ops: The guestmem operations to use
 * @data: Private data to pass to the ops functions
 */
int guestmem_attach_mapping(struct address_space *mapping,
			    const struct guestmem_ops *const ops,
			    struct list_head *data)
{
	struct guestmem *gmem;

	if (mapping->a_ops == &guestmem_aops) {
		gmem = mapping->i_private_data;
		if (gmem->ops != ops)
			return -EINVAL;

		goto add;
	}

	gmem = kzalloc(sizeof(*gmem), GFP_KERNEL);
	if (!gmem)
		return -ENOMEM;

	gmem->ops = ops;

	mapping->a_ops = &guestmem_aops;
	mapping->i_private_data = gmem;

	mapping_set_gfp_mask(mapping, GFP_HIGHUSER);
	mapping_set_inaccessible(mapping);
	/* Unmovable mappings are supposed to be marked unevictable as well. */
	WARN_ON_ONCE(!mapping_unevictable(mapping));

add:
	list_add(data, &mapping->i_private_list);
	return 0;
}
EXPORT_SYMBOL_GPL(guestmem_attach_mapping);

/**
 * guestmem_detach_mapping() - Detach a guestmem mapping
 * @mapping: The address space to detach
 * @data: Private data to detach
 */
void guestmem_detach_mapping(struct address_space *mapping,
			     struct list_head *data)
{
	list_del(data);

	if (list_empty(&mapping->i_private_list)) {
		kfree(mapping->i_private_data);
		mapping->i_private_data = NULL;
		mapping->a_ops = &empty_aops;
	}
}
EXPORT_SYMBOL_GPL(guestmem_detach_mapping);

/**
 * guestmem_grab_folio() - Grab a folio from a guestmem mapping
 * @mapping: The address space to grab from
 * @index: The index of the folio to grab
 *
 * Return: The grabbed folio, or ERR_PTR() on failure.
 */
struct folio *guestmem_grab_folio(struct address_space *mapping, pgoff_t index)
{
	/* TODO: Support huge pages. */
	return filemap_grab_folio(mapping, index);
}
EXPORT_SYMBOL_GPL(guestmem_grab_folio);

/**
 * guestmem_put_folio() - Helper to punch a hole in a guestmem mapping
 * @mapping: The address space to punch a hole in
 * @offset: The offset to punch a hole at
 * @len: The length of the hole to punch
 *
 * Return: 0 on success, -errno on failure.
 */
int guestmem_punch_hole(struct address_space *mapping, loff_t offset,
			loff_t len)
{
	pgoff_t start = offset >> PAGE_SHIFT;
	pgoff_t end = (offset + len) >> PAGE_SHIFT;
	int ret;

	filemap_invalidate_lock(mapping);
	ret = __guestmem_invalidate_begin(mapping, start, end);
	if (ret)
		goto out;

	truncate_inode_pages_range(mapping, offset, offset + len - 1);

	__guestmem_invalidate_end(mapping, start, end);

out:
	filemap_invalidate_unlock(mapping);
	return ret;
}
EXPORT_SYMBOL_GPL(guestmem_punch_hole);
