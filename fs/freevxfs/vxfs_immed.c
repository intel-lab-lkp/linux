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
 * @file:	file context (unused)
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
static int vxfs_immed_read_folio(struct file *file, struct folio *folio)
{
	struct vxfs_inode_info *vip = VXFS_INO(folio->mapping->host);
	size_t len = VXFS_NIMMED;

	if (folio->index > 0)
		len = 0;

	folio_fill_tail(folio, 0, vip->vii_immed.vi_immed, len);
	folio_end_read(folio, true);

	return 0;
}

/*
 * Address space operations for immed files and directories.
 */
const struct address_space_operations vxfs_immed_aops = {
	.read_folio =	vxfs_immed_read_folio,
};
