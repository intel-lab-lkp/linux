// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * iomap callack functions
 *
 * Copyright (C) 2026 Namjae Jeon <linkinjeon@kernel.org>
 */

#include <linux/iomap.h>
#include <linux/pagemap.h>

#include "exfat_raw.h"
#include "exfat_fs.h"
#include "iomap.h"

/*
 * exfat_iomap_put_folio - Put folio after iomap operation
 *
 * Called when iomap is finished with a folio zero-fills portions of
 * the folio beyond ->valid_size to prevent exposing uninitialized data.
 */
static void exfat_iomap_put_folio(struct inode *inode, loff_t pos,
		unsigned int len, struct folio *folio)
{
	struct exfat_inode_info *ei = EXFAT_I(inode);
	struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
	unsigned long sector_size = 1UL << inode->i_blkbits;
	loff_t start_down, end_up, init;

	mutex_lock(&sbi->s_lock);
	start_down = round_down(pos, sector_size);
	end_up = (pos + len - 1) | (sector_size - 1);
	init = ei->valid_size;

	if (init >= start_down && init <= end_up) {
		if (init < pos) {
			loff_t offset = offset_in_folio(folio, pos + len);

			if (offset == 0)
				offset = folio_size(folio);
			folio_zero_segments(folio,
					offset_in_folio(folio, init),
					offset_in_folio(folio, pos),
					offset,
					folio_size(folio));

		} else  {
			loff_t offset = max_t(loff_t, pos + len, init);

			offset = offset_in_folio(folio, offset);
			if (offset == 0)
				offset = folio_size(folio);
			folio_zero_segment(folio,
					offset,
					folio_size(folio));
		}
	} else if (init <= pos) {
		loff_t offset = 0, offset2 = offset_in_folio(folio, pos + len);

		if ((init >> folio_shift(folio)) == (pos >> folio_shift(folio)))
			offset = offset_in_folio(folio, init);
		if (offset2 == 0)
			offset2 = folio_size(folio);
		folio_zero_segments(folio,
				offset,
				offset_in_folio(folio, pos),
				offset2,
				folio_size(folio));
	}

	folio_unlock(folio);
	folio_put(folio);
	mutex_unlock(&sbi->s_lock);
}

const struct iomap_write_ops exfat_iomap_folio_ops = {
	.put_folio = exfat_iomap_put_folio,
};

/*
 * exfat_file_write_dio_end_io - Direct I/O write completion handler
 *
 * Updates i_size if the write extended the file. Called from the dio layer
 * after I/O completion.
 */
static int exfat_file_write_dio_end_io(struct kiocb *iocb, ssize_t size,
		int error, unsigned int flags)
{
	struct inode *inode = file_inode(iocb->ki_filp);

	if (error)
		return error;

	if (size && i_size_read(inode) < iocb->ki_pos + size) {
		i_size_write(inode, iocb->ki_pos + size);
		mark_inode_dirty(inode);
	}

	return 0;
}

const struct iomap_dio_ops exfat_write_dio_ops = {
	.end_io		= exfat_file_write_dio_end_io,
};

/*
 * exfat_read_iomap_begin - Begin mapping for reads
 *
 * Maps file range to disk location for read operations (read folio,
 * readahead, direct I/O read, etc.).
 *
 * Returns IOMAP_MAPPED for areas within ->valid_size, and IOMAP_UNWRITTEN
 * for allocated but uninitialized regions beyond ->valid_size.
 */
static int exfat_read_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
		unsigned int flags, struct iomap *iomap, struct iomap *srcmap)
{
	struct super_block *sb = inode->i_sb;
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	struct exfat_inode_info *ei = EXFAT_I(inode);
	unsigned int cluster, num_clusters = EXFAT_B_TO_CLU_ROUND_UP(length, sbi);
	loff_t cluster_offset, cluster_length;
	int err = 0;
	bool balloc = false;

	mutex_lock(&sbi->s_lock);
	iomap->bdev = inode->i_sb->s_bdev;
	iomap->offset = offset;

	err = exfat_map_cluster(inode, EXFAT_B_TO_CLU(offset, sbi),
			&cluster, &num_clusters, false, &balloc);
	if (err)
		goto out;

	cluster_offset = EXFAT_CLU_OFFSET(offset, sbi);
	cluster_length = EXFAT_CLU_TO_B(num_clusters, sbi);
	if (length > cluster_length - cluster_offset)
		iomap->length = cluster_length - cluster_offset;
	else
		iomap->length = length;

	iomap->addr = exfat_cluster_to_phys(sbi, cluster) + cluster_offset;
	if (offset >= ei->valid_size)
		iomap->type = IOMAP_UNWRITTEN;
	else
		iomap->type = IOMAP_MAPPED;

	if (!(flags & IOMAP_ZERO) && iomap->type == IOMAP_MAPPED &&
	    iomap->offset < ei->valid_size &&
	    iomap->offset + iomap->length > ei->valid_size) {
		iomap->length = round_up(ei->valid_size, 1 << inode->i_blkbits) -
			iomap->offset;
	}

	iomap->flags |= IOMAP_F_MERGED;
out:
	mutex_unlock(&sbi->s_lock);
	return err;
}

const struct iomap_ops exfat_read_iomap_ops = {
	.iomap_begin = exfat_read_iomap_begin,
};

/*
 * __exfat_write_iomap_begin - mapping logic for writes
 *
 * Maps the requested range and allocates clusters if needed.
 */
static int __exfat_write_iomap_begin(struct inode *inode, loff_t offset,
		loff_t length, struct iomap *iomap)
{
	struct super_block *sb = inode->i_sb;
	struct exfat_sb_info *sbi = EXFAT_SB(sb);
	unsigned int cluster, num_clusters;
	loff_t cluster_offset, cluster_length;
	int err;
	bool balloc = false;

	num_clusters = max(EXFAT_B_TO_CLU_ROUND_UP(offset + length, sbi) -
		EXFAT_B_TO_CLU_ROUND_UP(offset, sbi), 1);
	mutex_lock(&sbi->s_lock);
	err = exfat_map_cluster(inode, EXFAT_B_TO_CLU(offset, sbi),
			&cluster, &num_clusters, true, &balloc);
	if (err)
		goto out;

	iomap->bdev = inode->i_sb->s_bdev;
	iomap->offset = offset;

	cluster_offset = EXFAT_CLU_OFFSET(offset, sbi);
	cluster_length = EXFAT_CLU_TO_B(num_clusters, sbi);
	if (length > cluster_length - cluster_offset)
		iomap->length = cluster_length - cluster_offset;
	else
		iomap->length = length;
	iomap->addr = exfat_cluster_to_phys(sbi, cluster) + cluster_offset;
	iomap->type = IOMAP_MAPPED;
	if (balloc)
		iomap->flags = IOMAP_F_NEW;
out:
	mutex_unlock(&sbi->s_lock);
	return err;
}

/*
 * exfat_write_iomap_begin - Mapping for write operations
 *
 * Extends ->valid_size if the write starts beyond current initialized size.
 * Then performs actual block mapping (possibly allocating clusters).
 */
static int exfat_write_iomap_begin(struct inode *inode, loff_t offset,
		loff_t length, unsigned int flags, struct iomap *iomap,
		struct iomap *srcmap)
{
	int ret;

	if (EXFAT_I(inode)->valid_size < offset) {
		ret = exfat_extend_valid_size(inode, offset,
				flags & IOMAP_DIRECT ? true : false);
		if (ret)
			return ret;
	}

	ret = __exfat_write_iomap_begin(inode, offset, length, iomap);

	if (!(flags & IOMAP_DIRECT) && !ret &&
	    i_size_read(inode) < iomap->offset + iomap->length) {
		i_size_write(inode, iomap->offset + iomap->length);
		mark_inode_dirty(inode);
	}

	return ret;
}

/*
 * exfat_write_iomap_end - Update the state after write
 *
 * Extends ->valid_size to cover the newly written range.
 * Marks the inode dirty if metadata was changed.
 */
static int exfat_write_iomap_end(struct inode *inode, loff_t pos, loff_t length,
		ssize_t written, unsigned int flags, struct iomap *iomap)
{
	if (written) {
		struct exfat_sb_info *sbi = EXFAT_SB(inode->i_sb);
		struct exfat_inode_info *ei = EXFAT_I(inode);
		bool dirtied = false;
		loff_t end = pos + written;

		mutex_lock(&sbi->s_lock);
		if (ei->valid_size < end) {
			ei->valid_size = end;
			dirtied = true;
		}
		mutex_unlock(&sbi->s_lock);
		if (dirtied)
			mark_inode_dirty(inode);
	}

	return written;
}

const struct iomap_ops exfat_write_iomap_ops = {
	.iomap_begin	= exfat_write_iomap_begin,
	.iomap_end	= exfat_write_iomap_end,
};

static int exfat_mkwrite_iomap_begin(struct inode *inode, loff_t offset,
		loff_t length, unsigned int flags, struct iomap *iomap,
		struct iomap *srcmap)
{
	return __exfat_write_iomap_begin(inode, offset, length, iomap);
}

const struct iomap_ops exfat_mkwrite_iomap_ops = {
	.iomap_begin	= exfat_mkwrite_iomap_begin,
	.iomap_end	= exfat_write_iomap_end,
};

/*
 * exfat_writeback_range - Map folio during writeback
 *
 * Called for each folio during writeback. If the folio falls outside the
 * current iomap, remaps by calling read_iomap_begin.
 */
static ssize_t exfat_writeback_range(struct iomap_writepage_ctx *wpc,
		struct folio *folio, u64 offset, unsigned int len, u64 end_pos)
{
	if (offset < wpc->iomap.offset ||
	    offset >= wpc->iomap.offset + wpc->iomap.length) {
		int error;

		error = exfat_read_iomap_begin(wpc->inode, offset, len,
				0, &wpc->iomap, NULL);
		if (error)
			return error;
	}

	return iomap_add_to_ioend(wpc, folio, offset, end_pos, len);
}

const struct iomap_writeback_ops exfat_writeback_ops = {
	.writeback_range	= exfat_writeback_range,
	.writeback_submit	= iomap_ioend_writeback_submit,
};
