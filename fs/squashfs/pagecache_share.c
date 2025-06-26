// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024, Inspur
 */
#include <linux/xarray.h>
#include <linux/mutex.h>
#include <linux/xxhash.h>
#include <linux/slab.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/xattr.h>
#include <linux/uio.h>
#include <uapi/linux/fcntl.h>
#include "squashfs_fs_i.h"
#include "xattr.h"
#include "pagecache_share.h"
#include "squashfs.h"

#define PCS_FPRT_NAME  "md5sum"
#define PCS_FPRT_MAXLEN 64

static struct vfsmount *squashfs_pcs_mnt;

int squashfs_pcs_init_mnt(void)
{
	struct vfsmount *mnt;

	mnt = kern_mount(&squashfs_anon_fs_type);
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);
	squashfs_pcs_mnt = mnt;
	return 0;
}

void squashfs_pcs_mnt_exit(void)
{
	kern_unmount(squashfs_pcs_mnt);
	squashfs_pcs_mnt = NULL;
}

static int squashfs_pcs_eq(struct inode *inode, void *data)
{
	return *(unsigned long *)(inode->i_private) == *(unsigned long *)data ? 1 : 0;
}

static int squashfs_pcs_inode_set(struct inode *inode, void *data)
{
	inode->i_private = kmalloc(sizeof(unsigned long), GFP_KERNEL);
	*(unsigned long *)(inode->i_private) = *(unsigned long *)data;
	return 0;
}

int squashfs_pcs_fill_inode(struct inode *inode)
{
	struct squashfs_inode_info *sqi = squashfs_i(inode);
	struct super_block *sb = inode->i_sb;
	struct inode *ano_inode;
	char fprt[PCS_FPRT_MAXLEN];
	int fprt_len;
	const struct xattr_handler *handler = sb->s_xattr[1];

	fprt_len = handler->get(handler, NULL, inode, PCS_FPRT_NAME,
				     fprt, PCS_FPRT_MAXLEN);
	if (fprt_len < 0 || fprt_len > PCS_FPRT_MAXLEN)
		return -EINVAL;

	sqi->fprt_hash = xxh32(fprt, fprt_len, 0);
	ano_inode = iget5_locked(squashfs_pcs_mnt->mnt_sb,
				 sqi->fprt_hash, squashfs_pcs_eq,
				 squashfs_pcs_inode_set, &sqi->fprt_hash);
	if (IS_ERR(ano_inode))
		return -ENOMEM;

	if (ano_inode->i_state & I_NEW) {
		ano_inode->i_mapping = inode->i_mapping;
		ano_inode->i_size = inode->i_size;
		ano_inode->i_data.a_ops = &squashfs_aops;
		unlock_new_inode(ano_inode);
	}
	sqi->pcs_inode = ano_inode;
	return fprt_len;
}

static int squashfs_pcs_file_open(struct inode *inode, struct file *file)
{
	struct squashfs_inode_info *sqi = squashfs_i(inode);
	struct inode *pcs_inode;
	struct file *ano_file;

	pcs_inode = sqi->pcs_inode;
	if (!pcs_inode)
		return -EINVAL;

	ano_file = alloc_file_pseudo(pcs_inode, squashfs_pcs_mnt,
				     "[squashfs_pcs_f]", O_RDONLY,
				     &generic_ro_fops);
	if (!ano_file)
		return -ENOMEM;

	file_ra_state_init(&ano_file->f_ra, file->f_mapping);
	file->private_data = (void *)ano_file;
	ano_file->private_data = squashfs_i(inode);
	return 0;
}

static int squashfs_pcs_file_release(struct inode *inode, struct file *file)
{
	if (!file->private_data)
		return -EINVAL;
	fput((struct file *)file->private_data);
	file->private_data = NULL;

	return 0;
}

static ssize_t squashfs_pcs_file_read_iter(struct kiocb *iocb,
					   struct iov_iter *iter)
{
	size_t count = iov_iter_count(iter);
	struct file *backing_file = iocb->ki_filp->private_data;
	struct kiocb dedup_iocb;
	ssize_t nread;

	if (!count)
		return 0;

	kiocb_clone(&dedup_iocb, iocb, backing_file);
	nread = filemap_read(&dedup_iocb, iter, 0);
	iocb->ki_pos = dedup_iocb.ki_pos;
	touch_atime(&iocb->ki_filp->f_path);

	return nread;
}

const struct vm_operations_struct squashfs_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= filemap_page_mkwrite,
};

static int squashfs_pcs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct file *ano_file = file->private_data;

	vma_set_file(vma, ano_file);
	vma->vm_ops = &squashfs_file_vm_ops;
	return 0;
}

const struct file_operations squashfs_pcs_file_fops = {
	.open = squashfs_pcs_file_open,
	.llseek = generic_file_llseek,
	.read_iter = squashfs_pcs_file_read_iter,
	.mmap = squashfs_pcs_mmap,
	.release = squashfs_pcs_file_release,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read = filemap_splice_read,
};
