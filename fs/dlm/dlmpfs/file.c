// SPDX-License-Identifier: GPL-2.0-only
/* dlmpfs file implementation mostly flock/fcntl
 */

#include <linux/fs.h>
#include <linux/dlm.h>
#include <linux/slab.h>
#include <linux/dcache.h>
#include <linux/filelock.h>

#include "internal.h"

struct dlmpfs_file {
	struct dlmlk *flk;
};

static int dlmpfs_open(struct inode *inode, struct file *file)
{
	struct dlmpfs_fs_info *fsi = DLMPFS_FSI(file->f_inode);
	char strname[DLMPFS_NUM_RESNAME_LEN];
	struct dlmpfs_file *fp;

	fp = kzalloc_obj(*fp, GFP_NOFS);
	if (!fp)
		return -ENOMEM;

	dlmpfs_fill_resname_num("flock", strname, file->f_inode->i_ino);
	fp->flk = dlmlk_alloc(fsi->ls, strname, strlen(strname));
	file->private_data = fp;
	return simple_open(inode, file);
}

static int dlmpfs_do_flock(struct file *file, int cmd, struct file_lock *fl)
{
	struct dlmpfs_file *fp = file->private_data;
	unsigned long flags = 0;
	int mode;
	int rv;

	mode = lock_is_write(fl) ? DLM_LOCK_EX : DLM_LOCK_PR;
	/* user should not do that but we catch this case here */
	if (dlmlk_grmode(fp->flk) == mode)
		return 0;

	if (!IS_SETLKW(cmd))
		flags |= DLM_LKF_NOQUEUE;

	/* avoid SH->EX, do SH->NL->EX to avoid deadlocks */
	if (IS_SETLKW(cmd) &&
	    dlmlk_grmode(fp->flk) == DLM_LOCK_PR &&
	    mode == DLM_LOCK_EX)
		dlmlk_convert(fp->flk, DLM_LOCK_NL, flags);

	rv = dlmlk_convert_interuptible(fp->flk, mode, flags);
	if (rv == -EINTR)
		return rv;

	if (dlmlk_sb(fp->flk)->sb_status == -EAGAIN) {
		rv = -EAGAIN;
	} else {
		rv = locks_lock_file_wait(file, fl);
		/* should never be the case */
		WARN_ON(rv == -EINTR);
	}

	return rv;
}

static void dlmpfs_do_unflock(struct file *file, struct file_lock *fl)
{
	struct dlmpfs_file *fp = file->private_data;

	if (dlmlk_grmode(fp->flk) == DLM_LOCK_NL)
		return;

	dlmlk_convert(fp->flk, DLM_LOCK_NL, 0);
	locks_lock_file_wait(file, fl);
}

static int dlmpfs_flock(struct file *file, int cmd, struct file_lock *fl)
{
	if (!(fl->c.flc_flags & FL_FLOCK))
		return -ENOLCK;

	if (lock_is_unlock(fl)) {
		dlmpfs_do_unflock(file, fl);
		return 0;
	} else {
		return dlmpfs_do_flock(file, cmd, fl);
	}
}

static int dlmpfs_lock(struct file *file, int cmd, struct file_lock *fl)
{
	struct dlmpfs_fs_info *fsi = DLMPFS_FSI(file->f_inode);
	struct inode *i = file->f_inode;

	if (!(fl->c.flc_flags & FL_POSIX))
		return -ENOLCK;

	if (cmd == F_CANCELLK)
		return dlmplk_cancel(fsi->ls, i->i_ino, file, fl);
	else if (IS_GETLK(cmd))
		return dlmplk_get(fsi->ls, i->i_ino, file, fl);
	else if (lock_is_unlock(fl))
		return dlmplk_unlock(fsi->ls, i->i_ino, file, fl);

	return dlmplk_lock(fsi->ls, i->i_ino, file, cmd, fl);
}

static int dlmpfs_release(struct inode *inode, struct file *file)
{
	struct dlmpfs_file *fp = file->private_data;

	if (fp->flk)
		dlmlk_free_nowait(fp->flk);

	kfree(fp);
	file->private_data = NULL;
	return 0;
}

const struct file_operations dlmpfs_file_operations = {
	.open		= dlmpfs_open,
	.flock		= dlmpfs_flock,
	.lock		= dlmpfs_lock,
	.fsync		= noop_fsync,
	.release	= dlmpfs_release,
};

const struct inode_operations dlmpfs_file_inode_operations = {
	.setattr	= simple_setattr,
	.getattr	= simple_getattr,
};
