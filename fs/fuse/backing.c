// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE passthrough to backing file.
 *
 * Copyright (c) 2023 CTERA Networks.
 */

#include "fuse_i.h"

#include <linux/file.h>

struct fuse_backing *fuse_backing_get(struct fuse_backing *fb)
{
	if (fb && refcount_inc_not_zero(&fb->count))
		return fb;
	return NULL;
}

static void fuse_backing_free(struct fuse_backing *fb)
{
	pr_debug("%s: fb=0x%p\n", __func__, fb);

	if (fb->file)
		fput(fb->file);
	put_cred(fb->cred);
	kfree_rcu(fb, rcu);
}

void fuse_backing_put(struct fuse_backing *fb)
{
	if (fb && refcount_dec_and_test(&fb->count))
		fuse_backing_free(fb);
}

int fuse_backing_files_init(struct fuse_conn *fc)
{
	struct idr *idr;

	idr = kzalloc(sizeof(*idr), GFP_KERNEL);
	if (!idr)
		return -ENOMEM;
	idr_init(idr);
	rcu_assign_pointer(fc->backing_files_map, idr);
	return 0;
}

static int fuse_backing_id_alloc(struct fuse_conn *fc, struct fuse_backing *fb)
{
	struct idr *idr;
	int id;

	idr_preload(GFP_KERNEL);
	spin_lock(&fc->lock);
	idr = rcu_dereference_protected(fc->backing_files_map,
					lockdep_is_held(&fc->lock));
	/* FIXME: xarray might be space inefficient */
	id = idr_alloc_cyclic(idr, fb, 1, 0, GFP_ATOMIC);
	spin_unlock(&fc->lock);
	idr_preload_end();

	WARN_ON_ONCE(id == 0);
	return id;
}

static struct fuse_backing *fuse_backing_id_remove(struct fuse_conn *fc,
						   int id)
{
	struct idr *idr;
	struct fuse_backing *fb;

	spin_lock(&fc->lock);
	idr = rcu_dereference_protected(fc->backing_files_map,
					lockdep_is_held(&fc->lock));
	fb = idr_remove(idr, id);
	spin_unlock(&fc->lock);

	return fb;
}

static int fuse_backing_id_free(int id, void *p, void *data)
{
	struct fuse_backing *fb = p;

	WARN_ON_ONCE(refcount_read(&fb->count) != 1);
	fuse_backing_free(fb);
	return 0;
}

void fuse_backing_files_free(struct fuse_conn *fc)
{
	struct idr *idr = rcu_dereference_protected(fc->backing_files_map, 1);

	if (idr) {
		idr_for_each(idr, fuse_backing_id_free, NULL);
		idr_destroy(idr);
		kfree(idr);
	}
}

int fuse_backing_open(struct fuse_conn *fc, struct fuse_backing_map *map)
{
	struct file *file;
	struct super_block *backing_sb;
	struct fuse_backing *fb = NULL;
	int res;

	pr_debug("%s: fd=%d flags=0x%x\n", __func__, map->fd, map->flags);

	/* TODO: relax CAP_SYS_ADMIN once backing files are visible to lsof */
	res = -EPERM;
	if (!fc->passthrough || !capable(CAP_SYS_ADMIN))
		goto out;

	res = -EINVAL;
	if (map->flags || map->padding)
		goto out;

	file = fget_raw(map->fd);
	res = -EBADF;
	if (!file)
		goto out;

	/* read/write/splice/mmap passthrough only relevant for regular files */
	res = d_is_dir(file->f_path.dentry) ? -EISDIR : -EINVAL;
	if (!d_is_reg(file->f_path.dentry))
		goto out_fput;

	backing_sb = file_inode(file)->i_sb;
	res = -ELOOP;
	if (backing_sb->s_stack_depth >= fc->max_stack_depth)
		goto out_fput;

	fb = kmalloc(sizeof(struct fuse_backing), GFP_KERNEL);
	res = -ENOMEM;
	if (!fb)
		goto out_fput;

	fb->file = file;
	fb->cred = prepare_creds();
	refcount_set(&fb->count, 1);

	res = fuse_backing_id_alloc(fc, fb);
	if (res < 0) {
		fuse_backing_free(fb);
		fb = NULL;
	}

out:
	pr_debug("%s: fb=0x%p, ret=%i\n", __func__, fb, res);

	return res;

out_fput:
	fput(file);
	goto out;
}

int fuse_backing_close(struct fuse_conn *fc, int backing_id)
{
	struct fuse_backing *fb = NULL;
	int err;

	pr_debug("%s: backing_id=%d\n", __func__, backing_id);

	/* TODO: relax CAP_SYS_ADMIN once backing files are visible to lsof */
	err = -EPERM;
	if (!fc->passthrough || !capable(CAP_SYS_ADMIN))
		goto out;

	err = -EINVAL;
	if (backing_id <= 0)
		goto out;

	err = -ENOENT;
	fb = fuse_backing_id_remove(fc, backing_id);
	if (!fb)
		goto out;

	fuse_backing_put(fb);
	err = 0;
out:
	pr_debug("%s: fb=0x%p, err=%i\n", __func__, fb, err);

	return err;
}

int fuse_backing_close_all(struct fuse_conn *fc)
{
	struct idr *old_idr, *new_idr;
	struct fuse_backing *fb;
	int id;

	if (!fc->passthrough || !capable(CAP_SYS_ADMIN))
		return -EPERM;

	new_idr = kzalloc(sizeof(*new_idr), GFP_KERNEL);
	if (!new_idr)
		return -ENOMEM;

	idr_init(new_idr);

	/*
	 * Atomically exchange the old IDR with a new empty one under lock.
	 * This avoids long lock hold times and races with concurrent
	 * open/close operations.
	 */
	spin_lock(&fc->lock);
	old_idr = rcu_replace_pointer(fc->backing_files_map, new_idr,
				      lockdep_is_held(&fc->lock));
	spin_unlock(&fc->lock);

	/*
	 * Ensure all concurrent RCU readers complete before releasing backing
	 * files, so any in-flight lookups can safely take references.
	 */
	synchronize_rcu();

	if (old_idr) {
		idr_for_each_entry(old_idr, fb, id)
			fuse_backing_put(fb);

		idr_destroy(old_idr);
		kfree(old_idr);
	}

	return 0;
}

struct fuse_backing *fuse_backing_lookup(struct fuse_conn *fc, int backing_id)
{
	struct idr *idr;
	struct fuse_backing *fb;

	rcu_read_lock();
	idr = rcu_dereference(fc->backing_files_map);
	fb = idr ? idr_find(idr, backing_id) : NULL;
	fb = fuse_backing_get(fb);
	rcu_read_unlock();

	return fb;
}
