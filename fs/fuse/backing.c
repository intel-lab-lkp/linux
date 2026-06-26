// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE passthrough to backing file.
 *
 * Copyright (c) 2023 CTERA Networks.
 */

#include "dev.h"
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

void fuse_backing_files_init(struct fuse_conn *fc)
{
	fc->backing_ids_count = 0;
	fc->backing_files_next_id = 1;
	fc->backing_htable = NULL;
}

static int fuse_backing_id_alloc(struct fuse_conn *fc, struct fuse_backing *fb)
{
	int id;
	struct fuse_backing *iterator;

	spin_lock(&fc->lock);

	if (fc->backing_ids_count == INT_MAX) {
		id = -ENOSPC;
		goto out;
	}

retry:
	id = fc->backing_files_next_id;
	fc->backing_files_next_id = (id == INT_MAX) ? 1 : id + 1;

	hash_for_each_possible(fc->backing_htable->backing_files_ht,
			       iterator, node, id) {
		if (iterator->id == id)
			goto retry;
	}

	fb->id = id;
	hash_add_rcu(fc->backing_htable->backing_files_ht, &fb->node, id);
	fc->backing_ids_count++;

out:
	spin_unlock(&fc->lock);

	return id;
}

int fuse_backing_htable_alloc(struct fuse_conn *fc)
{
	struct fuse_backing_htable *ht;

	ht = kzalloc_obj(struct fuse_backing_htable);
	if (!ht)
		return -ENOMEM;

	hash_init(ht->backing_files_ht);
	fc->backing_htable = ht;

	return 0;
}

static struct fuse_backing *fuse_backing_id_remove(struct fuse_conn *fc,
						   int id)
{
	struct fuse_backing *iterator;
	struct fuse_backing *fb = NULL;

	spin_lock(&fc->lock);
	hash_for_each_possible(fc->backing_htable->backing_files_ht,
			       iterator, node, id) {
		if (iterator->id == id) {
			hash_del_rcu(&iterator->node);
			fb = iterator;
			break;
		}
	}

	if (fb)
		fc->backing_ids_count--;

	spin_unlock(&fc->lock);

	return fb;
}

void fuse_backing_files_free(struct fuse_conn *fc)
{
	struct fuse_backing *fb;
	struct hlist_node *tmp;
	int bkt;

	if (!fc->backing_htable)
		return;

	hash_for_each_safe(fc->backing_htable->backing_files_ht,
			   bkt, tmp, fb, node) {
		hash_del_rcu(&fb->node);
		WARN_ON_ONCE(refcount_read(&fb->count) != 1);
		fuse_backing_free(fb);
	}

	kfree(fc->backing_htable);
	fc->backing_htable = NULL;
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

	fb = kmalloc_obj(struct fuse_backing);
	res = -ENOMEM;
	if (!fb)
		goto out_fput;

	fb->file = file;
	fb->cred = get_current_cred();
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

struct fuse_backing *fuse_backing_lookup(struct fuse_conn *fc, int backing_id)
{
	struct fuse_backing *iterator;
	struct fuse_backing *fb = NULL;

	rcu_read_lock();
	hash_for_each_possible_rcu(fc->backing_htable->backing_files_ht,
				   iterator, node, backing_id) {
		if (iterator->id == backing_id) {
			fb = iterator;
			break;
		}
	}

	fb = fuse_backing_get(fb);
	rcu_read_unlock();

	return fb;
}
