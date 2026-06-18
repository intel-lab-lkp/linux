// SPDX-License-Identifier: GPL-2.0-only
/*
 * Memory-backed EROFS support.
 *
 * Serves EROFS data directly from a contiguous kernel memory region
 * (e.g. an initrd) without going through the block layer.
 */
#include "internal.h"
#include <trace/events/erofs.h>

struct erofs_memback_rq {
	struct bio_vec bvecs[16];
	struct bio bio;
	struct erofs_sb_info *sbi;
};

static void erofs_memback_rq_submit(struct erofs_memback_rq *rq)
{
	struct erofs_sb_info *sbi;
	struct bio_vec bv;
	struct bvec_iter iter;
	loff_t pos;

	if (!rq)
		return;

	sbi = rq->sbi;
	pos = rq->bio.bi_iter.bi_sector << SECTOR_SHIFT;

	bio_for_each_segment(bv, &rq->bio, iter) {
		void *dst = bvec_kmap_local(&bv);
		unsigned long avail = sbi->memback_size - pos;
		unsigned long len = min_t(unsigned long, bv.bv_len, avail);

		if (pos < sbi->memback_size && len)
			memcpy(dst, (char *)sbi->memback_data + pos, len);
		if (len < bv.bv_len)
			memset(dst + len, 0, bv.bv_len - len);
		kunmap_local(dst);
		pos += bv.bv_len;
	}
	bio_endio(&rq->bio);
	bio_uninit(&rq->bio);
	kfree(rq);
}

static struct erofs_memback_rq *
erofs_memback_rq_alloc(struct erofs_map_dev *mdev)
{
	struct erofs_memback_rq *rq =
		kzalloc(sizeof(*rq), GFP_KERNEL | __GFP_NOFAIL);

	bio_init(&rq->bio, NULL, rq->bvecs, ARRAY_SIZE(rq->bvecs), REQ_OP_READ);
	rq->sbi = EROFS_SB(mdev->m_sb);
	return rq;
}

struct bio *erofs_memback_bio_alloc(struct erofs_map_dev *mdev)
{
	return &erofs_memback_rq_alloc(mdev)->bio;
}

void erofs_memback_submit_bio(struct bio *bio)
{
	erofs_memback_rq_submit(
		container_of(bio, struct erofs_memback_rq, bio));
}

struct erofs_memback_io {
	struct erofs_map_blocks map;
	struct erofs_map_dev dev;
	struct erofs_memback_rq *rq;
};

static int erofs_memback_scan_folio(struct erofs_memback_io *io,
				    struct inode *inode, struct folio *folio)
{
	struct erofs_sb_info *sbi = EROFS_SB(inode->i_sb);
	struct erofs_map_blocks *map = &io->map;
	unsigned int cur = 0, end = folio_size(folio), len, attached = 0;
	loff_t pos = folio_pos(folio), ofs;
	int err = 0;

	erofs_onlinefolio_init(folio);
	while (cur < end) {
		if (!in_range(pos + cur, map->m_la, map->m_llen)) {
			map->m_la = pos + cur;
			map->m_llen = end - cur;
			err = erofs_map_blocks(inode, map);
			if (err)
				break;
		}

		ofs = pos + cur - map->m_la;
		len = min_t(loff_t, map->m_llen - ofs, end - cur);
		if (map->m_flags & EROFS_MAP_META) {
			struct erofs_buf buf = __EROFS_BUF_INITIALIZER;
			void *src;

			src = erofs_read_metabuf(&buf, inode->i_sb,
						 map->m_pa + ofs,
						 erofs_inode_in_metabox(inode));
			if (IS_ERR(src)) {
				err = PTR_ERR(src);
				break;
			}
			memcpy_to_folio(folio, cur, src, len);
			erofs_put_metabuf(&buf);
		} else if (!(map->m_flags & EROFS_MAP_MAPPED)) {
			folio_zero_segment(folio, cur, cur + len);
			attached = 0;
		} else {
			loff_t pa = map->m_pa + ofs;

			if (pa + len > sbi->memback_size) {
				err = -EFSCORRUPTED;
				break;
			}
			memcpy_to_folio(folio, cur,
					(char *)sbi->memback_data + pa, len);
			attached = 1;
		}
		cur += len;
	}
	erofs_onlinefolio_end(folio, err, false);
	return err;
}

static int erofs_memback_read_folio(struct file *file, struct folio *folio)
{
	bool need_iput;
	struct inode *realinode =
		erofs_real_inode(folio_inode(folio), &need_iput);
	struct erofs_memback_io io = {};
	int err;

	trace_erofs_read_folio(realinode, folio, true);
	err = erofs_memback_scan_folio(&io, realinode, folio);
	if (need_iput)
		iput(realinode);
	return err;
}

static void erofs_memback_readahead(struct readahead_control *rac)
{
	bool need_iput;
	struct inode *realinode =
		erofs_real_inode(rac->mapping->host, &need_iput);
	struct erofs_memback_io io = {};
	struct folio *folio;
	int err;

	trace_erofs_readahead(realinode, readahead_index(rac),
			      readahead_count(rac), true);
	while ((folio = readahead_folio(rac))) {
		err = erofs_memback_scan_folio(&io, realinode, folio);
		if (err && err != -EINTR)
			erofs_err(realinode->i_sb,
				  "readahead error at folio %lu @ nid %llu",
				  folio->index, EROFS_I(realinode)->nid);
	}
	if (need_iput)
		iput(realinode);
}

const struct address_space_operations erofs_memback_aops = {
	.read_folio = erofs_memback_read_folio,
	.readahead = erofs_memback_readahead,
};
