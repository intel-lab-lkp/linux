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

/*
 * famfs_teardown()
 *
 * Deallocate famfs metadata for a fuse_conn
 */
void
famfs_teardown(struct fuse_conn *fc)
{
	struct famfs_dax_devlist *devlist = fc->dax_devlist;
	int i;

	fc->dax_devlist = NULL;

	if (!devlist)
		return;

	if (!devlist->devlist)
		goto out;

	/* Close & release all the daxdevs in our table */
	for (i = 0; i < devlist->nslots; i++) {
		if (devlist->devlist[i].valid && devlist->devlist[i].devp)
			fs_put_dax(devlist->devlist[i].devp, fc);
	}
	kfree(devlist->devlist);

out:
	kfree(devlist);
}

static int
famfs_verify_daxdev(const char *pathname, dev_t *devno)
{
	struct inode *inode;
	struct path path;
	int err;

	if (!pathname || !*pathname)
		return -EINVAL;

	err = kern_path(pathname, LOOKUP_FOLLOW, &path);
	if (err)
		return err;

	inode = d_backing_inode(path.dentry);
	if (!S_ISCHR(inode->i_mode)) {
		err = -EINVAL;
		goto out_path_put;
	}

	if (!may_open_dev(&path)) { /* had to export this */
		err = -EACCES;
		goto out_path_put;
	}

	*devno = inode->i_rdev;

out_path_put:
	path_put(&path);
	return err;
}

/**
 * famfs_fuse_get_daxdev()
 *
 * Send a GET_DAXDEV message to the fuse server to retrieve info on a
 * dax device.
 *
 * @fm    - fuse_mount
 * @index - the index of the dax device; daxdevs are referred to by index
 *          in fmaps, and the server resolves the index to a particular daxdev
 *
 * Returns: 0=success
 *          -errno=failure
 */
static int
famfs_fuse_get_daxdev(struct fuse_mount *fm, const u64 index)
{
	struct fuse_daxdev_out daxdev_out = { 0 };
	struct fuse_conn *fc = fm->fc;
	struct famfs_daxdev *daxdev;
	int err = 0;

	FUSE_ARGS(args);

	pr_notice("%s: index=%lld\n", __func__, index);

	/* Store the daxdev in our table */
	if (index >= fc->dax_devlist->nslots) {
		pr_err("%s: index(%lld) > nslots(%d)\n",
		       __func__, index, fc->dax_devlist->nslots);
		err = -EINVAL;
		goto out;
	}

	args.opcode = FUSE_GET_DAXDEV;
	args.nodeid = index;

	args.in_numargs = 0;

	args.out_numargs = 1;
	args.out_args[0].size = sizeof(daxdev_out);
	args.out_args[0].value = &daxdev_out;

	/* Send GET_DAXDEV command */
	err = fuse_simple_request(fm, &args);
	if (err) {
		pr_err("%s: err=%d from fuse_simple_request()\n",
		       __func__, err);
		/* Error will be that the payload is smaller than FMAP_BUFSIZE,
		 * which is the max we can handle. Empty payload handled below.
		 */
		goto out;
	}

	down_write(&fc->famfs_devlist_sem);

	daxdev = &fc->dax_devlist->devlist[index];
	pr_debug("%s: dax_devlist %llx daxdev[%lld]=%llx\n", __func__,
		 (u64)fc->dax_devlist, index, (u64)daxdev);

	/* Abort if daxdev is now valid */
	if (daxdev->valid) {
		up_write(&fc->famfs_devlist_sem);
		/* We already have a valid entry at this index */
		err = -EALREADY;
		goto out;
	}

	/* This verifies that the dev is valid and can be opened and gets the devno */
	pr_debug("%s: famfs_verify_daxdev(%s)\n", __func__, daxdev_out.name);
	err = famfs_verify_daxdev(daxdev_out.name, &daxdev->devno);
	if (err) {
		up_write(&fc->famfs_devlist_sem);
		pr_err("%s: err=%d from famfs_verify_daxdev()\n", __func__, err);
		goto out;
	}

	/* This will fail if it's not a dax device */
	pr_debug("%s: dax_dev_get(%x)\n", __func__, daxdev->devno);
	daxdev->devp = dax_dev_get(daxdev->devno);
	if (!daxdev->devp) {
		up_write(&fc->famfs_devlist_sem);
		pr_warn("%s: device %s not found or not dax\n",
			__func__, daxdev_out.name);
		err = -ENODEV;
		goto out;
	}

	daxdev->name = kstrdup(daxdev_out.name, GFP_KERNEL);
	wmb(); /* all daxdev fields must be visible before marking it valid */
	daxdev->valid = 1;

	up_write(&fc->famfs_devlist_sem);

	pr_debug("%s: daxdev(%lld, %s)=%llx opened and marked valid\n",
		 __func__, index, daxdev->name, (u64)daxdev);

out:
	return err;
}

/**
 * famfs_update_daxdev_table()
 *
 * This function is called for each new file fmap, to verify whether all
 * referenced daxdevs are already known (i.e. in the table). Any daxdev
 * indices that are not in the table will be retrieved via
 * famfs_fuse_get_daxdev()
 * @fm   - fuse_mount
 * @meta - famfs_file_meta, in-memory format, built from a GET_FMAP response
 *
 * Returns: 0=success
 *          -errno=failure
 */
static int
famfs_update_daxdev_table(
	struct fuse_mount *fm,
	const struct famfs_file_meta *meta)
{
	struct famfs_dax_devlist *local_devlist;
	struct fuse_conn *fc = fm->fc;
	int err;
	int i;

	pr_debug("%s: dev_bitmap=0x%llx\n", __func__, meta->dev_bitmap);

	/* First time through we will need to allocate the dax_devlist */
	if (!fc->dax_devlist) {
		local_devlist = kcalloc(1, sizeof(*fc->dax_devlist), GFP_KERNEL);
		if (!local_devlist)
			return -ENOMEM;

		local_devlist->nslots = MAX_DAXDEVS;
		pr_debug("%s: allocate dax_devlist=%llx\n", __func__,
			 (u64)local_devlist);

		local_devlist->devlist = kcalloc(MAX_DAXDEVS,
						 sizeof(struct famfs_daxdev),
						 GFP_KERNEL);
		if (!local_devlist->devlist) {
			kfree(local_devlist);
			return -ENOMEM;
		}

		/* We don't need the famfs_devlist_sem here because we use cmpxchg... */
		if (cmpxchg(&fc->dax_devlist, NULL, local_devlist) != NULL) {
			pr_debug("%s: aborting new devlist\n", __func__);
			kfree(local_devlist->devlist);
			kfree(local_devlist); /* another thread beat us to it */
		} else {
			pr_debug("%s: published new dax_devlist %llx / %llx\n",
				 __func__, (u64)local_devlist,
				 (u64)local_devlist->devlist);
		}
	}

	down_read(&fc->famfs_devlist_sem);
	for (i = 0; i < fc->dax_devlist->nslots; i++) {
		if (meta->dev_bitmap & (1ULL << i)) {
			/* This file meta struct references devindex i
			 * if devindex i isn't in the table; get it...
			 */
			if (!(fc->dax_devlist->devlist[i].valid)) {
				up_read(&fc->famfs_devlist_sem);

				pr_notice("%s: daxdev=%d (%llx) invalid...getting\n",
					  __func__, i,
					  (u64)(&fc->dax_devlist->devlist[i]));
				err = famfs_fuse_get_daxdev(fm, i);
				if (err)
					pr_err("%s: failed to get daxdev=%d\n",
					       __func__, i);

				down_read(&fc->famfs_devlist_sem);
			}
		}
	}
	up_read(&fc->famfs_devlist_sem);

	return 0;
}

/***************************************************************************/

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
 * famfs_fuse_meta_alloc() - Allocate famfs file metadata
 * @metap:       Pointer to an mcache_map_meta pointer
 * @ext_count:  The number of extents needed
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
		return -EINVAL;
	}

	if (fmh->nextents < 1) {
		pr_err("%s: nextents %d < 1\n", __func__, fmh->nextents);
		return -EINVAL;
	}

	if (fmh->nextents > FUSE_FAMFS_MAX_EXTENTS) {
		pr_err("%s: nextents %d > max (%d) 1\n",
		       __func__, fmh->nextents, FUSE_FAMFS_MAX_EXTENTS);
		return -E2BIG;
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

/**
 * famfs_file_init_dax()
 *
 * Initialize famfs metadata for a file, based on the contents of the GET_FMAP
 * response
 *
 * @fm        - fuse_mount
 * @inode     - the inode
 * @fmap_buf  - fmap response message
 * @fmap_size - Size of the fmap message
 *
 * Returns: 0=success
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
		return -EEXIST;
	}

	rc = famfs_fuse_meta_alloc(fmap_buf, fmap_size, &meta);
	if (rc)
		goto errout;

	/* Make sure this fmap doesn't reference any unknown daxdevs */
	famfs_update_daxdev_table(fm, meta);

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

