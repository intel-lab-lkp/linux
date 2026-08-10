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

	ret = dax_iomap_fault(vmf, order, &pfn, NULL, NULL /*&famfs_iomap_ops */);
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

	rc = dax_iomap_rw(iocb, to, NULL /*&famfs_iomap_ops */);
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

	rc = dax_iomap_rw(iocb, from, NULL /*&famfs_iomap_ops*/);
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

