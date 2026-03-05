// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024, Alibaba Cloud
 */
#include "internal.h"
#include <trace/events/erofs.h>

struct erofs_fileio_rq {
	struct bio_vec bvecs[16];
	struct bio bio;
	struct kiocb iocb;
	struct super_block *sb;
	refcount_t ref;
};

struct erofs_fileio_ctx {
	struct erofs_fileio_rq *rq;
	struct erofs_device_info *dif;
};

static void erofs_fileio_ki_complete(struct kiocb *iocb, long ret)
{
	struct erofs_fileio_rq *rq =
			container_of(iocb, struct erofs_fileio_rq, iocb);
	struct folio_iter fi;

	if (ret >= 0 && ret != rq->bio.bi_iter.bi_size)
		ret = -EIO;
	if (!rq->bio.bi_end_io) {
		bio_for_each_folio_all(fi, &rq->bio) {
			DBG_BUGON(folio_test_uptodate(fi.folio));
			iomap_finish_folio_read(fi.folio, fi.offset, fi.length,
						ret < 0 ? ret : 0);
		}
	} else if (ret < 0 && !rq->bio.bi_status) {
		rq->bio.bi_status = errno_to_blk_status(ret);
	}
	bio_endio(&rq->bio);
	bio_uninit(&rq->bio);
	if (refcount_dec_and_test(&rq->ref))
		kfree(rq);
}

static void erofs_fileio_rq_submit(struct erofs_fileio_rq *rq)
{
	struct iov_iter iter;
	ssize_t ret;

	if (!rq)
		return;
	rq->iocb.ki_pos = rq->bio.bi_iter.bi_sector << SECTOR_SHIFT;
	rq->iocb.ki_ioprio = get_current_ioprio();
	rq->iocb.ki_complete = erofs_fileio_ki_complete;
	if (test_opt(&EROFS_SB(rq->sb)->opt, DIRECT_IO) &&
	    rq->iocb.ki_filp->f_mode & FMODE_CAN_ODIRECT)
		rq->iocb.ki_flags = IOCB_DIRECT;
	iov_iter_bvec(&iter, ITER_DEST, rq->bvecs, rq->bio.bi_vcnt,
		      rq->bio.bi_iter.bi_size);
	scoped_with_creds(rq->iocb.ki_filp->f_cred)
		ret = vfs_iocb_iter_read(rq->iocb.ki_filp, &rq->iocb, &iter);
	if (ret != -EIOCBQUEUED)
		erofs_fileio_ki_complete(&rq->iocb, ret);
	if (refcount_dec_and_test(&rq->ref))
		kfree(rq);
}

static struct erofs_fileio_rq *erofs_fileio_rq_alloc(struct erofs_map_dev *mdev)
{
	struct erofs_fileio_rq *rq = kzalloc_obj(*rq, GFP_KERNEL | __GFP_NOFAIL);

	bio_init(&rq->bio, NULL, rq->bvecs, ARRAY_SIZE(rq->bvecs), REQ_OP_READ);
	rq->iocb.ki_filp = mdev->m_dif->file;
	rq->sb = mdev->m_sb;
	refcount_set(&rq->ref, 2);
	return rq;
}

struct bio *erofs_fileio_bio_alloc(struct erofs_map_dev *mdev)
{
	return &erofs_fileio_rq_alloc(mdev)->bio;
}

void erofs_fileio_submit_bio(struct bio *bio)
{
	return erofs_fileio_rq_submit(container_of(bio, struct erofs_fileio_rq,
						   bio));
}

static int erofs_fileio_read_folio_range(const struct iomap_iter *iter,
		struct iomap_read_folio_ctx *ctx, size_t len)
{
	struct erofs_iomap_iter_ctx *iter_ctx = iter->private;
	struct erofs_device_info *dif = iter_ctx->dif;
	struct inode *realinode = iter_ctx ? iter_ctx->realinode : iter->inode;
	struct folio *folio = ctx->cur_folio;
	struct erofs_fileio_ctx *fileio_ctx = ctx->read_ctx;
	struct iomap *iomap = (struct iomap *)&iter->iomap;
	size_t poff = offset_in_folio(folio, iter->pos);
	loff_t pos = iter->pos;
	int ret = 0;

	if (iomap->type == IOMAP_HOLE) {
		folio_zero_range(folio, poff, len);
		return 0;
	}

	while (len > 0) {
		sector_t sector = iomap_sector(iomap, pos);
		unsigned int off = offset_in_folio(folio, pos);
		unsigned int n = min(len, folio_size(folio) - off);
		struct erofs_map_dev mdev = {};

		if (!n)
			break;
		if (!fileio_ctx->rq ||
		    fileio_ctx->dif != dif ||
		    bio_end_sector(&fileio_ctx->rq->bio) != sector) {
			erofs_fileio_rq_submit(fileio_ctx->rq);
			mdev = (struct erofs_map_dev) {
				.m_dif = dif,
				.m_sb = realinode->i_sb,
				.m_pa = (sector << SECTOR_SHIFT) + off,
			};
			fileio_ctx->dif = dif;
			fileio_ctx->rq = erofs_fileio_rq_alloc(&mdev);
			fileio_ctx->rq->bio.bi_iter.bi_sector =
				(mdev.m_dif->fsoff + mdev.m_pa) >> 9;
		}
		if (!bio_add_folio(&fileio_ctx->rq->bio, folio, n, off)) {
			erofs_fileio_rq_submit(fileio_ctx->rq);
			fileio_ctx->rq = NULL;
			continue;
		}
		pos += n;
		len -= n;
	}
	return ret;
}

static void erofs_fileio_submit_read(struct iomap_read_folio_ctx *ctx)
{
	struct erofs_fileio_ctx *fileio_ctx = ctx->read_ctx;

	erofs_fileio_rq_submit(fileio_ctx->rq);
	fileio_ctx->rq = NULL;
}

static const struct iomap_read_ops erofs_fileio_read_ops = {
	.read_folio_range = erofs_fileio_read_folio_range,
	.submit_read = erofs_fileio_submit_read,
};

static int erofs_fileio_read_folio(struct file *file, struct folio *folio)
{
	struct erofs_fileio_ctx fileio_ctx = {};
	struct iomap_read_folio_ctx read_ctx = {
		.ops = &erofs_fileio_read_ops,
		.cur_folio = folio,
		.read_ctx = &fileio_ctx,
	};
	bool need_iput;
	struct erofs_iomap_iter_ctx iter_ctx = {
		.realinode = erofs_real_inode(folio_inode(folio), &need_iput),
	};

	trace_erofs_read_folio(iter_ctx.realinode, folio, true);
	iomap_read_folio(&erofs_iomap_ops, &read_ctx, &iter_ctx);
	if (need_iput)
		iput(iter_ctx.realinode);
	return 0;
}

static void erofs_fileio_readahead(struct readahead_control *rac)
{
	struct erofs_fileio_ctx fileio_ctx = {};
	struct iomap_read_folio_ctx read_ctx = {
		.ops = &erofs_fileio_read_ops,
		.rac = rac,
		.read_ctx = &fileio_ctx,
	};
	bool need_iput;
	struct erofs_iomap_iter_ctx iter_ctx = {
		.realinode = erofs_real_inode(rac->mapping->host, &need_iput),
	};

	trace_erofs_readahead(iter_ctx.realinode, readahead_index(rac),
			      readahead_count(rac), true);
	iomap_readahead(&erofs_iomap_ops, &read_ctx, &iter_ctx);
	if (need_iput)
		iput(iter_ctx.realinode);
}

const struct address_space_operations erofs_fileio_aops = {
	.read_folio = erofs_fileio_read_folio,
	.readahead = erofs_fileio_readahead,
};
