// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE passthrough to backing file.
 *
 * Copyright (c) 2023 CTERA Networks.
 */

#include "fuse_i.h"
#include "fuse_trace.h"

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
	idr_init(&fc->backing_files_map);
}

static int fuse_backing_id_alloc(struct fuse_conn *fc, struct fuse_backing *fb)
{
	const unsigned long max =
		fb->ops->id_end > 0 ? fb->ops->id_end - 1 : INT_MAX;
	int id;

	WARN_ON_ONCE(fb->ops->id_start < 1);
	fb->id = fb->ops->id_start;

	idr_preload(GFP_KERNEL);
	spin_lock(&fc->lock);
	/* FIXME: xarray might be space inefficient */
	id = idr_alloc_u32(&fc->backing_files_map, fb, &fb->id, max,
			   GFP_ATOMIC);
	if (id < 0)
		fb->id = -1;
	else
		id = fb->id;
	spin_unlock(&fc->lock);
	idr_preload_end();

	WARN_ON_ONCE(id == 0);
	return id;
}

static int fuse_backing_id_remove(struct fuse_conn *fc, int id,
				  struct fuse_backing *old_fb)
{
	struct fuse_backing *fb;
	int ret = 0;

	spin_lock(&fc->lock);
	fb = idr_find(&fc->backing_files_map, id);
	if (fb != old_fb) {
		ret = -EBADF;
		goto out_unlock;
	}

	fb = idr_remove(&fc->backing_files_map, id);
	WARN_ON(fb != old_fb);

out_unlock:
	spin_unlock(&fc->lock);
	return ret;
}

static int fuse_backing_id_free(int id, void *p, void *data)
{
	struct fuse_backing *fb = p;

	WARN_ON_ONCE(refcount_read(&fb->count) != 1);

	trace_fuse_backing_close((struct fuse_conn *)data, fb);

	fuse_backing_free(fb);
	return 0;
}

void fuse_backing_files_free(struct fuse_conn *fc)
{
	idr_for_each(&fc->backing_files_map, fuse_backing_id_free, fc);
	idr_destroy(&fc->backing_files_map);
}

static inline const struct fuse_backing_ops *
fuse_backing_ops_from_map(const struct fuse_backing_map *map)
{
	switch (map->flags & FUSE_BACKING_TYPE_MASK) {
#ifdef CONFIG_FUSE_PASSTHROUGH
	case FUSE_BACKING_TYPE_PASSTHROUGH:
		return &fuse_passthrough_backing_ops;
#endif
	default:
		break;
	}

	return NULL;
}

int fuse_backing_open(struct fuse_conn *fc, struct fuse_backing_map *map)
{
	struct file *file;
	struct fuse_backing *fb = NULL;
	const struct fuse_backing_ops *ops = fuse_backing_ops_from_map(map);
	uint32_t op_flags = map->flags & ~FUSE_BACKING_TYPE_MASK;
	int res;

	pr_debug("%s: fd=%d flags=0x%x\n", __func__, map->fd, map->flags);

	res = -EOPNOTSUPP;
	if (!ops)
		goto out;
	WARN_ON(ops->type != (map->flags & FUSE_BACKING_TYPE_MASK));

	res = ops->may_admin ? ops->may_admin(fc, op_flags) : 0;
	if (res)
		goto out;

	res = -EINVAL;
	if (map->padding)
		goto out;

	file = fget_raw(map->fd);
	res = -EBADF;
	if (!file)
		goto out;

	res = ops->may_open ? ops->may_open(fc, file) : 0;
	if (res)
		goto out_fput;

	fb = kmalloc_obj(struct fuse_backing);
	res = -ENOMEM;
	if (!fb)
		goto out_fput;

	fb->file = file;
	fb->cred = prepare_creds();
	fb->ops = ops;
	fb->id = -1;
	refcount_set(&fb->count, 1);

	res = fuse_backing_id_alloc(fc, fb);
	if (res < 0) {
		fuse_backing_free(fb);
		fb = NULL;
		goto out;
	}

	trace_fuse_backing_open(fc, fb);
out:
	pr_debug("%s: fb=0x%p, ret=%i\n", __func__, fb, res);

	return res;

out_fput:
	fput(file);
	goto out;
}

static struct fuse_backing *__fuse_backing_lookup(struct fuse_conn *fc,
						  int backing_id)
{
	struct fuse_backing *fb;

	rcu_read_lock();
	fb = idr_find(&fc->backing_files_map, backing_id);
	fb = fuse_backing_get(fb);
	rcu_read_unlock();

	return fb;
}

int fuse_backing_close(struct fuse_conn *fc, int backing_id)
{
	struct fuse_backing *fb = NULL;
	const struct fuse_backing_ops *ops;
	int err;

	pr_debug("%s: backing_id=%d\n", __func__, backing_id);

	err = -EINVAL;
	if (backing_id <= 0)
		goto out;

	err = -ENOENT;
	fb = __fuse_backing_lookup(fc, backing_id);
	if (!fb)
		goto out;
	ops = fb->ops;

	err = ops->may_admin ? ops->may_admin(fc, 0) : 0;
	if (err)
		goto out_fb;

	err = ops->may_close ? ops->may_close(fc, fb) : 0;
	if (err)
		goto out_fb;

	err = fuse_backing_id_remove(fc, backing_id, fb);
	if (err)
		goto out_fb;

	trace_fuse_backing_close(fc, fb);

	/* drop the backing id cache's refcount */
	fuse_backing_put(fb);
out_fb:
	/* drop the refcount we got earlier */
	fuse_backing_put(fb);
out:
	pr_debug("%s: fb=0x%p, err=%i\n", __func__, fb, err);

	return err;
}

struct fuse_backing *fuse_backing_lookup(struct fuse_conn *fc,
					 const struct fuse_backing_ops *ops,
					 int backing_id)
{
	struct fuse_backing *fb;

	rcu_read_lock();
	fb = idr_find(&fc->backing_files_map, backing_id);
	if (fb && fb->ops != ops)
		fb = NULL;
	fb = fuse_backing_get(fb);
	rcu_read_unlock();

	return fb;
}
