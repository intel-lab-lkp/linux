// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024, Alibaba Cloud
 */
#include <linux/xxhash.h>
#include <linux/refcount.h>
#include "pagecache_share.h"
#include "internal.h"
#include "xattr.h"

#define PCS_FPRT_IDX	4
#define PCS_FPRT_NAME	"erofs.fingerprint"
#define PCS_FPRT_MAXLEN (sizeof(size_t) + 1024)

static DEFINE_MUTEX(pseudo_mnt_lock);
static refcount_t pseudo_mnt_count;
static struct vfsmount *erofs_pcs_mnt;

int erofs_pcs_init_mnt(void)
{
	mutex_lock(&pseudo_mnt_lock);
	if (!erofs_pcs_mnt) {
		struct vfsmount *tmp = kern_mount(&erofs_anon_fs_type);
		if (IS_ERR(tmp))
			return PTR_ERR(tmp);
		erofs_pcs_mnt = tmp;
		refcount_set(&pseudo_mnt_count, 1);
	} else
		refcount_add(1, &pseudo_mnt_count);
	mutex_unlock(&pseudo_mnt_lock);
	return 0;
}

void erofs_pcs_free_mnt(void)
{
	mutex_lock(&pseudo_mnt_lock);
	if (refcount_dec_and_test(&pseudo_mnt_count)) {
		kern_unmount(erofs_pcs_mnt);
		erofs_pcs_mnt = NULL;
	}
	mutex_unlock(&pseudo_mnt_lock);
}

static int erofs_pcs_eq(struct inode *inode, void *data)
{
	return inode->i_private && memcmp(inode->i_private, data,
			sizeof(size_t) + *(size_t *)data) == 0 ? 1 : 0;
}

static int erofs_pcs_set_fprt(struct inode *inode, void *data)
{
	/* fprt length and content */
	inode->i_private = kmalloc(*(size_t *)data + sizeof(size_t),
				   GFP_KERNEL);
	memcpy(inode->i_private, data, sizeof(size_t) + *(size_t *)data);
	return 0;
}

void erofs_pcs_fill_inode(struct inode *inode)
{
	struct erofs_inode *vi = EROFS_I(inode);
	char fprt[PCS_FPRT_MAXLEN];
	struct inode *ano_inode;
	unsigned long fprt_hash;
	size_t fprt_len;

	vi->ano_inode = NULL;
	fprt_len = erofs_getxattr(inode, PCS_FPRT_IDX, PCS_FPRT_NAME,
			fprt + sizeof(size_t), PCS_FPRT_MAXLEN);
	if (fprt_len > 0 && fprt_len <= PCS_FPRT_MAXLEN) {
		*(size_t *)fprt = fprt_len;
		fprt_hash = xxh32(fprt + sizeof(size_t), fprt_len, 0);
		ano_inode = iget5_locked(erofs_pcs_mnt->mnt_sb, fprt_hash,
				erofs_pcs_eq, erofs_pcs_set_fprt, fprt);
		vi->ano_inode = ano_inode;
		if (ano_inode->i_state & I_NEW) {
			if (erofs_inode_is_data_compressed(vi->datalayout))
				ano_inode->i_mapping->a_ops = &z_erofs_aops;
			else
				ano_inode->i_mapping->a_ops =
						&erofs_raw_access_aops;
			ano_inode->i_size = inode->i_size;
			unlock_new_inode(ano_inode);
		}
	}
}

static struct file *erofs_pcs_alloc_file(struct file *file,
					 struct inode *ano_inode)
{
	struct file *ano_file;

	ano_file = alloc_file_pseudo(ano_inode, erofs_pcs_mnt, "[erofs_pcs_f]",
				     O_RDONLY, &erofs_file_fops);
	file_ra_state_init(&ano_file->f_ra, file->f_mapping);
	ano_file->private_data = EROFS_I(file_inode(file));
	return ano_file;
}

static int erofs_pcs_file_open(struct inode *inode, struct file *file)
{
	struct file *ano_file;
	struct inode *ano_inode;
	struct erofs_inode *vi = EROFS_I(inode);

	ano_inode = vi->ano_inode;
	if (!ano_inode)
		return -EINVAL;
	ano_file = erofs_pcs_alloc_file(file, ano_inode);
	ihold(ano_inode);
	file->private_data = (void *)ano_file;
	return 0;
}

static int erofs_pcs_file_release(struct inode *inode, struct file *file)
{
	if (!file->private_data)
		return -EINVAL;
	fput((struct file *)file->private_data);
	file->private_data = NULL;
	return 0;
}

static ssize_t erofs_pcs_file_read_iter(struct kiocb *iocb,
					struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct file *file, *ano_file;
	struct kiocb ano_iocb;
	ssize_t res;

	if (!iov_iter_count(to))
		return 0;
#ifdef CONFIG_FS_DAX
	if (IS_DAX(inode))
		return erofs_file_fops.read_iter(iocb, to);
#endif
	if (iocb->ki_flags & IOCB_DIRECT)
		return erofs_file_fops.read_iter(iocb, to);

	memcpy(&ano_iocb, iocb, sizeof(struct kiocb));
	file = iocb->ki_filp;
	ano_file = file->private_data;
	if (!ano_file)
		return -EINVAL;
	ano_iocb.ki_filp = ano_file;
	res = filemap_read(&ano_iocb, to, 0);
	memcpy(iocb, &ano_iocb, sizeof(struct kiocb));
	iocb->ki_filp = file;
	file_accessed(file);
	return res;
}

static int erofs_pcs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct file *ano_file = file->private_data;

	vma_set_file(vma, ano_file);
	vma->vm_ops = &generic_file_vm_ops;
	return 0;
}

const struct file_operations erofs_pcs_file_fops = {
	.open		= erofs_pcs_file_open,
	.llseek		= generic_file_llseek,
	.read_iter	= erofs_pcs_file_read_iter,
	.mmap		= erofs_pcs_mmap,
	.release	= erofs_pcs_file_release,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read	= filemap_splice_read,
};
