// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2000-2001 Christoph Hellwig.
 */

/*
 * Veritas filesystem driver - support for 'immed' inodes.
 */
#include <linux/fs.h>
#include <linux/pagemap.h>

#include "vxfs.h"
#include "vxfs_extern.h"
#include "vxfs_inode.h"

/**
 * vxfs_immed_read_folio - read part of an immed inode into pagecache
 * @fp:		file context (unused)
 * @folio:	folio to fill in.
 *
 * Description:
 *   vxfs_immed_read_folio reads a part of the immed area of the
 *   file that hosts @folio into the pagecache.
 *
 * Returns:
 *   Zero on success, else a negative error code.
 *
 * Locking status:
 *   @folio is locked and will be unlocked.
 */
static int vxfs_immed_read_folio(struct file *fp, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	size_t offset = folio_pos(folio);
	struct vxfs_inode_info *vip = VXFS_INO(folio->mapping->host);
	size_t size = min_t(loff_t, inode->i_size,
			    sizeof(vip->vii_immed.vi_immed));
	size_t len = 0;

	if (offset < size) {
		len = min_t(size_t, size - offset, folio_size(folio));
		memcpy_to_folio(folio, 0, vip->vii_immed.vi_immed + offset,
				len);
	}

	if (len < folio_size(folio))
		folio_zero_segment(folio, len, folio_size(folio));

	folio_mark_uptodate(folio);
	folio_unlock(folio);

	return 0;
}

/*
 * Address space operations for immed files and directories.
 */
const struct address_space_operations vxfs_immed_aops = {
	.read_folio =	vxfs_immed_read_folio,
};
