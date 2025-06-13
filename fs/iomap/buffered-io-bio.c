// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2010 Red Hat, Inc.
 * Copyright (C) 2016-2023 Christoph Hellwig.
 */
#include <linux/bio.h>
#include <linux/buffer_head.h>
#include <linux/iomap.h>
#include <linux/writeback.h>

#include "internal.h"

int iomap_bio_read_folio_sync(loff_t block_start, struct folio *folio,
		size_t poff, size_t plen, const struct iomap *iomap)
{
	struct bio_vec bvec;
	struct bio bio;

	bio_init(&bio, iomap->bdev, &bvec, 1, REQ_OP_READ);
	bio.bi_iter.bi_sector = iomap_sector(iomap, block_start);
	bio_add_folio_nofail(&bio, folio, plen, poff);
	return submit_bio_wait(&bio);
}

static void iomap_finish_folio_write(struct inode *inode, struct folio *folio,
		size_t len)
{
	struct iomap_folio_state *ifs = folio->private;

	WARN_ON_ONCE(i_blocks_per_folio(inode, folio) > 1 && !ifs);
	WARN_ON_ONCE(ifs && atomic_read(&ifs->write_bytes_pending) <= 0);

	if (!ifs || atomic_sub_and_test(len, &ifs->write_bytes_pending))
		folio_end_writeback(folio);
}

/*
 * We're now finished for good with this ioend structure.  Update the page
 * state, release holds on bios, and finally free up memory.  Do not use the
 * ioend after this.
 */
u32 iomap_finish_ioend_buffered(struct iomap_ioend *ioend)
{
	struct inode *inode = ioend->io_inode;
	struct bio *bio = &ioend->io_bio;
	struct folio_iter fi;
	u32 folio_count = 0;

	if (ioend->io_error) {
		mapping_set_error(inode->i_mapping, ioend->io_error);
		if (!bio_flagged(bio, BIO_QUIET)) {
			pr_err_ratelimited(
"%s: writeback error on inode %lu, offset %lld, sector %llu",
				inode->i_sb->s_id, inode->i_ino,
				ioend->io_offset, ioend->io_sector);
		}
	}

	/* walk all folios in bio, ending page IO on them */
	bio_for_each_folio_all(fi, bio) {
		iomap_finish_folio_write(inode, fi.folio, fi.length);
		folio_count++;
	}

	bio_put(bio);	/* frees the ioend */
	return folio_count;
}

static void iomap_writepage_end_bio(struct bio *bio)
{
	struct iomap_ioend *ioend = iomap_ioend_from_bio(bio);

	ioend->io_error = blk_status_to_errno(bio->bi_status);
	iomap_finish_ioend_buffered(ioend);
}

static struct iomap_ioend *iomap_alloc_ioend(struct iomap_writepage_ctx *wpc,
		struct writeback_control *wbc, struct inode *inode, loff_t pos,
		u16 ioend_flags)
{
	struct bio *bio;

	bio = bio_alloc_bioset(wpc->iomap.bdev, BIO_MAX_VECS,
			       REQ_OP_WRITE | wbc_to_write_flags(wbc),
			       GFP_NOFS, &iomap_ioend_bioset);
	bio->bi_iter.bi_sector = iomap_sector(&wpc->iomap, pos);
	bio->bi_end_io = iomap_writepage_end_bio;
	bio->bi_write_hint = inode->i_write_hint;
	wbc_init_bio(wbc, bio);
	wpc->nr_folios = 0;
	return iomap_init_ioend(inode, bio, pos, ioend_flags);
}

static bool iomap_can_add_to_ioend(struct iomap_writepage_ctx *wpc, loff_t pos,
		u16 ioend_flags)
{
	if (ioend_flags & IOMAP_IOEND_BOUNDARY)
		return false;
	if ((ioend_flags & IOMAP_IOEND_NOMERGE_FLAGS) !=
	    (wpc->ioend->io_flags & IOMAP_IOEND_NOMERGE_FLAGS))
		return false;
	if (pos != wpc->ioend->io_offset + wpc->ioend->io_size)
		return false;
	if (!(wpc->iomap.flags & IOMAP_F_ANON_WRITE) &&
	    iomap_sector(&wpc->iomap, pos) !=
	    bio_end_sector(&wpc->ioend->io_bio))
		return false;
	/*
	 * Limit ioend bio chain lengths to minimise IO completion latency. This
	 * also prevents long tight loops ending page writeback on all the
	 * folios in the ioend.
	 */
	if (wpc->nr_folios >= IOEND_BATCH_SIZE)
		return false;
	return true;
}

/*
 * Test to see if we have an existing ioend structure that we could append to
 * first; otherwise finish off the current ioend and start another.
 *
 * If a new ioend is created and cached, the old ioend is submitted to the block
 * layer instantly.  Batching optimisations are provided by higher level block
 * plugging.
 *
 * At the end of a writeback pass, there will be a cached ioend remaining on the
 * writepage context that the caller will need to submit.
 */
int iomap_add_to_ioend(struct iomap_writepage_ctx *wpc,
		struct writeback_control *wbc, struct folio *folio,
		struct inode *inode, loff_t pos, loff_t end_pos,
		unsigned len)
{
	struct iomap_folio_state *ifs = folio->private;
	size_t poff = offset_in_folio(folio, pos);
	unsigned int ioend_flags = 0;
	int error;

	if (wpc->iomap.type == IOMAP_UNWRITTEN)
		ioend_flags |= IOMAP_IOEND_UNWRITTEN;
	if (wpc->iomap.flags & IOMAP_F_SHARED)
		ioend_flags |= IOMAP_IOEND_SHARED;
	if (folio_test_dropbehind(folio))
		ioend_flags |= IOMAP_IOEND_DONTCACHE;
	if (pos == wpc->iomap.offset && (wpc->iomap.flags & IOMAP_F_BOUNDARY))
		ioend_flags |= IOMAP_IOEND_BOUNDARY;

	if (!wpc->ioend || !iomap_can_add_to_ioend(wpc, pos, ioend_flags)) {
new_ioend:
		error = iomap_submit_ioend(wpc, 0);
		if (error)
			return error;
		wpc->ioend = iomap_alloc_ioend(wpc, wbc, inode, pos,
				ioend_flags);
	}

	if (!bio_add_folio(&wpc->ioend->io_bio, folio, len, poff))
		goto new_ioend;

	if (ifs)
		atomic_add(len, &ifs->write_bytes_pending);

	/*
	 * Clamp io_offset and io_size to the incore EOF so that ondisk
	 * file size updates in the ioend completion are byte-accurate.
	 * This avoids recovering files with zeroed tail regions when
	 * writeback races with appending writes:
	 *
	 *    Thread 1:                  Thread 2:
	 *    ------------               -----------
	 *    write [A, A+B]
	 *    update inode size to A+B
	 *    submit I/O [A, A+BS]
	 *                               write [A+B, A+B+C]
	 *                               update inode size to A+B+C
	 *    <I/O completes, updates disk size to min(A+B+C, A+BS)>
	 *    <power failure>
	 *
	 *  After reboot:
	 *    1) with A+B+C < A+BS, the file has zero padding in range
	 *       [A+B, A+B+C]
	 *
	 *    |<     Block Size (BS)   >|
	 *    |DDDDDDDDDDDD0000000000000|
	 *    ^           ^        ^
	 *    A          A+B     A+B+C
	 *                       (EOF)
	 *
	 *    2) with A+B+C > A+BS, the file has zero padding in range
	 *       [A+B, A+BS]
	 *
	 *    |<     Block Size (BS)   >|<     Block Size (BS)    >|
	 *    |DDDDDDDDDDDD0000000000000|00000000000000000000000000|
	 *    ^           ^             ^           ^
	 *    A          A+B           A+BS       A+B+C
	 *                             (EOF)
	 *
	 *    D = Valid Data
	 *    0 = Zero Padding
	 *
	 * Note that this defeats the ability to chain the ioends of
	 * appending writes.
	 */
	wpc->ioend->io_size += len;
	if (wpc->ioend->io_offset + wpc->ioend->io_size > end_pos)
		wpc->ioend->io_size = end_pos - wpc->ioend->io_offset;

	wbc_account_cgroup_owner(wbc, folio, len);
	return 0;
}
