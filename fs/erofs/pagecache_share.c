// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024, Alibaba Cloud
 */
#include <linux/xxhash.h>
#include <linux/refcount.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include "pagecache_share.h"
#include "internal.h"
#include "xattr.h"

#define PCSHR_FPRT_IDX	4
#define PCSHR_FPRT_NAME	"erofs.fingerprint"
#define PCSHR_FPRT_MAXLEN (sizeof(size_t) + 1024)

struct erofs_pcshr_counter {
	struct mutex mutex;
	struct kref ref;
	struct vfsmount *mnt;
};

struct erofs_pcshr_private {
	char fprt[PCSHR_FPRT_MAXLEN];
};

static struct erofs_pcshr_counter mnt_counter = {
	.mutex = __MUTEX_INITIALIZER(mnt_counter.mutex),
	.mnt = NULL,
};

static void erofs_pcshr_counter_release(struct kref *ref)
{
	struct erofs_pcshr_counter *counter = container_of(ref,
			struct erofs_pcshr_counter, ref);

	DBG_BUGON(!counter->mnt);
	kern_unmount(counter->mnt);
	counter->mnt = NULL;
}

int erofs_pcshr_init_mnt(void)
{
	int ret;
	struct vfsmount *tmp;

	mutex_lock(&mnt_counter.mutex);
	if (!mnt_counter.mnt) {
		tmp = kern_mount(&erofs_anon_fs_type);
		if (IS_ERR(tmp)) {
			ret = PTR_ERR(tmp);
			goto out;
		}
		mnt_counter.mnt = tmp;
		kref_init(&mnt_counter.ref);
	} else
		kref_get(&mnt_counter.ref);
	ret = 0;
out:
	mutex_unlock(&mnt_counter.mutex);
	return ret;
}

void erofs_pcshr_free_mnt(void)
{
	mutex_lock(&mnt_counter.mutex);
	kref_put(&mnt_counter.ref, erofs_pcshr_counter_release);
	mutex_unlock(&mnt_counter.mutex);
}

static int erofs_fprt_eq(struct inode *inode, void *data)
{
	struct erofs_pcshr_private *ano_private = inode->i_private;

	return ano_private && memcmp(ano_private->fprt, data,
			sizeof(size_t) + *(size_t *)data) == 0 ? 1 : 0;
}

static int erofs_fprt_set(struct inode *inode, void *data)
{
	struct erofs_pcshr_private *ano_private;

	ano_private = kmalloc(sizeof(struct erofs_pcshr_private), GFP_KERNEL);
	if (!ano_private)
		return -ENOMEM;
	memcpy(ano_private, data, sizeof(size_t) + *(size_t *)data);
	inode->i_private = ano_private;
	return 0;
}

int erofs_pcshr_fill_inode(struct inode *inode)
{
	struct erofs_inode *vi = EROFS_I(inode);
	/* | fingerprint length | fingerprint content | */
	char fprt[PCSHR_FPRT_MAXLEN];
	struct inode *ano_inode;
	unsigned long fprt_hash;
	size_t fprt_len;
	int ret = -1;

	vi->ano_inode = NULL;
	memset(fprt, 0, sizeof(fprt));
	fprt_len = erofs_getxattr(inode, PCSHR_FPRT_IDX, PCSHR_FPRT_NAME,
			fprt + sizeof(size_t), PCSHR_FPRT_MAXLEN);
	if (fprt_len > 0 && fprt_len <= PCSHR_FPRT_MAXLEN) {
		*(size_t *)fprt = fprt_len;
		fprt_hash = xxh32(fprt + sizeof(size_t), fprt_len, 0);
		ano_inode = iget5_locked(mnt_counter.mnt->mnt_sb, fprt_hash,
					 erofs_fprt_eq, erofs_fprt_set, fprt);
		DBG_BUGON(!ano_inode);
		vi->ano_inode = ano_inode;
		if (ano_inode->i_state & I_NEW) {
			if (erofs_inode_is_data_compressed(vi->datalayout))
				ano_inode->i_mapping->a_ops = &z_erofs_aops;
			else
				ano_inode->i_mapping->a_ops = &erofs_aops;
			ano_inode->i_size = inode->i_size;
			unlock_new_inode(ano_inode);
		}
		ret = 0;
	}
	return ret;
}

void erofs_pcshr_free_inode(struct inode *inode)
{
	struct erofs_inode *vi = EROFS_I(inode);

	if (S_ISREG(inode->i_mode) &&  vi->ano_inode) {
		iput(vi->ano_inode);
		vi->ano_inode = NULL;
	}
}

static struct file *erofs_pcshr_alloc_file(struct file *file,
					   struct inode *ano_inode)
{
	struct file *ano_file;

	ano_file = alloc_file_pseudo(ano_inode, mnt_counter.mnt,
			"[erofs_pcssh_f]", O_RDONLY, &erofs_file_fops);
	if (IS_ERR(ano_file))
		return ano_file;

	file_ra_state_init(&ano_file->f_ra, file->f_mapping);
	ano_file->private_data = EROFS_I(file_inode(file));
	return ano_file;
}

static int erofs_pcshr_file_open(struct inode *inode, struct file *file)
{
	struct file *ano_file;
	struct inode *ano_inode;
	struct erofs_inode *vi = EROFS_I(inode);

	ano_inode = vi->ano_inode;
	if (!ano_inode)
		return -EINVAL;

	ano_file = erofs_pcshr_alloc_file(file, ano_inode);
	if (IS_ERR(ano_file))
		return PTR_ERR(ano_file);

	ihold(ano_inode);
	file->private_data = (void *)ano_file;
	return 0;
}

static int erofs_pcshr_file_release(struct inode *inode, struct file *file)
{
	if (!file->private_data)
		return -EINVAL;

	fput((struct file *)file->private_data);
	file->private_data = NULL;
	return 0;
}

static ssize_t erofs_pcshr_file_read_iter(struct kiocb *iocb,
					struct iov_iter *to)
{
	struct inode __maybe_unused *inode = file_inode(iocb->ki_filp);
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

extern const struct vm_operations_struct generic_file_vm_ops;

static int erofs_pcshr_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct file *ano_file = file->private_data;

	vma_set_file(vma, ano_file);
	vma->vm_ops = &generic_file_vm_ops;
	return 0;
}

const struct file_operations erofs_pcshr_fops = {
	.open		= erofs_pcshr_file_open,
	.llseek		= generic_file_llseek,
	.read_iter	= erofs_pcshr_file_read_iter,
	.mmap		= erofs_pcshr_mmap,
	.release	= erofs_pcshr_file_release,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read	= filemap_splice_read,
};
