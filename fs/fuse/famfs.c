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
#include <linux/pagemap.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/string.h>

#include "famfs_kfmap.h"
#include "fuse_i.h"

static void famfs_set_daxdev_err(
	struct fuse_conn *fc, struct dax_device *dax_devp);

static int
famfs_dax_notify_failure(struct dax_device *dax_devp, u64 offset,
			u64 len, int mf_flags)
{
	struct fuse_conn *fc = dax_holder(dax_devp);

	famfs_set_daxdev_err(fc, dax_devp);

	return 0;
}

static const struct dax_holder_operations famfs_fuse_dax_holder_ops = {
	.notify_failure		= famfs_dax_notify_failure,
};

/*
 * DAX address_space_operations for famfs.
 * famfs doesn't need dirty tracking - writes go directly to
 * memory with no writeback required.
 */
static const struct address_space_operations famfs_dax_aops = {
	.dirty_folio	= noop_dirty_folio,
};

/*****************************************************************************/

/*
 * famfs_teardown()
 *
 * Deallocate famfs metadata for a fuse_conn
 */
void
famfs_teardown(struct fuse_conn *fc)
{
	struct famfs_dax_devlist *devlist __free(kfree) = fc->dax_devlist;
	int i;

	fc->dax_devlist = NULL;

	if (!devlist)
		return;

	if (!devlist->devlist)
		return;

	/* Close & release all the daxdevs in our table */
	for (i = 0; i < devlist->nslots; i++) {
		struct famfs_daxdev *dd = &devlist->devlist[i];

		if (!dd->valid)
			continue;

		/* Only call fs_put_dax if fs_dax_get succeeded */
		if (dd->devp) {
			if (!dd->dax_err)
				fs_put_dax(dd->devp, fc);
			put_dax(dd->devp);
		}

		kfree(dd->name);
	}
	kfree(devlist->devlist);
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
 * famfs_fuse_get_daxdev() - Retrieve info for a DAX device from fuse server
 *
 * Send a GET_DAXDEV message to the fuse server to retrieve info on a
 * dax device.
 *
 * @fm:     fuse_mount
 * @index:  the index of the dax device; daxdevs are referred to by index
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
	int rc;

	FUSE_ARGS(args);

	/* Store the daxdev in our table */
	if (index >= fc->dax_devlist->nslots) {
		pr_err("%s: index(%lld) > nslots(%d)\n",
		       __func__, index, fc->dax_devlist->nslots);
		return -EINVAL;
	}

	args.opcode = FUSE_GET_DAXDEV;
	args.nodeid = index;

	args.in_numargs = 0;

	args.out_numargs = 1;
	args.out_args[0].size = sizeof(daxdev_out);
	args.out_args[0].value = &daxdev_out;

	/* Send GET_DAXDEV command */
	rc = fuse_simple_request(fm, &args);
	if (rc) {
		pr_err("%s: rc=%d from fuse_simple_request()\n",
		       __func__, rc);
		/* Error will be that the payload is smaller than FMAP_BUFSIZE,
		 * which is the max we can handle. Empty payload handled below.
		 */
		return rc;
	}

	scoped_guard(rwsem_write, &fc->famfs_devlist_sem) {
		daxdev = &fc->dax_devlist->devlist[index];

		/* Abort if daxdev is now valid (races are possible here) */
		if (daxdev->valid) {
			pr_debug("%s: daxdev already known\n", __func__);
			return 0;
		}

		/* Verify dev is valid and can be opened and gets the devno */
		rc = famfs_verify_daxdev(daxdev_out.name, &daxdev->devno);
		if (rc) {
			pr_err("%s: rc=%d from famfs_verify_daxdev()\n",
			       __func__, rc);
			return rc;
		}

		daxdev->name = kstrdup(daxdev_out.name, GFP_KERNEL);
		if (!daxdev->name)
			return -ENOMEM;

		/* This will fail if it's not a dax device */
		daxdev->devp = dax_dev_get(daxdev->devno);
		if (!daxdev->devp) {
			pr_warn("%s: device %s not found or not dax\n",
				__func__, daxdev_out.name);
			kfree(daxdev->name);
			daxdev->name = NULL;
			return -ENODEV;
		}

		rc = fs_dax_get(daxdev->devp, fc, &famfs_fuse_dax_holder_ops);
		if (rc) {
			/* Mark as valid with dax_err to prevent retry loop.
			 * famfs_dax_err() will return -EIO on access attempts.
			 * Teardown handles this case: skips fs_put_dax, calls put_dax.
			 */
			daxdev->dax_err = 1;
			pr_err("%s: fs_dax_get(%lld) failed\n",
			       __func__, (u64)daxdev->devno);
		}

		wmb(); /* All other fields must be visible before valid */
		daxdev->valid = 1;
	}

	return 0;
}

/**
 * famfs_update_daxdev_table() - Update the daxdev table
 * @fm:   fuse_mount
 * @meta: famfs_file_meta, in-memory format, built from a GET_FMAP response
 *
 * This function is called for each new file fmap, to verify whether all
 * referenced daxdevs are already known (i.e. in the table). Any daxdev
 * indices referenced in @meta but not in the table will be retrieved via
 * famfs_fuse_get_daxdev() and added to the table
 *
 * Return: 0=success
 *         -errno=failure
 */
static int
famfs_update_daxdev_table(
	struct fuse_mount *fm,
	const struct famfs_file_meta *meta)
{
	struct famfs_dax_devlist *local_devlist;
	struct fuse_conn *fc = fm->fc;
	int indices_to_fetch[MAX_DAXDEVS];
	int n_to_fetch = 0;
	int err;

	/* First time through we will need to allocate the dax_devlist */
	if (!fc->dax_devlist) {
		local_devlist = kcalloc(1, sizeof(*fc->dax_devlist), GFP_KERNEL);
		if (!local_devlist)
			return -ENOMEM;

		local_devlist->nslots = MAX_DAXDEVS;

		local_devlist->devlist = kcalloc(MAX_DAXDEVS,
						 sizeof(struct famfs_daxdev),
						 GFP_KERNEL);
		if (!local_devlist->devlist) {
			kfree(local_devlist);
			return -ENOMEM;
		}

		/* We don't need famfs_devlist_sem here because we use cmpxchg */
		if (cmpxchg(&fc->dax_devlist, NULL, local_devlist) != NULL) {
			kfree(local_devlist->devlist);
			kfree(local_devlist); /* another thread beat us to it */
		}
	}

	/* Collect indices that need fetching while holding read lock */
	scoped_guard(rwsem_read, &fc->famfs_devlist_sem) {
		unsigned long i;

		for_each_set_bit(i, (unsigned long *)&meta->dev_bitmap, MAX_DAXDEVS) {
			if (!(fc->dax_devlist->devlist[i].valid))
				indices_to_fetch[n_to_fetch++] = i;
		}
	}

	/* Fetch needed daxdevs outside the read lock */
	for (int j = 0; j < n_to_fetch; j++) {
		err = famfs_fuse_get_daxdev(fm, indices_to_fetch[j]);
		if (err)
			pr_err("%s: failed to get daxdev=%d\n",
			       __func__, indices_to_fetch[j]);
	}

	return 0;
}

static void
famfs_set_daxdev_err(
	struct fuse_conn *fc,
	struct dax_device *dax_devp)
{
	int i;

	/* Gotta search the list by dax_devp;
	 * read lock because we're not adding or removing daxdev entries
	 */
	scoped_guard(rwsem_write, &fc->famfs_devlist_sem) {
		for (i = 0; i < fc->dax_devlist->nslots; i++) {
			if (fc->dax_devlist->devlist[i].valid) {
				struct famfs_daxdev *dd;

				dd = &fc->dax_devlist->devlist[i];
				if (dd->devp != dax_devp)
					continue;

				dd->error = true;

				pr_err("%s: memory error on daxdev %s (%d)\n",
				       __func__, dd->name, i);
				return;
			}
		}
	}
	pr_err("%s: memory err on unrecognized daxdev\n", __func__);
}

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

			/* Move past 1 interleaved extent header in fmap_buf */
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

	/* Make sure this fmap doesn't reference any unknown daxdevs */
	famfs_update_daxdev_table(fm, meta);

	/* Publish the famfs metadata on fi->famfs_meta */
	inode_lock(inode);

	if (famfs_meta_set(fi, meta) == NULL) {
		i_size_write(inode, meta->file_size);
		inode->i_flags |= S_DAX;
		inode->i_data.a_ops = &famfs_dax_aops;
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

/*********************************************************************
 * iomap_operations
 *
 * This stuff uses the iomap (dax-related) helpers to resolve file offsets to
 * offsets within a dax device.
 */

static int famfs_file_bad(struct inode *inode);

static int famfs_dax_err(struct famfs_daxdev *dd)
{
	if (!dd->valid) {
		pr_err("%s: daxdev=%s invalid\n",
		       __func__, dd->name);
		return -EIO;
	}
	if (dd->dax_err) {
		pr_err("%s: daxdev=%s dax_err\n",
		       __func__, dd->name);
		return -EIO;
	}
	if (dd->error) {
		pr_err("%s: daxdev=%s memory error\n",
		       __func__, dd->name);
		return -EHWPOISON;
	}
	return 0;
}

static int
famfs_interleave_fileofs_to_daxofs(struct inode *inode, struct iomap *iomap,
			 loff_t file_offset, off_t len, unsigned int flags)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct famfs_file_meta *meta = fi->famfs_meta;
	struct fuse_conn *fc = get_fuse_conn(inode);
	loff_t local_offset = file_offset;

	/* This function is only for extent_type INTERLEAVED_EXTENT */
	if (meta->fm_extent_type != INTERLEAVED_EXTENT) {
		pr_err("%s: bad extent type\n", __func__);
		goto err_out;
	}

	if (famfs_file_bad(inode))
		goto err_out;

	iomap->offset = file_offset;

	for (int i = 0; i < meta->fm_niext; i++) {
		struct famfs_meta_interleaved_ext *fei = &meta->ie[i];
		u64 chunk_size = fei->fie_chunk_size;
		u64 nstrips = fei->fie_nstrips;
		u64 ext_size =  min(fei->fie_nbytes, meta->file_size);

		if (!IS_ALIGNED(chunk_size, PMD_SIZE)) {
			pr_err("%s: chunk_size %lld not PMD-aligned\n",
			       __func__, meta->ie[i].fie_chunk_size);
			return -EINVAL;
		}
		if (ext_size == 0) {
			pr_err("%s: ext_size=%lld file_size=%ld\n",
			       __func__, fei->fie_nbytes, meta->file_size);
			goto err_out;
		}

		/* Is the data is in this striped extent? */
		if (local_offset < ext_size) {
			struct famfs_daxdev *dd;
			u64 chunk_num       = local_offset / chunk_size;
			u64 chunk_offset    = local_offset % chunk_size;
			u64 chunk_remainder = chunk_size - chunk_offset;
			u64 stripe_num      = chunk_num / nstrips;
			u64 strip_num       = chunk_num % nstrips;
			u64 strip_offset    = chunk_offset + (stripe_num * chunk_size);
			u64 strip_dax_ofs = fei->ie_strips[strip_num].ext_offset;
			u64 strip_devidx = fei->ie_strips[strip_num].dev_index;
			int rc;

			if (strip_devidx >= fc->dax_devlist->nslots) {
				pr_err("%s: strip_devidx %llu >= nslots %d\n",
				       __func__, strip_devidx,
				       fc->dax_devlist->nslots);
				goto err_out;
			}

			if (!fc->dax_devlist->devlist[strip_devidx].valid) {
				pr_err("%s: daxdev=%lld invalid\n", __func__,
					strip_devidx);
				goto err_out;
			}

			dd = &fc->dax_devlist->devlist[strip_devidx];

			rc = famfs_dax_err(dd);
			if (rc) {
				/* Shut down access to this file */
				meta->error = true;
				return rc;
			}

			iomap->addr    = strip_dax_ofs + strip_offset;
			iomap->offset  = file_offset;
			iomap->length  = min_t(loff_t, len, chunk_remainder);

			iomap->dax_dev = fc->dax_devlist->devlist[strip_devidx].devp;

			iomap->type    = IOMAP_MAPPED;
			iomap->flags   = flags;

			return 0;
		}
		local_offset -= ext_size; /* offset is beyond this striped extent */
	}

 err_out:
	pr_err("%s: err_out\n", __func__);

	/* We fell out the end of the extent list.
	 * Set iomap to zero length in this case, and return 0
	 * This just means that the r/w is past EOF
	 */
	iomap->addr    = 0; /* there is no valid dax device offset */
	iomap->offset  = file_offset; /* file offset */
	iomap->length  = 0; /* this had better result in no access to dax mem */
	iomap->dax_dev = NULL;
	iomap->type    = IOMAP_MAPPED;
	iomap->flags   = flags;

	return -EIO;
}

/**
 * famfs_fileofs_to_daxofs() - Resolve (file, offset, len) to (daxdev, offset, len)
 *
 * This function is called by famfs_fuse_iomap_begin() to resolve an offset in a
 * file to an offset in a dax device. This is upcalled from dax from calls to
 * both  * dax_iomap_fault() and dax_iomap_rw(). Dax finishes the job resolving
 * a fault to a specific physical page (the fault case) or doing a memcpy
 * variant (the rw case)
 *
 * Pages can be PTE (4k), PMD (2MiB) or (theoretically) PuD (1GiB)
 * (these sizes are for X86; may vary on other cpu architectures
 *
 * @inode:  The file where the fault occurred
 * @iomap:       To be filled in to indicate where to find the right memory,
 *               relative  to a dax device.
 * @file_offset: Within the file where the fault occurred (will be page boundary)
 * @len:         The length of the faulted mapping (will be a page multiple)
 *               (will be trimmed in *iomap if it's disjoint in the extent list)
 * @flags:       flags passed to famfs_fuse_iomap_begin(), and sent back via
 *               struct iomap
 *
 * Return values: 0. (info is returned in a modified @iomap struct)
 */
static int
famfs_fileofs_to_daxofs(struct inode *inode, struct iomap *iomap,
			loff_t file_offset, off_t len, unsigned int flags)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct famfs_file_meta *meta = fi->famfs_meta;
	struct fuse_conn *fc = get_fuse_conn(inode);
	loff_t local_offset = file_offset;

	if (!fc->dax_devlist) {
		pr_err("%s: null dax_devlist\n", __func__);
		goto err_out;
	}

	if (famfs_file_bad(inode))
		goto err_out;

	if (meta->fm_extent_type == INTERLEAVED_EXTENT)
		return famfs_interleave_fileofs_to_daxofs(inode, iomap,
							  file_offset,
							  len, flags);

	iomap->offset = file_offset;

	for (int i = 0; i < meta->fm_nextents; i++) {
		/* TODO: check devindex too */
		loff_t dax_ext_offset = meta->se[i].ext_offset;
		loff_t dax_ext_len    = meta->se[i].ext_len;
		u64 daxdev_idx = meta->se[i].dev_index;


		/* TODO: test that superblock and log offsets only happen
		 * with superblock and log files. Requires instrumentaiton
		 * from user space...
		 */

		/* local_offset is the offset minus the size of extents skipped
		 * so far; If local_offset < dax_ext_len, the data of interest
		 * starts in this extent
		 */
		if (local_offset < dax_ext_len) {
			loff_t ext_len_remainder = dax_ext_len - local_offset;
			struct famfs_daxdev *dd;
			int rc;

			if (daxdev_idx >= fc->dax_devlist->nslots) {
				pr_err("%s: daxdev_idx %llu >= nslots %d\n",
				       __func__, daxdev_idx,
				       fc->dax_devlist->nslots);
				goto err_out;
			}

			dd = &fc->dax_devlist->devlist[daxdev_idx];

			rc = famfs_dax_err(dd);
			if (rc) {
				/* Shut down access to this file */
				meta->error = true;
				return rc;
			}

			/*
			 * OK, we found the file metadata extent where this
			 * data begins
			 * @local_offset      - The offset within the current
			 *                      extent
			 * @ext_len_remainder - Remaining length of ext after
			 *                      skipping local_offset
			 * Outputs:
			 * iomap->addr:   the offset within the dax device where
			 *                the  data starts
			 * iomap->offset: the file offset
			 * iomap->length: the valid length resolved here
			 */
			iomap->addr    = dax_ext_offset + local_offset;
			iomap->offset  = file_offset;
			iomap->length  = min_t(loff_t, len, ext_len_remainder);

			iomap->dax_dev = fc->dax_devlist->devlist[daxdev_idx].devp;

			iomap->type    = IOMAP_MAPPED;
			iomap->flags   = flags;
			return 0;
		}
		local_offset -= dax_ext_len; /* Get ready for the next extent */
	}

 err_out:
	pr_err("%s: err_out\n", __func__);

	/* We fell out the end of the extent list.
	 * Set iomap to zero length in this case, and return 0
	 * This just means that the r/w is past EOF
	 */
	iomap->addr    = 0; /* there is no valid dax device offset */
	iomap->offset  = file_offset; /* file offset */
	iomap->length  = 0; /* this had better result in no access to dax mem */
	iomap->dax_dev = NULL;
	iomap->type    = IOMAP_MAPPED;
	iomap->flags   = flags;

	return -EIO;
}

/**
 * famfs_fuse_iomap_begin() - Handler for iomap_begin upcall from dax
 *
 * This function is pretty simple because files are
 * * never partially allocated
 * * never have holes (never sparse)
 * * never "allocate on write"
 *
 * @inode:  inode for the file being accessed
 * @offset: offset within the file
 * @length: Length being accessed at offset
 * @flags:  flags to be retured via struct iomap
 * @iomap:  iomap struct to be filled in, resolving (offset, length) to
 *          (daxdev, offset, len)
 * @srcmap: source mapping if it is a COW operation (which it is not here)
 */
static int
famfs_fuse_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
		  unsigned int flags, struct iomap *iomap, struct iomap *srcmap)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct famfs_file_meta *meta = fi->famfs_meta;
	size_t size;

	size = i_size_read(inode);

	WARN_ON(size != meta->file_size);

	return famfs_fileofs_to_daxofs(inode, iomap, offset, length, flags);
}

/* Note: We never need a special set of write_iomap_ops because famfs never
 * performs allocation on write.
 */
const struct iomap_ops famfs_iomap_ops = {
	.iomap_begin		= famfs_fuse_iomap_begin,
};

/*********************************************************************
 * vm_operations
 */
static vm_fault_t
__famfs_fuse_filemap_fault(struct vm_fault *vmf, unsigned int pe_size,
		      bool write_fault)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	vm_fault_t ret;
	unsigned long pfn;

	if (!IS_DAX(file_inode(vmf->vma->vm_file))) {
		pr_err("%s: file not marked IS_DAX!!\n", __func__);
		return VM_FAULT_SIGBUS;
	}

	if (write_fault) {
		sb_start_pagefault(inode->i_sb);
		file_update_time(vmf->vma->vm_file);
	}

	ret = dax_iomap_fault(vmf, pe_size, &pfn, NULL, &famfs_iomap_ops);
	if (ret & VM_FAULT_NEEDDSYNC)
		ret = dax_finish_sync_fault(vmf, pe_size, pfn);

	if (write_fault)
		sb_end_pagefault(inode->i_sb);

	return ret;
}

static inline bool
famfs_is_write_fault(struct vm_fault *vmf)
{
	return (vmf->flags & FAULT_FLAG_WRITE) &&
	       (vmf->vma->vm_flags & VM_SHARED);
}

static vm_fault_t
famfs_filemap_fault(struct vm_fault *vmf)
{
	return __famfs_fuse_filemap_fault(vmf, 0, famfs_is_write_fault(vmf));
}

static vm_fault_t
famfs_filemap_huge_fault(struct vm_fault *vmf, unsigned int pe_size)
{
	return __famfs_fuse_filemap_fault(vmf, pe_size,
					  famfs_is_write_fault(vmf));
}

static vm_fault_t
famfs_filemap_mkwrite(struct vm_fault *vmf)
{
	return __famfs_fuse_filemap_fault(vmf, 0, true);
}

const struct vm_operations_struct famfs_file_vm_ops = {
	.fault		= famfs_filemap_fault,
	.huge_fault	= famfs_filemap_huge_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= famfs_filemap_mkwrite,
	.pfn_mkwrite	= famfs_filemap_mkwrite,
};

/*********************************************************************
 * file_operations
 */

/**
 * famfs_file_bad() - Check for files that aren't in a valid state
 *
 * @inode: inode
 *
 * Returns: 0=success
 *          -errno=failure
 */
static int
famfs_file_bad(struct inode *inode)
{
	struct fuse_inode *fi = get_fuse_inode(inode);
	struct famfs_file_meta *meta = fi->famfs_meta;
	size_t i_size = i_size_read(inode);

	if (!meta) {
		pr_err("%s: un-initialized famfs file\n", __func__);
		return -EIO;
	}
	if (meta->error) {
		pr_debug("%s: previously detected metadata errors\n", __func__);
		return -EIO;
	}
	if (i_size != meta->file_size) {
		pr_warn("%s: i_size overwritten from %ld to %ld\n",
		       __func__, meta->file_size, i_size);
		meta->error = true;
		return -ENXIO;
	}
	if (!IS_DAX(inode)) {
		pr_debug("%s: inode %llx IS_DAX is false\n",
			 __func__, (u64)inode);
		return -ENXIO;
	}
	return 0;
}

static ssize_t
famfs_fuse_rw_prep(struct kiocb *iocb, struct iov_iter *ubuf)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	size_t i_size = i_size_read(inode);
	size_t count = iov_iter_count(ubuf);
	size_t max_count;
	ssize_t rc;

	rc = famfs_file_bad(inode);
	if (rc)
		return (ssize_t)rc;

	/* Avoid unsigned underflow if position is past EOF */
	if (iocb->ki_pos >= i_size)
		max_count = 0;
	else
		max_count = i_size - iocb->ki_pos;

	if (count > max_count)
		iov_iter_truncate(ubuf, max_count);

	if (!iov_iter_count(ubuf))
		return 0;

	return rc;
}

ssize_t
famfs_fuse_read_iter(struct kiocb *iocb, struct iov_iter	*to)
{
	ssize_t rc;

	rc = famfs_fuse_rw_prep(iocb, to);
	if (rc)
		return rc;

	if (!iov_iter_count(to))
		return 0;

	rc = dax_iomap_rw(iocb, to, &famfs_iomap_ops);

	file_accessed(iocb->ki_filp);
	return rc;
}

ssize_t
famfs_fuse_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	ssize_t rc;

	rc = famfs_fuse_rw_prep(iocb, from);
	if (rc)
		return rc;

	if (!iov_iter_count(from))
		return 0;

	return dax_iomap_rw(iocb, from, &famfs_iomap_ops);
}

int
famfs_fuse_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file_inode(file);
	ssize_t rc;

	rc = famfs_file_bad(inode);
	if (rc)
		return rc;

	file_accessed(file);
	vma->vm_ops = &famfs_file_vm_ops;
	vm_flags_set(vma, VM_HUGEPAGE);
	return 0;
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
