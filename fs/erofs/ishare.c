// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024, Alibaba Cloud
 */
#include <linux/xxhash.h>
#include <linux/refcount.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/ramfs.h>
#include "ishare.h"
#include "internal.h"
#include "xattr.h"

static DEFINE_MUTEX(erofs_ishare_lock);
static struct vfsmount *erofs_ishare_mnt;
static refcount_t erofs_ishare_supers;

int erofs_ishare_init(struct super_block *sb)
{
	struct vfsmount *mnt = NULL;
	struct erofs_sb_info *sbi = EROFS_SB(sb);

	if (!sbi->ishare_key)
		return 0;

	mutex_lock(&erofs_ishare_lock);
	if (erofs_ishare_mnt) {
		refcount_inc(&erofs_ishare_supers);
	} else {
		mnt = kern_mount(&erofs_anon_fs_type);
		if (!IS_ERR(mnt)) {
			erofs_ishare_mnt = mnt;
			refcount_set(&erofs_ishare_supers, 1);
		}
	}
	mutex_unlock(&erofs_ishare_lock);
	return IS_ERR(mnt) ? PTR_ERR(mnt) : 0;
}

void erofs_ishare_exit(struct super_block *sb)
{
	struct erofs_sb_info *sbi = EROFS_SB(sb);
	struct vfsmount *tmp;

	if (!sbi->ishare_key || !erofs_ishare_mnt)
		return;

	mutex_lock(&erofs_ishare_lock);
	if (refcount_dec_and_test(&erofs_ishare_supers)) {
		tmp = erofs_ishare_mnt;
		erofs_ishare_mnt = NULL;
		mutex_unlock(&erofs_ishare_lock);
		kern_unmount(tmp);
		mutex_lock(&erofs_ishare_lock);
	}
	mutex_unlock(&erofs_ishare_lock);
	kfree(sbi->ishare_key);
	sbi->ishare_key = NULL;
}

static int erofs_ishare_iget5_eq(struct inode *inode, void *data)
{
	struct erofs_inode *vi = EROFS_I(inode);

	return vi->fingerprint && memcmp(vi->fingerprint, data,
			sizeof(size_t) + *(size_t *)data) == 0;
}

static int erofs_ishare_iget5_set(struct inode *inode, void *data)
{
	struct erofs_inode *vi = EROFS_I(inode);

	vi->fingerprint = data;
	INIT_LIST_HEAD(&vi->backing_head);
	spin_lock_init(&vi->lock);
	return 0;
}

bool erofs_ishare_fill_inode(struct inode *inode)
{
	struct erofs_inode *vi = EROFS_I(inode);
	struct erofs_sb_info *sbi = EROFS_SB(inode->i_sb);
	struct inode *idedup;
	/*
	 * fingerprint layout:
	 * fingerprint length + fingerprint content (xattr_value + domain_id)
	 */
	char *ishare_key = sbi->ishare_key, *fingerprint;
	ssize_t ishare_vlen;
	unsigned long hash;
	int key_idx;

	if (!sbi->domain_id || !ishare_key)
		return false;

	key_idx = sbi->ishare_key_idx;
	ishare_vlen = erofs_getxattr(inode, key_idx, ishare_key, NULL, 0);
	if (ishare_vlen <= 0 || ishare_vlen > (1 << sbi->blkszbits))
		return false;

	fingerprint = kmalloc(sizeof(ssize_t) + ishare_vlen +
			      strlen(sbi->domain_id), GFP_KERNEL);
	if (!fingerprint)
		return false;

	*(ssize_t *)fingerprint = ishare_vlen + strlen(sbi->domain_id);
	if (ishare_vlen != erofs_getxattr(inode, key_idx, ishare_key,
					  fingerprint + sizeof(ssize_t),
					  ishare_vlen)) {
		kfree(fingerprint);
		return false;
	}

	memcpy(fingerprint + sizeof(ssize_t) + ishare_vlen,
	       sbi->domain_id, strlen(sbi->domain_id));
	hash = xxh32(fingerprint + sizeof(ssize_t),
		     ishare_vlen + strlen(sbi->domain_id), hash);
	idedup = iget5_locked(erofs_ishare_mnt->mnt_sb, hash,
			      erofs_ishare_iget5_eq, erofs_ishare_iget5_set,
			      fingerprint);
	if (!idedup) {
		kfree(fingerprint);
		return false;
	}

	INIT_LIST_HEAD(&vi->backing_link);
	vi->ishare = idedup;
	spin_lock(&EROFS_I(idedup)->lock);
	list_add(&vi->backing_link, &EROFS_I(idedup)->backing_head);
	spin_unlock(&EROFS_I(idedup)->lock);

	if (!(idedup->i_state & I_NEW)) {
		kfree(fingerprint);
		return true;
	}

	if (erofs_inode_is_data_compressed(vi->datalayout))
		idedup->i_mapping->a_ops = &z_erofs_aops;
	else
		idedup->i_mapping->a_ops = &erofs_aops;
	idedup->i_mode = vi->vfs_inode.i_mode;
	i_size_write(idedup, vi->vfs_inode.i_size);
	unlock_new_inode(idedup);
	return true;
}

void erofs_ishare_free_inode(struct inode *inode)
{
	struct erofs_inode *vi = EROFS_I(inode);
	struct inode *idedup = vi->ishare;

	if (!idedup)
		return;

	spin_lock(&EROFS_I(idedup)->lock);
	list_del(&vi->backing_link);
	spin_unlock(&EROFS_I(idedup)->lock);
	iput(idedup);
	vi->ishare = NULL;
}

static int erofs_ishare_file_open(struct inode *inode, struct file *file)
{
	struct file *realfile;
	struct inode *dedup;

	dedup = EROFS_I(inode)->ishare;
	if (!dedup)
		return -EINVAL;

	realfile = alloc_file_pseudo(dedup, erofs_ishare_mnt, "erofs_ishare_file",
				     O_RDONLY, &erofs_file_fops);
	if (IS_ERR(realfile))
		return PTR_ERR(realfile);

	file_ra_state_init(&realfile->f_ra, file->f_mapping);
	realfile->private_data = EROFS_I(inode);
	file->private_data = realfile;
	return 0;
}

static int erofs_ishare_file_release(struct inode *inode, struct file *file)
{
	struct file *realfile = file->private_data;

	if (!realfile)
		return -EINVAL;
	fput(realfile);
	realfile->private_data = NULL;
	return 0;
}

static ssize_t erofs_ishare_file_read_iter(struct kiocb *iocb,
						    struct iov_iter *to)
{
	struct file *realfile = iocb->ki_filp->private_data;
	struct inode *inode = file_inode(iocb->ki_filp);
	struct kiocb dedup_iocb;
	ssize_t nread;

	if (!realfile)
		return -EINVAL;
	if (!iov_iter_count(to))
		return 0;

	/* fallback to the original file in DAX or DIRECT mode */
	if (IS_DAX(inode) || (iocb->ki_flags & IOCB_DIRECT))
		realfile = iocb->ki_filp;

	kiocb_clone(&dedup_iocb, iocb, realfile);
	nread = filemap_read(&dedup_iocb, to, 0);
	iocb->ki_pos = dedup_iocb.ki_pos;
	touch_atime(&iocb->ki_filp->f_path);
	return nread;
}

static int erofs_ishare_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct file *realfile = file->private_data;

	if (!realfile)
		return -EINVAL;

	vma_set_file(vma, realfile);
	return generic_file_readonly_mmap(file, vma);
}

const struct file_operations erofs_ishare_fops = {
	.open		= erofs_ishare_file_open,
	.llseek		= generic_file_llseek,
	.read_iter	= erofs_ishare_file_read_iter,
	.mmap		= erofs_ishare_mmap,
	.release	= erofs_ishare_file_release,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read	= filemap_splice_read,
};
