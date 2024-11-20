/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_GUESTMEM_H
#define _LINUX_GUESTMEM_H

struct address_space;
struct list_head;

/**
 * struct guestmem_ops - Hypervisor-specific maintenance operations to perform on folios
 * @release_folio - Try to bring the folio back to fully owned by Linux
 *		    for instance: about to free the folio [optional]
 * @invalidate_begin - start invalidating mappings between start and end offsets
 * @invalidate_end - paired with ->invalidate_begin() [optional]
 */
struct guestmem_ops {
	bool (*release_folio)(struct list_head *entry, struct folio *folio);
	int (*invalidate_begin)(struct list_head *entry, pgoff_t start,
				pgoff_t end);
	void (*invalidate_end)(struct list_head *entry, pgoff_t start,
			       pgoff_t end);
};

int guestmem_attach_mapping(struct address_space *mapping,
			    const struct guestmem_ops *const ops,
			    struct list_head *data);
void guestmem_detach_mapping(struct address_space *mapping,
			     struct list_head *data);

struct folio *guestmem_grab_folio(struct address_space *mapping, pgoff_t index);
int guestmem_punch_hole(struct address_space *mapping, loff_t offset,
			loff_t len);

#endif
