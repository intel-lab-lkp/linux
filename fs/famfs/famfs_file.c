// SPDX-License-Identifier: GPL-2.0
/*
 * famfs - dax file system for shared fabric-attached memory
 *
 * Copyright 2023-2024 Micron Technology, Inc.
 *
 * This file system, originally based on ramfs the dax support from xfs,
 * is intended to allow multiple host systems to mount a common file system
 * view of dax files that map to shared memory.
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/dax.h>
#include <linux/iomap.h>
#include <linux/capability.h>

#include <linux/famfs_ioctl.h>
#include "famfs_internal.h"

/* Expose famfs kernel abi version as a read-only module parameter */
static int kabi_version = FAMFS_KABI_VERSION;
module_param(kabi_version, int, 0444);
MODULE_PARM_DESC(kabi_version, "famfs kernel abi version");

void
famfs_meta_free(struct famfs_file_meta *map)
{
	if (map) {
		switch (map->fm_extent_type) {
		case FAMFS_IOC_EXT_SIMPLE:
			kfree(map->se);
			break;
		case FAMFS_IOC_EXT_INTERLEAVE:
			if (map->ie) {
				u32 i;

				for (i = 0; i < map->fm_niext; i++)
					kfree(map->ie[i].ie_strips);
			}
			kfree(map->ie);
			break;
		default:
			break;
		}
	}
	kfree(map);
}

static int
famfs_check_ext_alignment(struct famfs_meta_simple_ext *se)
{
	int errs = 0;

	if (!IS_ALIGNED(se->ext_offset, PAGE_SIZE))
		errs++;
	if (!IS_ALIGNED(se->ext_len, PAGE_SIZE))
		errs++;

	return errs;
}

/**
 * famfs_file_init_dax() - FAMFSIOC_MAP_CREATE ioctl handler
 * @file: the un-initialized file
 * @arg:  user pointer to a self-describing fmap message
 *
 * The map-create ioctl carries the fmap as a self-describing message: a
 * struct famfs_ioc_fmap_header followed by an extent list. The message is
 * copied in, parsed into a famfs_file_meta, and published on inode->i_private.
 * Both the simple-extent and the interleaved (striped) wire forms are handled.
 * The wire layout byte-matches the fmap carried in a fuse famfs GET_FMAP reply.
 */
static int
famfs_file_init_dax(struct file *file, void __user *arg)
{
	struct famfs_ioc_fmap_header fmh;
	struct famfs_file_meta *meta = NULL;
	struct famfs_fs_info *fsi;
	struct super_block *sb;
	struct inode *inode;
	void *fmap_buf = NULL;
	size_t extent_total = 0;
	size_t next_offset;
	int errs = 0;
	int rc;
	u32 i, j;

	inode = file_inode(file);
	if (!inode)
		return -EBADF;
	if (inode->i_private)
		return -EEXIST;

	sb  = inode->i_sb;
	fsi = sb->s_fs_info;
	if (fsi->deverror)
		return -ENODEV;
	if (!famfs_opt_enabled(fsi, FAMFS_OPT_MAP_CREATE))
		return -EPERM;

	if (copy_from_user(&fmh, arg, sizeof(fmh)))
		return -EFAULT;

	if (fmh.fmap_version != FAMFS_FMAP_VERSION)
		return -EINVAL;
	if (fmh.fmap_size < sizeof(fmh))
		return -EINVAL;
	if (fmh.fmap_size > FAMFS_FMAP_MSG_MAX)
		return -EFBIG;
	if (fmh.nextents < 1)
		return -EINVAL;

	fmap_buf = kvmalloc(fmh.fmap_size, GFP_KERNEL);
	if (!fmap_buf)
		return -ENOMEM;

	if (copy_from_user(fmap_buf, arg, fmh.fmap_size)) {
		rc = -EFAULT;
		goto out;
	}
	next_offset = sizeof(fmh);	/* start of the extent list */

	meta = kzalloc_obj(*meta, GFP_KERNEL);
	if (!meta) {
		rc = -ENOMEM;
		goto out;
	}

	meta->error = false;
	meta->file_type = fmh.file_type;
	meta->file_size = fmh.file_size;
	meta->fm_extent_type = fmh.ext_type;

	switch (fmh.ext_type) {
	case FAMFS_IOC_EXT_SIMPLE: {
		struct famfs_ioc_simple_ext *se_in = fmap_buf + next_offset;

		next_offset += (size_t)fmh.nextents * sizeof(*se_in);
		if (next_offset > fmh.fmap_size) {
			rc = -EINVAL;
			goto out;
		}

		meta->fm_nextents = fmh.nextents;
		meta->se = kcalloc(meta->fm_nextents, sizeof(*meta->se),
				   GFP_KERNEL);
		if (!meta->se) {
			rc = -ENOMEM;
			goto out;
		}

		for (i = 0; i < fmh.nextents; i++) {
			meta->se[i].dev_index  = se_in[i].se_devindex;
			meta->se[i].ext_offset = se_in[i].se_offset;
			meta->se[i].ext_len    = se_in[i].se_len;

			if (meta->se[i].dev_index >= FAMFS_MAX_DAXDEVS) {
				rc = -EINVAL;
				goto out;
			}
			meta->dev_bitmap |= BIT_ULL(meta->se[i].dev_index);
			errs += famfs_check_ext_alignment(&meta->se[i]);
			extent_total += meta->se[i].ext_len;
		}
		break;
	}

	case FAMFS_IOC_EXT_INTERLEAVE: {
		s64 size_remainder = meta->file_size;
		u32 niext = fmh.nextents;

		meta->fm_niext = niext;
		meta->ie = kcalloc(niext, sizeof(*meta->ie), GFP_KERNEL);
		if (!meta->ie) {
			rc = -ENOMEM;
			goto out;
		}

		/* Outer loop is over the separate interleaved extents */
		for (i = 0; i < niext; i++) {
			struct famfs_ioc_iext *ie_in = fmap_buf + next_offset;
			struct famfs_ioc_simple_ext *sie_in;
			u64 nstrips;

			next_offset += sizeof(*ie_in);
			if (next_offset > fmh.fmap_size) {
				rc = -EINVAL;
				goto out;
			}

			/* chunk_size must be exactly one supported alloc unit */
			if (ie_in->ie_chunk_size != PAGE_SIZE &&
			    ie_in->ie_chunk_size != PMD_SIZE) {
				rc = -EINVAL;
				goto out;
			}
			if (ie_in->ie_nbytes == 0) {
				rc = -EINVAL;
				goto out;
			}

			nstrips = ie_in->ie_nstrips;
			if (nstrips < 1) {
				rc = -EINVAL;
				goto out;
			}

			meta->ie[i].fie_chunk_size = ie_in->ie_chunk_size;
			meta->ie[i].fie_nstrips    = ie_in->ie_nstrips;
			meta->ie[i].fie_nbytes     = ie_in->ie_nbytes;

			/* The strip extents follow the interleaved-ext header */
			sie_in = fmap_buf + next_offset;
			next_offset += nstrips * sizeof(*sie_in);
			if (next_offset > fmh.fmap_size) {
				rc = -EINVAL;
				goto out;
			}

			meta->ie[i].ie_strips =
				kcalloc(nstrips,
					sizeof(meta->ie[i].ie_strips[0]),
					GFP_KERNEL);
			if (!meta->ie[i].ie_strips) {
				rc = -ENOMEM;
				goto out;
			}

			/* Inner loop is over the strips */
			for (j = 0; j < nstrips; j++) {
				struct famfs_meta_simple_ext *so =
					&meta->ie[i].ie_strips[j];

				so->dev_index  = sie_in[j].se_devindex;
				so->ext_offset = sie_in[j].se_offset;
				so->ext_len    = sie_in[j].se_len;

				if (so->dev_index >= FAMFS_MAX_DAXDEVS) {
					rc = -EINVAL;
					goto out;
				}
				meta->dev_bitmap |= BIT_ULL(so->dev_index);
				errs += famfs_check_ext_alignment(so);
				extent_total += so->ext_len;
				size_remainder -= so->ext_len;
			}
		}

		if (size_remainder > 0) {
			/* Strips do not cover the whole file */
			rc = -EINVAL;
			goto out;
		}
		break;
	}

	default:
		rc = -EINVAL;
		goto out;
	}

	if (errs > 0) {
		rc = -EINVAL;
		goto out;
	}
	if (extent_total < meta->file_size) {
		rc = -EINVAL;
		goto out;
	}

	/* Publish the famfs metadata on inode->i_private */
	inode_lock(inode);
	if (inode->i_private) {
		rc = -EEXIST; /* file already has famfs metadata */
	} else {
		inode->i_private = meta;
		i_size_write(inode, meta->file_size);
		inode->i_flags |= S_DAX;
		meta = NULL; /* owned by the inode now */
		rc = 0;
	}
	inode_unlock(inode);

out:
	kvfree(fmap_buf);
	if (meta)
		famfs_meta_free(meta);
	return rc;
}

/**
 * famfs_daxdev_open() - FAMFSIOC_DAXDEV_OPEN ioctl handler
 * @file: any file in the famfs mount (the table is per-superblock)
 * @arg:  ptr to struct famfs_ioc_daxdev in user space
 *
 * Register a devdax device (identified by path) into the mount's daxdev table
 * at the caller-specified index, so files whose extents reference that index
 * can be mapped. The path is resolved by famfs_lookup_daxdev() - the same
 * helper the mount uses for the primary daxdev - so every slot is resolved
 * identically. Registering exposes raw device memory, so it requires
 * CAP_SYS_RAWIO.
 */
static int
famfs_daxdev_open(struct file *file, void __user *arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct famfs_fs_info *fsi = sb->s_fs_info;
	struct famfs_ioc_daxdev dd;
	dev_t devno;
	int rc;

	char *path __free(kfree) = NULL;

	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;

	if (copy_from_user(&dd, arg, sizeof(dd)))
		return -EFAULT;

	/* @flags is reserved; reject non-zero so it stays available */
	if (dd.flags)
		return -EINVAL;

	/*
	 * If this daxdev index is already populated there is nothing to do.
	 * The index is cluster-invariant, so a valid slot already names this
	 * device; skip the path resolution entirely. install_daxdev() rechecks
	 * ->valid under the write lock, so this is purely an optimization.
	 */
	scoped_guard(rwsem_read, &fsi->devlist_sem) {
		if (dd.daxdev_index >= fsi->dax_devlist->nslots)
			return -EINVAL;
		if (fsi->dax_devlist->devlist[dd.daxdev_index].valid)
			return 0;
	}

	if (dd.daxdev_path_len == 0 || dd.daxdev_path_len >= PATH_MAX)
		return -EINVAL;

	/* +1 so the terminating NUL is included within the bound */
	path = strndup_user((const char __user *)(uintptr_t)dd.daxdev_path,
			    dd.daxdev_path_len + 1);
	if (IS_ERR(path))
		return PTR_ERR(no_free_ptr(path));

	rc = famfs_lookup_daxdev(path, &devno);
	if (rc)
		return rc;

	/*
	 * The daxdev table is allocated at mount time (for the slot-0 primary),
	 * so it is always present here; no need to allocate it.
	 */
	rc = famfs_install_daxdev(fsi, sb, dd.daxdev_index, devno, path);
	if (rc)
		pr_debug("%s: failed to install daxdev index %llu (%s)\n",
		       __func__, dd.daxdev_index, path);
	return rc;
}

/**
 * famfs_file_ioctl() - Top-level famfs file ioctl handler
 * @file: the file
 * @cmd:  ioctl opcode
 * @arg:  ioctl opcode argument (if any)
 */
static long
famfs_file_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	struct famfs_fs_info *fsi = inode->i_sb->s_fs_info;
	long rc;

	if (fsi->deverror && (cmd != FAMFSIOC_NOP))
		return -ENODEV;

	switch (cmd) {
	case FAMFSIOC_NOP:
		rc = 0;
		break;

	case FAMFSIOC_DAXDEV_OPEN:
		rc = famfs_daxdev_open(file, (void __user *)arg);
		break;

	case FAMFSIOC_MAP_CREATE:
		rc = famfs_file_init_dax(file, (void __user *)arg);
		break;

	default:
		rc = -ENOTTY;
		break;
	}

	return rc;
}

/*********************************************************************
 * iomap_operations
 *
 * This stuff uses the iomap (dax-related) helpers to resolve file offsets to
 * offsets within a dax device.
 */

static ssize_t famfs_file_invalid(struct inode *inode);

/* Check the health of a daxdev table slot */
static int
famfs_dax_err(struct famfs_daxdev *dd)
{
	if (!dd->valid) {
		pr_debug("%s: daxdev=%s invalid\n", __func__, dd->name);
		return -EIO;
	}
	if (dd->dax_err) {
		pr_debug("%s: daxdev=%s dax_err\n", __func__, dd->name);
		return -EIO;
	}
	if (dd->error) {
		pr_debug("%s: daxdev=%s memory error\n", __func__, dd->name);
		return -EHWPOISON;
	}
	return 0;
}

/*
 * famfs_daxdev_from_index() - resolve an extent's dev_index to a health-checked
 * dax_device from the table. On success returns the dax_device and sets
 * *errp = 0; on failure returns NULL and sets *errp (< 0).
 */
static struct dax_device *
famfs_daxdev_from_index(struct famfs_fs_info *fsi, u64 dev_index, int *errp)
{
	struct famfs_dax_devlist *devlist = fsi->dax_devlist;
	struct famfs_daxdev *dd;
	int rc;

	if (!devlist || dev_index >= devlist->nslots) {
		pr_debug("%s: dev_index %llu out of range\n",
			__func__, dev_index);
		*errp = -EIO;
		return NULL;
	}
	dd = &devlist->devlist[dev_index];
	rc = famfs_dax_err(dd);
	if (rc) {
		*errp = rc;
		return NULL;
	}
	*errp = 0;
	return dd->devp;
}

static int
famfs_meta_to_dax_offset_interleaved(
	struct inode *inode,
	struct iomap *iomap,
	loff_t file_offset,
	loff_t len)
{
	struct famfs_fs_info  *fsi = inode->i_sb->s_fs_info;
	struct famfs_file_meta *meta = inode->i_private;
	loff_t local_offset = file_offset;
	int rc;
	int i;

	if (fsi->deverror || famfs_file_invalid(inode))
		goto err_out;

	/* This function is only for extent_type FAMFS_IOC_EXT_INTERLEAVE */
	if (meta->fm_extent_type != FAMFS_IOC_EXT_INTERLEAVE) {
		pr_debug("%s: bad extent type\n", __func__);
		goto err_out;
	}

	iomap->offset = file_offset;

	for (i = 0; i < meta->fm_niext; i++) {
		struct famfs_meta_interleaved_ext *fei = &meta->ie[i];
		u64 chunk_size = fei->fie_chunk_size;
		u64 nstrips = fei->fie_nstrips;
		u64 ext_size = fei->fie_nbytes;

		ext_size = min_t(u64, ext_size, meta->file_size);

		if (ext_size == 0)
			goto err_out;

		/* Is the data is in this striped extent? */
		if (local_offset < ext_size) {
			u64 chunk_num       = local_offset / chunk_size;
			u64 chunk_offset    = local_offset % chunk_size;
			u64 stripe_num      = chunk_num / nstrips;
			u64 strip_num       = chunk_num % nstrips;
			u64 chunk_remainder = chunk_size - chunk_offset;
			u64 strip_offset    = chunk_offset + (stripe_num * chunk_size);
			struct famfs_meta_simple_ext *strip = &fei->ie_strips[strip_num];
			struct dax_device *daxdev;

			/*
			 * MAP_CREATE only checks that the strips' combined
			 * length covers the file, not that each strip is large
			 * enough for the chunks striped onto it. Guard against a
			 * malformed fmap with an undersized strip so we never
			 * resolve to a dax offset past the strip's extent.
			 */
			if (strip_offset >= strip->ext_len)
				goto err_out;

			daxdev = famfs_daxdev_from_index(fsi, strip->dev_index, &rc);
			if (!daxdev) {
				meta->error = true;
				return rc;
			}

			iomap->addr    = strip->ext_offset + strip_offset;
			iomap->offset  = file_offset;
			iomap->length  = min_t(loff_t, len, chunk_remainder);
			iomap->length  = min_t(loff_t, iomap->length,
					       strip->ext_len - strip_offset);
			iomap->dax_dev = daxdev;
			iomap->type    = IOMAP_MAPPED;

			return 0;
		}
		local_offset -= ext_size; /* offset is beyond this striped extent */
	}

 err_out:
	/*
	 * We fell out the end of the extent list (access past EOF) or the file
	 * is invalid. Return -EIO: iomap requires a non-zero-length mapping on
	 * success (iomap_iter_done() warns on length == 0), so signal the error
	 * rather than returning a zero-length IOMAP_MAPPED.
	 */
	pr_debug("%s: could not resolve file_offset %lld (past EOF?)\n",
		 __func__, (long long)file_offset);

	iomap->addr    = 0; /* there is no valid dax device offset */
	iomap->offset  = file_offset; /* file offset */
	iomap->length  = 0;
	iomap->dax_dev = famfs_daxdev_from_index(fsi, 0, &rc);
	iomap->type    = IOMAP_MAPPED;

	return -EIO;
}

/**
 * famfs_meta_to_dax_offset() - Resolve (file, offset, len) to (daxdev, offset, len)
 *
 * This function is called by famfs_iomap_begin() to resolve an offset in a
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
 *
 * Return values: 0. (info is returned in a modified @iomap struct)
 */
static int
famfs_meta_to_dax_offset(
	struct inode *inode,
	struct iomap *iomap,
	loff_t file_offset,
	loff_t len)
{
	struct famfs_fs_info  *fsi = inode->i_sb->s_fs_info;
	struct famfs_file_meta *meta = inode->i_private;
	loff_t local_offset = file_offset;
	int rc;
	int i;

	if (fsi->deverror || famfs_file_invalid(inode))
		goto err_out;

	if (meta->fm_extent_type == FAMFS_IOC_EXT_INTERLEAVE)
		return famfs_meta_to_dax_offset_interleaved(inode,
					iomap, file_offset, len);

	if (meta->fm_extent_type != FAMFS_IOC_EXT_SIMPLE)
		goto err_out;

	iomap->offset = file_offset;

	for (i = 0; i < meta->fm_nextents; i++) {
		loff_t dax_ext_offset = meta->se[i].ext_offset;
		loff_t dax_ext_len    = meta->se[i].ext_len;

		if ((dax_ext_offset == 0) &&
		    (meta->file_type != FAMFS_SUPERBLOCK))
			pr_warn("%s: zero offset on non-superblock file!!\n",
				__func__);

		/* local_offset is the offset minus the size of extents skipped
		 * so far; If local_offset < dax_ext_len, the data of interest
		 * starts in this extent
		 */
		if (local_offset < dax_ext_len) {
			loff_t ext_len_remainder = dax_ext_len - local_offset;
			struct dax_device *daxdev;

			daxdev = famfs_daxdev_from_index(fsi,
						meta->se[i].dev_index, &rc);
			if (!daxdev) {
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
			iomap->dax_dev = daxdev;
			iomap->type    = IOMAP_MAPPED;

			return 0;
		}
		local_offset -= dax_ext_len; /* Get ready for the next extent */
	}

 err_out:
	/*
	 * We fell out the end of the extent list (access past EOF) or the file
	 * is in an invalid state. Return -EIO: iomap requires a non-zero-length
	 * mapping on success (iomap_iter_done() warns on length == 0), so signal
	 * the error rather than returning a zero-length IOMAP_MAPPED. dax turns
	 * this into a short read/write or a SIGBUS.
	 */
	pr_debug("%s: could not resolve file_offset %lld (past EOF?)\n",
		 __func__, (long long)file_offset);

	iomap->addr    = 0; /* there is no valid dax device offset */
	iomap->offset  = file_offset; /* file offset */
	iomap->length  = 0;
	iomap->dax_dev = famfs_daxdev_from_index(fsi, 0, &rc);
	iomap->type    = IOMAP_MAPPED;

	return -EIO;
}

/**
 * famfs_iomap_begin() - Handler for iomap_begin upcall from dax
 *
 * This function is pretty simple because files are
 * * never partially allocated
 * * never have holes (never sparse)
 * * never "allocate on write"
 *
 * @inode:  inode for the file being accessed
 * @offset: offset within the file
 * @length: Length being accessed at offset
 * @flags:
 * @iomap:  iomap struct to be filled in, resolving (offset, length) to
 *          (daxdev, offset, len)
 * @srcmap:
 */
static int
famfs_iomap_begin(
	struct inode *inode,
	loff_t offset,
	loff_t length,
	unsigned int flags,
	struct iomap *iomap,
	struct iomap *srcmap)
{
	return famfs_meta_to_dax_offset(inode, iomap, offset, length);
}

/* Note: We never need a special set of write_iomap_ops because famfs never
 * performs allocation on write.
 */
const struct iomap_ops famfs_iomap_ops = {
	.iomap_begin		= famfs_iomap_begin,
};

/*********************************************************************
 * vm_operations
 */
static vm_fault_t
__famfs_filemap_fault(
	struct vm_fault *vmf,
	unsigned int order,
	bool write_fault)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct super_block *sb = inode->i_sb;
	struct famfs_fs_info *fsi = sb->s_fs_info;
	vm_fault_t ret;
	unsigned long pfn;

	if (fsi->deverror)
		return VM_FAULT_SIGBUS;

	if (!IS_DAX(file_inode(vmf->vma->vm_file))) {
		pr_err("%s: file not marked IS_DAX!!\n", __func__);
		return VM_FAULT_SIGBUS;
	}

	if (write_fault) {
		sb_start_pagefault(inode->i_sb);
		file_update_time(vmf->vma->vm_file);
	}

	ret = dax_iomap_fault(vmf, order, &pfn, NULL, &famfs_iomap_ops);
	if (ret & VM_FAULT_NEEDDSYNC)
		ret = dax_finish_sync_fault(vmf, order, pfn);

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
	return __famfs_filemap_fault(vmf, 0, famfs_is_write_fault(vmf));
}

static vm_fault_t
famfs_filemap_huge_fault(struct vm_fault *vmf, unsigned int order)
{
	return __famfs_filemap_fault(vmf, order, famfs_is_write_fault(vmf));
}

static vm_fault_t
famfs_filemap_mkwrite(struct vm_fault *vmf)
{
	return __famfs_filemap_fault(vmf, 0, true);
}

const struct vm_operations_struct famfs_file_vm_ops = {
	.fault		= famfs_filemap_fault,
	.huge_fault	= famfs_filemap_huge_fault,
	.page_mkwrite	= famfs_filemap_mkwrite,
	.pfn_mkwrite	= famfs_filemap_mkwrite,
};

/*********************************************************************
 * file_operations
 */

/* Reject I/O to files that aren't in a valid state */
static ssize_t
famfs_file_invalid(struct inode *inode)
{
	struct famfs_file_meta *meta = inode->i_private;
	size_t i_size = i_size_read(inode);

	if (!meta) {
		pr_debug("%s: un-initialized famfs file\n", __func__);
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
		pr_debug("%s: inode %llx IS_DAX is false\n", __func__, (u64)inode);
		return -ENXIO;
	}
	return 0;
}

static ssize_t
famfs_rw_prep(struct kiocb *iocb, struct iov_iter *ubuf)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	struct super_block *sb = inode->i_sb;
	struct famfs_fs_info *fsi = sb->s_fs_info;
	size_t i_size = i_size_read(inode);
	size_t count = iov_iter_count(ubuf);
	size_t max_count;
	ssize_t rc;

	if (fsi->deverror)
		return -ENODEV;

	rc = famfs_file_invalid(inode);
	if (rc)
		return rc;

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

static ssize_t
famfs_dax_read_iter(struct kiocb *iocb, struct iov_iter	*to)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	ssize_t rc;

	/* dax_iomap_rw() requires i_rwsem held (shared for read) */
	inode_lock_shared(inode);
	rc = famfs_rw_prep(iocb, to);
	if (rc || !iov_iter_count(to)) {
		inode_unlock_shared(inode);
		return rc;
	}

	rc = dax_iomap_rw(iocb, to, &famfs_iomap_ops);
	inode_unlock_shared(inode);

	if (rc > 0)
		file_accessed(iocb->ki_filp);
	return rc;
}

/**
 * famfs_dax_write_iter()
 *
 * We need our own write-iter in order to prevent append
 *
 * @iocb:
 * @from: iterator describing the user memory source for the write
 */
static ssize_t
famfs_dax_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	struct famfs_fs_info *fsi = inode->i_sb->s_fs_info;
	ssize_t rc;

	if (!famfs_opt_enabled(fsi, FAMFS_OPT_WRITE))
		return -EPERM;

	/* dax_iomap_rw() requires i_rwsem held (exclusive for write) */
	inode_lock(inode);
	rc = famfs_rw_prep(iocb, from);
	if (rc || !iov_iter_count(from)) {
		inode_unlock(inode);
		return rc;
	}

	kiocb_modified(iocb); /* mtime/ctime + strip set[e]uid */

	rc = dax_iomap_rw(iocb, from, &famfs_iomap_ops);
	inode_unlock(inode);
	return rc;
}

static int
famfs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct famfs_fs_info *fsi = sb->s_fs_info;
	ssize_t rc;

	if (fsi->deverror)
		return -ENODEV;

	/*
	 * Gate shared-writable mappings on FAMFS_OPT_WRITE. Reject a mapping
	 * that is already writable, and strip VM_MAYWRITE from a read-only
	 * shared mapping so a later mprotect(PROT_WRITE) cannot upgrade it.
	 * This is best effort: it does not revoke mappings that already exist.
	 */
	if ((vma->vm_flags & VM_SHARED) &&
	    !famfs_opt_enabled(fsi, FAMFS_OPT_WRITE)) {
		if (vma->vm_flags & VM_WRITE)
			return -EPERM;
		vm_flags_clear(vma, VM_MAYWRITE);
	}

	rc = famfs_file_invalid(inode);
	if (rc)
		return (int)rc;

	file_accessed(file);
	vma->vm_ops = &famfs_file_vm_ops;
	vm_flags_set(vma, VM_HUGEPAGE);
	return 0;
}

const struct file_operations famfs_file_operations = {
	.owner             = THIS_MODULE,

	/* fmap metadata is immutable after MAP_CREATE, so MAP_SYNC is free */
	.fop_flags	   = FOP_MMAP_SYNC,

	/* Custom famfs operations */
	.write_iter	   = famfs_dax_write_iter,
	.read_iter	   = famfs_dax_read_iter,
	.unlocked_ioctl    = famfs_file_ioctl,
	.mmap		   = famfs_file_mmap,

	/* Force PMD alignment for mmap */
	.get_unmapped_area = thp_get_unmapped_area,

	/* Generic Operations */
	.fsync		   = noop_fsync,
	.splice_read	   = copy_splice_read,
	.splice_write	   = iter_file_splice_write,
	.llseek		   = generic_file_llseek,
};

