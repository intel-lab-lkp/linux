// SPDX-License-Identifier: GPL-2.0
/*
 * famfs - dax file system for shared fabric-attached memory
 *
 * Copyright 2023-2025 Micron Technology, Inc.
 *
 * This file system, originally based on ramfs the dax support from xfs,
 * is intended to allow multiple host systems to mount a common file system
 * view of dax files that map to shared memory.
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/dax.h>
#include <linux/iomap.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/string.h>

#include "famfs_kfmap.h"
#include "fuse_i.h"


void
__famfs_meta_free(void *famfs_meta)
{
	struct famfs_file_meta *fmap = famfs_meta;

	if (!fmap)
		return;

	if (fmap) {
		switch (fmap->fm_extent_type) {
		case SIMPLE_DAX_EXTENT:
			kfree(fmap->se);
			break;
		case INTERLEAVED_EXTENT:
			if (fmap->ie)
				kfree(fmap->ie->ie_strips);

			kfree(fmap->ie);
			break;
		default:
			pr_err("%s: invalid fmap type\n", __func__);
			break;
		}
	}
	kfree(fmap);
}

static int
famfs_check_ext_alignment(struct famfs_meta_simple_ext *se)
{
	int errs = 0;

	if (se->dev_index != 0)
		errs++;

	/* TODO: pass in alignment so we can support the other page sizes */
	if (!IS_ALIGNED(se->ext_offset, PMD_SIZE))
		errs++;

	if (!IS_ALIGNED(se->ext_len, PMD_SIZE))
		errs++;

	return errs;
}

/**
 * famfs_meta_alloc() - Allocate famfs file metadata
 * @metap:       Pointer to an mcache_map_meta pointer
 * @ext_count:  The number of extents needed
 */
static int
famfs_meta_alloc_v3(
	void *fmap_buf,
	size_t fmap_buf_size,
	struct famfs_file_meta **metap)
{
	struct famfs_file_meta *meta = NULL;
	struct fuse_famfs_fmap_header *fmh;
	size_t extent_total = 0;
	size_t next_offset = 0;
	int errs = 0;
	int i, j;
	int rc;

	fmh = (struct fuse_famfs_fmap_header *)fmap_buf;

	/* Move past fmh in fmap_buf */
	next_offset += sizeof(*fmh);
	if (next_offset > fmap_buf_size) {
		pr_err("%s:%d: fmap_buf underflow offset/size %ld/%ld\n",
		       __func__, __LINE__, next_offset, fmap_buf_size);
		rc = -EINVAL;
		goto errout;
	}

	if (fmh->nextents < 1) {
		pr_err("%s: nextents %d < 1\n", __func__, fmh->nextents);
		rc = -EINVAL;
		goto errout;
	}

	if (fmh->nextents > FUSE_FAMFS_MAX_EXTENTS) {
		pr_err("%s: nextents %d > max (%d) 1\n",
		       __func__, fmh->nextents, FUSE_FAMFS_MAX_EXTENTS);
		rc = -E2BIG;
		goto errout;
	}

	meta = kzalloc(sizeof(*meta), GFP_KERNEL);
	if (!meta)
		return -ENOMEM;
	meta->error = false;

	meta->file_type = fmh->file_type;
	meta->file_size = fmh->file_size;
	meta->fm_extent_type = fmh->ext_type;

	switch (fmh->ext_type) {
	case FUSE_FAMFS_EXT_SIMPLE: {
		struct fuse_famfs_simple_ext *se_in;

		se_in = (struct fuse_famfs_simple_ext *)(fmap_buf + next_offset);

		/* Move past simple extents */
		next_offset += fmh->nextents * sizeof(*se_in);
		if (next_offset > fmap_buf_size) {
			pr_err("%s:%d: fmap_buf underflow offset/size %ld/%ld\n",
			       __func__, __LINE__, next_offset, fmap_buf_size);
			rc = -EINVAL;
			goto errout;
		}

		meta->fm_nextents = fmh->nextents;

		meta->se = kcalloc(meta->fm_nextents, sizeof(*(meta->se)),
				   GFP_KERNEL);
		if (!meta->se) {
			rc = -ENOMEM;
			goto errout;
		}

		if ((meta->fm_nextents > FUSE_FAMFS_MAX_EXTENTS) ||
		    (meta->fm_nextents < 1)) {
			rc = -EINVAL;
			goto errout;
		}

		for (i = 0; i < fmh->nextents; i++) {
			meta->se[i].dev_index  = se_in[i].se_devindex;
			meta->se[i].ext_offset = se_in[i].se_offset;
			meta->se[i].ext_len    = se_in[i].se_len;

			/* Record bitmap of referenced daxdev indices */
			meta->dev_bitmap |= (1 << meta->se[i].dev_index);

			errs += famfs_check_ext_alignment(&meta->se[i]);

			extent_total += meta->se[i].ext_len;
		}
		break;
	}

	case FUSE_FAMFS_EXT_INTERLEAVE: {
		s64 size_remainder = meta->file_size;
		struct fuse_famfs_iext *ie_in;
		int niext = fmh->nextents;

		meta->fm_niext = niext;

		/* Allocate interleaved extent */
		meta->ie = kcalloc(niext, sizeof(*(meta->ie)), GFP_KERNEL);
		if (!meta->ie) {
			rc = -ENOMEM;
			goto errout;
		}

		/*
		 * Each interleaved extent has a simple extent list of strips.
		 * Outer loop is over separate interleaved extents
		 */
		for (i = 0; i < niext; i++) {
			u64 nstrips;
			struct fuse_famfs_simple_ext *sie_in;

			/* ie_in = one interleaved extent in fmap_buf */
			ie_in = (struct fuse_famfs_iext *)
				(fmap_buf + next_offset);

			/* Move past one interleaved extent header in fmap_buf */
			next_offset += sizeof(*ie_in);
			if (next_offset > fmap_buf_size) {
				pr_err("%s:%d: fmap_buf underflow offset/size %ld/%ld\n",
				       __func__, __LINE__, next_offset, fmap_buf_size);
				rc = -EINVAL;
				goto errout;
			}

			nstrips = ie_in->ie_nstrips;
			meta->ie[i].fie_chunk_size = ie_in->ie_chunk_size;
			meta->ie[i].fie_nstrips    = ie_in->ie_nstrips;
			meta->ie[i].fie_nbytes     = ie_in->ie_nbytes;

			if (!meta->ie[i].fie_nbytes) {
				pr_err("%s: zero-length interleave!\n",
				       __func__);
				rc = -EINVAL;
				goto errout;
			}

			/* sie_in = the strip extents in fmap_buf */
			sie_in = (struct fuse_famfs_simple_ext *)
				(fmap_buf + next_offset);

			/* Move past strip extents in fmap_buf */
			next_offset += nstrips * sizeof(*sie_in);
			if (next_offset > fmap_buf_size) {
				pr_err("%s:%d: fmap_buf underflow offset/size %ld/%ld\n",
				       __func__, __LINE__, next_offset, fmap_buf_size);
				rc = -EINVAL;
				goto errout;
			}

			if ((nstrips > FUSE_FAMFS_MAX_STRIPS) || (nstrips < 1)) {
				pr_err("%s: invalid nstrips=%lld (max=%d)\n",
				       __func__, nstrips,
				       FUSE_FAMFS_MAX_STRIPS);
				errs++;
			}

			/* Allocate strip extent array */
			meta->ie[i].ie_strips = kcalloc(ie_in->ie_nstrips,
					sizeof(meta->ie[i].ie_strips[0]),
							GFP_KERNEL);
			if (!meta->ie[i].ie_strips) {
				rc = -ENOMEM;
				goto errout;
			}

			/* Inner loop is over strips */
			for (j = 0; j < nstrips; j++) {
				struct famfs_meta_simple_ext *strips_out;
				u64 devindex = sie_in[j].se_devindex;
				u64 offset   = sie_in[j].se_offset;
				u64 len      = sie_in[j].se_len;

				strips_out = meta->ie[i].ie_strips;
				strips_out[j].dev_index  = devindex;
				strips_out[j].ext_offset = offset;
				strips_out[j].ext_len    = len;

				/* Record bitmap of referenced daxdev indices */
				meta->dev_bitmap |= (1 << devindex);

				extent_total += len;
				errs += famfs_check_ext_alignment(&strips_out[j]);
				size_remainder -= len;
			}
		}

		if (size_remainder > 0) {
			/* Sum of interleaved extent sizes is less than file size! */
			pr_err("%s: size_remainder %lld (0x%llx)\n",
			       __func__, size_remainder, size_remainder);
			rc = -EINVAL;
			goto errout;
		}
		break;
	}

	default:
		pr_err("%s: invalid ext_type %d\n", __func__, fmh->ext_type);
		rc = -EINVAL;
		goto errout;
	}

	if (errs > 0) {
		pr_err("%s: %d alignment errors found\n", __func__, errs);
		rc = -EINVAL;
		goto errout;
	}

	/* More sanity checks */
	if (extent_total < meta->file_size) {
		pr_err("%s: file size %ld larger than map size %ld\n",
		       __func__, meta->file_size, extent_total);
		rc = -EINVAL;
		goto errout;
	}

	*metap = meta;

	return 0;
errout:
	__famfs_meta_free(meta);
	return rc;
}

int
famfs_file_init_dax(
	struct fuse_mount *fm,
	struct inode *inode,
	void *fmap_buf,
	size_t fmap_size)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct famfs_file_meta *meta = NULL;
	int rc;

	if (fi->famfs_meta) {
		pr_notice("%s: i_no=%ld fmap_size=%ld ALREADY INITIALIZED\n",
			  __func__,
			  inode->i_ino, fmap_size);
		return -EEXIST;
	}

	rc = famfs_meta_alloc_v3(fmap_buf, fmap_size, &meta);
	if (rc)
		goto errout;

	/* Publish the famfs metadata on fi->famfs_meta */
	inode_lock(inode);
	if (fi->famfs_meta) {
		rc = -EEXIST; /* file already has famfs metadata */
	} else {
		if (famfs_meta_set(fi, meta) != NULL) {
			pr_err("%s: file already had metadata\n", __func__);
			rc = -EALREADY;
			goto errout;
		}
		i_size_write(inode, meta->file_size);
		inode->i_flags |= S_DAX;
	}
	inode_unlock(inode);

 errout:
	if (rc)
		__famfs_meta_free(meta);

	return rc;
}

