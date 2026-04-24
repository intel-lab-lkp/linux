// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE passthrough to backing file.
 *
 * Copyright (c) 2023 CTERA Networks.
 */

#include "fuse_i.h"

#include <linux/file.h>
#include <linux/fdtable.h>
#include <linux/task_work.h>

struct fuse_backing *fuse_backing_get(struct fuse_backing *fb)
{
	if (fb && refcount_inc_not_zero(&fb->count))
		return fb;
	return NULL;
}

static void fuse_backing_free(struct fuse_backing *fb)
{
	pr_debug("%s: fb=0x%p\n", __func__, fb);

	WARN_ON_ONCE(fb->fd >= 0);
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
	int id;

	idr_preload(GFP_KERNEL);
	spin_lock(&fc->lock);
	/* FIXME: xarray might be space inefficient */
	id = idr_alloc_cyclic(&fc->backing_files_map, fb, 1, 0, GFP_ATOMIC);
	spin_unlock(&fc->lock);
	idr_preload_end();

	WARN_ON_ONCE(id == 0);
	return id;
}

static struct fuse_backing *fuse_backing_id_remove(struct fuse_conn *fc,
						   int id)
{
	struct fuse_backing *fb;

	spin_lock(&fc->lock);
	fb = idr_remove(&fc->backing_files_map, id);
	spin_unlock(&fc->lock);

	return fb;
}

struct fuse_backing_close_work {
	struct callback_head cb;
	int fd;
};

static void fuse_backing_close_fd(struct callback_head *cb)
{
	struct fuse_backing_close_work *w =
		container_of(cb, struct fuse_backing_close_work, cb);
	close_fd(w->fd);
	kfree(w);
}

static int fuse_backing_id_free(int id, void *p, void *data)
{
	struct fuse_conn *fc = data;
	struct fuse_backing *fb = p;

	WARN_ON_ONCE(refcount_read(&fb->count) != 1);

	if (fb->fd >= 0 && fc->daemon_task) {
		struct fuse_backing_close_work *w;

		w = kmalloc_obj(*w, GFP_ATOMIC);
		if (w) {
			init_task_work(&w->cb, fuse_backing_close_fd);
			w->fd = fb->fd;
			/*
			 * Schedule close_fd() to run in the daemon's context.
			 * TWA_RESUME fires on the daemon's next return to
			 * userspace -- in practice immediately after its
			 * blocked read() unblocks with an error at teardown.
			 * -ESRCH means the daemon already exited and closed
			 * all its fds; nothing to do.
			 */
			if (task_work_add(fc->daemon_task, &w->cb, TWA_RESUME))
				kfree(w);
		} else {
			pr_warn_ratelimited("fuse: failed to close backing fd %d on teardown\n",
					    fb->fd);
		}
		fb->fd = -1;
	}

	fuse_backing_free(fb);
	return 0;
}

void fuse_backing_files_free(struct fuse_conn *fc)
{
	idr_for_each(&fc->backing_files_map, fuse_backing_id_free, fc);
	idr_destroy(&fc->backing_files_map);

	if (fc->daemon_task) {
		put_task_struct(fc->daemon_task);
		fc->daemon_task = NULL;
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

	fb = kmalloc_obj(struct fuse_backing);
	res = -ENOMEM;
	if (!fb)
		goto out_fput;

	/*
	 * Capture the daemon's task on the first BACKING_OPEN so that
	 * fuse_backing_files_free() can schedule fd closes via task_work
	 * during teardown, even when teardown runs outside daemon context.
	 */
	if (!fc->daemon_task) {
		spin_lock(&fc->lock);
		if (!fc->daemon_task)
			fc->daemon_task = get_task_struct(current);
		spin_unlock(&fc->lock);
	}

	fb->file = file;	/* fget_raw ref transferred */
	fb->fd = -1;
	fb->cred = prepare_creds();
	if (!fb->cred) {
		res = -ENOMEM;
		goto out_free;
	}
	refcount_set(&fb->count, 1);
	fb->fd = get_unused_fd_flags(O_CLOEXEC);
	if (fb->fd < 0) {
		res = fb->fd;
		goto out_free;
	}
	get_file(file);
	fd_install(fb->fd, file);

	res = fuse_backing_id_alloc(fc, fb);
	if (res < 0)
		goto out_close_fd;

out:
	pr_debug("%s: fb=0x%p, ret=%i\n", __func__, fb, res);

	return res;

out_close_fd:
	close_fd(fb->fd);
	fb->fd = -1;
out_free:
	fuse_backing_free(fb);
	fb = NULL;
	goto out;
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

	/*
	 * Close the fd installed in the daemon's fd table on BACKING_OPEN.
	 * BACKING_CLOSE always runs in the daemon's ioctl context, so
	 * close_fd() targets the correct fd table.
	 */
	if (fb->fd >= 0) {
		close_fd(fb->fd);
		fb->fd = -1;
	}
	fuse_backing_put(fb);
	err = 0;
out:
	pr_debug("%s: fb=0x%p, err=%i\n", __func__, fb, err);

	return err;
}

struct fuse_backing *fuse_backing_lookup(struct fuse_conn *fc, int backing_id)
{
	struct fuse_backing *fb;

	rcu_read_lock();
	fb = idr_find(&fc->backing_files_map, backing_id);
	fb = fuse_backing_get(fb);
	rcu_read_unlock();

	return fb;
}
