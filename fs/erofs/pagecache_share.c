// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024, Alibaba Cloud
 */
#include <linux/xxhash.h>
#include <linux/refcount.h>
#include <uapi/linux/fadvise.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include "pagecache_share.h"
#include "internal.h"
#include "xattr.h"

#define PCS_FPRT_IDX	4
#define PCS_FPRT_NAME	"erofs.fingerprint"
#define PCS_FPRT_MAXLEN (sizeof(size_t) + 1024)

static DEFINE_MUTEX(pseudo_mnt_lock);
static refcount_t pseudo_mnt_count;
static struct vfsmount *erofs_pcs_mnt;
struct kmem_cache *erofs_pcs_segsp;

int erofs_pcs_init_mnt(void)
{
	mutex_lock(&pseudo_mnt_lock);
	if (!erofs_pcs_mnt) {
		struct vfsmount *tmp = kern_mount(&erofs_anon_fs_type);
		if (IS_ERR(tmp))
			return PTR_ERR(tmp);
		erofs_pcs_mnt = tmp;
		refcount_set(&pseudo_mnt_count, 1);
		erofs_pcs_segsp = kmem_cache_create("erofs_pcs_segs",
				sizeof(struct interval_tree_node), 0,
				SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT, NULL);
		if (!erofs_pcs_segsp)
			return -ENOMEM;
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
		kmem_cache_destroy(erofs_pcs_segsp);
		erofs_pcs_segsp = NULL;
	}
	mutex_unlock(&pseudo_mnt_lock);
}

struct interval_tree_node *erofs_pcs_alloc_seg(void)
{
	return kmem_cache_alloc(erofs_pcs_segsp, GFP_KERNEL);
}

void erofs_pcs_free_seg(struct interval_tree_node *seg)
{
	kmem_cache_free(erofs_pcs_segsp, seg);
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
		vi->segs = RB_ROOT_CACHED;
		mutex_init(&vi->segs_mutex);
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

static int erofs_pcs_fadvise(struct file *file, loff_t offset, loff_t len, int advice)
{
	struct erofs_inode *vi = EROFS_I(file_inode(file));
	struct interval_tree_node *seg, *next_seg, *new_seg;
	struct file *ano_file = file->private_data;
	erofs_off_t start, end;
	int err = 0;
	u64 l, r;

	if (advice != POSIX_FADV_DONTNEED)
		return generic_fadvise(ano_file, offset, len, advice);

	start = offset >> PAGE_SHIFT;
	/* len = 0 means EOF */
	end = (!len ? LLONG_MAX : offset + len) >> PAGE_SHIFT;

	mutex_lock(&vi->segs_mutex);
	seg = interval_tree_iter_first(&vi->segs, start, end);
	while (seg) {
		next_seg = interval_tree_iter_next(seg, start, end);
		l = max_t(u64, seg->start | 0ULL, start);
		r = min_t(u64, seg->last | 0ULL, end);
		if (l > r)
			continue;
		(void)invalidate_mapping_pages(ano_file->f_mapping, l, r);
		if (seg->start < l) {
			new_seg = erofs_pcs_alloc_seg();
			new_seg->start = seg->start;
			new_seg->last = l;
			interval_tree_insert(new_seg, &vi->segs);
		}
		if (r < seg->last) {
			new_seg = erofs_pcs_alloc_seg();
			new_seg->start = r;
			new_seg->last = seg->last;
			interval_tree_insert(new_seg, &vi->segs);
		}
		interval_tree_remove(seg, &vi->segs);
		seg = next_seg;
	}
	mutex_unlock(&vi->segs_mutex);
	return err;
}

const struct file_operations erofs_pcs_file_fops = {
	.open		= erofs_pcs_file_open,
	.llseek		= generic_file_llseek,
	.read_iter	= erofs_pcs_file_read_iter,
	.mmap		= erofs_pcs_mmap,
	.release	= erofs_pcs_file_release,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read	= filemap_splice_read,
	.fadvise	= erofs_pcs_fadvise,
};
