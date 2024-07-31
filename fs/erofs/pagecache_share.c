// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024, Alibaba Cloud
 */
#include <linux/xarray.h>
#include <linux/mutex.h>
#include <linux/xxhash.h>
#include "internal.h"
#include "xattr.h"
#include "pagecache_share.h"

struct erofs_pcs_lhead {
	struct mutex lmutex;
	struct list_head list;
};

#define PCS_FPRT_IDX	4
#define PCS_FPRT_NAME	"erofs.fingerprint"
#define PCS_FPRT_MAXLEN 1024

DEFINE_XARRAY(pcs_xarray);

void erofs_pcs_fill_inode(struct inode *inode)
{
	struct erofs_inode *vi = EROFS_I(inode);
	char fprt[PCS_FPRT_MAXLEN];

	vi->fprt_len = erofs_getxattr(inode, PCS_FPRT_IDX, PCS_FPRT_NAME, fprt,
				      PCS_FPRT_MAXLEN);
	if (vi->fprt_len > 0 && vi->fprt_len <= PCS_FPRT_MAXLEN) {
		vi->fprt = kmalloc(vi->fprt_len, GFP_KERNEL);
		if (IS_ERR(vi->fprt)) {
			vi->fprt_len = -1;
			return;
		}
		memcpy(vi->fprt, fprt, vi->fprt_len);
		vi->fprt_hash = xxh32(vi->fprt, vi->fprt_len, 0);
	}
}

int erofs_pcs_add(struct inode *inode)
{
	struct erofs_inode *vi = EROFS_I(inode);
	struct erofs_pcs_lhead *lst;

	xa_lock(&pcs_xarray);
	lst = xa_load(&pcs_xarray, vi->fprt_hash);
	if (!lst) {
		lst = kmalloc(sizeof(struct erofs_pcs_lhead), GFP_KERNEL);
		if (!lst) {
			xa_unlock(&pcs_xarray);
			return -ENOMEM;
		}
		mutex_init(&lst->lmutex);
		INIT_LIST_HEAD(&lst->list);
		/* we have already held the xa_lock here */
		__xa_store(&pcs_xarray, vi->fprt_hash, lst, GFP_KERNEL);
	}
	xa_unlock(&pcs_xarray);

	mutex_lock(&lst->lmutex);
	list_add_tail(&vi->pcs_list, &lst->list);
	mutex_unlock(&lst->lmutex);
	return 0;
}

int erofs_pcs_remove(struct inode *inode)
{
	struct erofs_inode *vi = EROFS_I(inode);
	struct erofs_pcs_lhead *lst = xa_load(&pcs_xarray, vi->fprt_hash);

	if (!lst || list_empty(&lst->list))
		return -EINVAL;

	mutex_lock(&lst->lmutex);
	down_write(&vi->pcs_rwsem);
	list_del(&vi->pcs_list);
	up_write(&vi->pcs_rwsem);
	mutex_unlock(&lst->lmutex);

	xa_lock(&pcs_xarray);
	if (list_empty(&lst->list)) {
		__xa_erase(&pcs_xarray, vi->fprt_hash);
		kfree(lst);
	}
	xa_unlock(&pcs_xarray);
	return 0;
}

static struct inode *erofs_pcs_get4read(struct inode *inode)
{
	struct erofs_inode *vi = EROFS_I(inode), *pcs_inode = NULL, *p, *tmp;
	struct erofs_pcs_lhead *lst = xa_load(&pcs_xarray, vi->fprt_hash);

	if (!lst || list_empty(&lst->list))
		return ERR_PTR(-EINVAL);

	mutex_lock(&lst->lmutex);
	list_for_each_entry_safe(p, tmp, &lst->list, pcs_list) {
		if (vi->fprt_len == p->fprt_len &&
			!memcmp(vi->fprt, p->fprt, p->fprt_len)) {
			pcs_inode = p;
			break;
		}
	}
	if (pcs_inode)
		down_read(&pcs_inode->pcs_rwsem);
	mutex_unlock(&lst->lmutex);

	return pcs_inode ? &pcs_inode->vfs_inode : ERR_PTR(-EINVAL);
}

static int erofs_pcs_file_open(struct inode *inode, struct file *file)
{
	struct inode *pcs_inode;
	struct file *ano_file;

	pcs_inode = erofs_pcs_get4read(inode);
	if (IS_ERR(pcs_inode))
		return PTR_ERR(pcs_inode);

	ano_file = alloc_file_pseudo(pcs_inode, file->f_path.mnt, "[erofs_pcs_f]",
				     O_RDONLY, &erofs_file_fops);
	file_ra_state_init(&ano_file->f_ra, file->f_mapping);
	ihold(pcs_inode);
	file->private_data = (void *)ano_file;
	return 0;
}

static int erofs_pcs_file_release(struct inode *inode, struct file *file)
{
	struct inode *pcs_inode;

	if (!file->private_data)
		return -EINVAL;
	pcs_inode = ((struct file *)file->private_data)->f_inode;
	up_read(&EROFS_I(pcs_inode)->pcs_rwsem);
	fput((struct file *)file->private_data);
	file->private_data = NULL;
	return 0;
}

static ssize_t erofs_pcs_file_read_iter(struct kiocb *iocb,
					struct iov_iter *to)
{
	struct file *file, *ano_file;
	struct kiocb ano_iocb;
	ssize_t res;

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

static vm_fault_t erofs_pcs_fault(struct vm_fault *vmf)
{
	return filemap_fault(vmf);
}

static const struct vm_operations_struct erofs_pcs_file_vm_ops = {
	.fault = erofs_pcs_fault,
};

static int erofs_pcs_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct file *ano_file = file->private_data;

	vma_set_file(vma, ano_file);
	vma->vm_ops = &erofs_pcs_file_vm_ops;
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
