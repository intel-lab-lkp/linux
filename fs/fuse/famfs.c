// SPDX-License-Identifier: GPL-2.0
/*
 * famfs - dax file system for shared fabric-attached memory
 *
 * Copyright 2023-2026 Micron Technology, Inc.
 *
 * This file system, originally based on ramfs the dax support from xfs,
 * is intended to allow multiple host systems to mount a common file system
 * view of dax files that map to shared memory.
 */

#include <linux/cleanup.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/dax.h>
#include <linux/iomap.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/string.h>

#include "famfs_kfmap.h"
#include "fuse_i.h"


/***************************************************************************/

void __famfs_meta_free(void *famfs_meta)
{
	struct famfs_file_meta *fmap = famfs_meta;

	if (!fmap)
		return;

	switch (fmap->fm_extent_type) {
	case SIMPLE_DAX_EXTENT:
		kfree(fmap->se);
		break;
	case INTERLEAVED_EXTENT:
		if (fmap->ie) {
			for (int i = 0; i < fmap->fm_niext; i++)
				kfree(fmap->ie[i].ie_strips);
		}
		kfree(fmap->ie);
		break;
	default:
		pr_err("%s: invalid fmap type\n", __func__);
		break;
	}

	kfree(fmap);
}
DEFINE_FREE(__famfs_meta_free, void *, if (_T) __famfs_meta_free(_T))

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
 * famfs_fuse_meta_alloc() - Allocate famfs file metadata
 * @fmap_buf:  fmap buffer from fuse server
 * @fmap_buf_size: size of fmap buffer
 * @metap:         pointer where 'struct famfs_file_meta' is returned
 *
 * Returns: 0=success
 *          -errno=failure
 */
static int
famfs_fuse_meta_alloc(
	void *fmap_buf,
	size_t fmap_buf_size,
	struct famfs_file_meta **metap)
{
	struct fuse_famfs_fmap_header *fmh;
	size_t extent_total = 0;
	size_t next_offset = 0;
	int errs = 0;
	int i, j;

	fmh = fmap_buf;

	/* Move past fmh in fmap_buf */
	next_offset += sizeof(*fmh);
	if (next_offset > fmap_buf_size) {
		pr_err("%s:%d: fmap_buf underflow offset/size %ld/%ld\n",
		       __func__, __LINE__, next_offset, fmap_buf_size);
		return -EINVAL;
	}

	if (fmh->nextents < 1) {
		pr_err("%s: nextents %d < 1\n", __func__, fmh->nextents);
		return -ERANGE;
	}

	if (fmh->nextents > FUSE_FAMFS_MAX_EXTENTS) {
		pr_err("%s: nextents %d > max (%d) 1\n",
		       __func__, fmh->nextents, FUSE_FAMFS_MAX_EXTENTS);
		return -ERANGE;
	}

	struct famfs_file_meta *meta __free(__famfs_meta_free) = kzalloc(sizeof(*meta), GFP_KERNEL);

	if (!meta)
		return -ENOMEM;

	meta->error = false;
	meta->file_type = fmh->file_type;
	meta->file_size = fmh->file_size;
	meta->fm_extent_type = fmh->ext_type;

	switch (fmh->ext_type) {
	case FUSE_FAMFS_EXT_SIMPLE: {
		struct fuse_famfs_simple_ext *se_in;

		se_in = fmap_buf + next_offset;

		/* Move past simple extents */
		next_offset += fmh->nextents * sizeof(*se_in);
		if (next_offset > fmap_buf_size) {
			pr_err("%s:%d: fmap_buf underflow offset/size %ld/%ld\n",
			       __func__, __LINE__, next_offset, fmap_buf_size);
			return -EINVAL;
		}

		meta->fm_nextents = fmh->nextents;

		meta->se = kcalloc(meta->fm_nextents, sizeof(*(meta->se)),
				   GFP_KERNEL);
		if (!meta->se)
			return -ENOMEM;

		if ((meta->fm_nextents > FUSE_FAMFS_MAX_EXTENTS) ||
		    (meta->fm_nextents < 1))
			return -EINVAL;

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
		if (!meta->ie)
			return -ENOMEM;

		/*
		 * Each interleaved extent has a simple extent list of strips.
		 * Outer loop is over separate interleaved extents
		 */
		for (i = 0; i < niext; i++) {
			u64 nstrips;
			struct fuse_famfs_simple_ext *sie_in;

			/* ie_in = one interleaved extent in fmap_buf */
			ie_in = fmap_buf + next_offset;

			/* Move past one interleaved extent header in fmap_buf */
			next_offset += sizeof(*ie_in);
			if (next_offset > fmap_buf_size) {
				pr_err("%s:%d: fmap_buf underflow offset/size %ld/%ld\n",
				       __func__, __LINE__, next_offset,
				       fmap_buf_size);
				return -EINVAL;
			}

			if (!IS_ALIGNED(ie_in->ie_chunk_size, PMD_SIZE)) {
				pr_err("%s: chunk_size %lld not PMD-aligned\n",
				       __func__, meta->ie[i].fie_chunk_size);
				return -EINVAL;
			}

			if (ie_in->ie_nbytes == 0) {
				pr_err("%s: zero-length interleave!\n",
				       __func__);
				return -EINVAL;
			}

			nstrips = ie_in->ie_nstrips;
			meta->ie[i].fie_chunk_size = ie_in->ie_chunk_size;
			meta->ie[i].fie_nstrips    = ie_in->ie_nstrips;
			meta->ie[i].fie_nbytes     = ie_in->ie_nbytes;

			/* sie_in = the strip extents in fmap_buf */
			sie_in = fmap_buf + next_offset;

			/* Move past strip extents in fmap_buf */
			next_offset += nstrips * sizeof(*sie_in);
			if (next_offset > fmap_buf_size) {
				pr_err("%s:%d: fmap_buf underflow offset/size %ld/%ld\n",
				       __func__, __LINE__, next_offset,
				       fmap_buf_size);
				return -EINVAL;
			}

			if ((nstrips > FUSE_FAMFS_MAX_STRIPS) || (nstrips < 1)) {
				pr_err("%s: invalid nstrips=%lld (max=%d)\n",
				       __func__, nstrips,
				       FUSE_FAMFS_MAX_STRIPS);
				errs++;
			}

			/* Allocate strip extent array */
			meta->ie[i].ie_strips =
				kcalloc(ie_in->ie_nstrips,
					sizeof(meta->ie[i].ie_strips[0]),
					GFP_KERNEL);
			if (!meta->ie[i].ie_strips)
				return -ENOMEM;

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
			return -EINVAL;
		}
		break;
	}

	default:
		pr_err("%s: invalid ext_type %d\n", __func__, fmh->ext_type);
		return -EINVAL;
	}

	if (errs > 0) {
		pr_err("%s: %d alignment errors found\n", __func__, errs);
		return -EINVAL;
	}

	/* More sanity checks */
	if (extent_total < meta->file_size) {
		pr_err("%s: file size %ld larger than map size %ld\n",
		       __func__, meta->file_size, extent_total);
		return -EINVAL;
	}

	if (cmpxchg(metap, NULL, meta) != NULL) {
		pr_debug("%s: fmap race detected\n", __func__);
		return 0; /* fmap already installed */
	}
	retain_and_null_ptr(meta);

	return 0;
}

/**
 * famfs_file_init_dax() - init famfs dax file metadata
 *
 * @fm:        fuse_mount
 * @inode:     the inode
 * @fmap_buf:  fmap response message
 * @fmap_size: Size of the fmap message
 *
 * Initialize famfs metadata for a file, based on the contents of the GET_FMAP
 * response
 *
 * Return: 0=success
 *          -errno=failure
 */
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
		return 0;
	}

	rc = famfs_fuse_meta_alloc(fmap_buf, fmap_size, &meta);
	if (rc)
		goto errout;

	/* Publish the famfs metadata on fi->famfs_meta */
	inode_lock(inode);

	if (famfs_meta_set(fi, meta) == NULL) {
		i_size_write(inode, meta->file_size);
		inode->i_flags |= S_DAX;
	} else {
		pr_debug("%s: file already had metadata\n", __func__);
		__famfs_meta_free(meta);
		/* rc is 0 - the file is valid */
	}

	inode_unlock(inode);
	return 0;

errout:
	if (rc)
		__famfs_meta_free(meta);

	return rc;
}

#define FMAP_BUFSIZE PAGE_SIZE

int fuse_get_fmap(struct fuse_mount *fm, struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	size_t fmap_bufsize = FMAP_BUFSIZE;
	u64 nodeid = get_node_id(inode);
	ssize_t fmap_size;
	int rc;

	FUSE_ARGS(args);

	/* Don't retrieve if we already have the famfs metadata */
	if (fi->famfs_meta)
		return 0;

	void *fmap_buf __free(kfree) = kzalloc(FMAP_BUFSIZE, GFP_KERNEL);

	if (!fmap_buf)
		return -ENOMEM;

	args.opcode = FUSE_GET_FMAP;
	args.nodeid = nodeid;

	/* Variable-sized output buffer
	 * this causes fuse_simple_request() to return the size of the
	 * output payload
	 */
	args.out_argvar = true;
	args.out_numargs = 1;
	args.out_args[0].size = fmap_bufsize;
	args.out_args[0].value = fmap_buf;

	/* Send GET_FMAP command */
	rc = fuse_simple_request(fm, &args);
	if (rc < 0) {
		pr_err("%s: err=%d from fuse_simple_request()\n",
		       __func__, rc);
		return rc;
	}
	fmap_size = rc;

	/* Convert fmap into in-memory format and hang from inode */
	rc = famfs_file_init_dax(fm, inode, fmap_buf, fmap_size);

	return rc;
}
