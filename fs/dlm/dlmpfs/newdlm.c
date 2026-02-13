// SPDX-License-Identifier: GPL-2.0-only
/* creating a better to use DLM API above the current API
 */

#include <linux/dlm.h>
#include <linux/delay.h>
#include <linux/filelock.h>
#include <linux/dlm_plock.h>

#include "internal.h"

struct dlmlk {
	/* need to aligned */
	unsigned char lvb[8];
	struct dlm_lksb sb;
	struct completion compl;
	struct dlmls *ls;
	bool nowait;
	unsigned char name[DLM_RESNAME_MAXLEN];
	size_t namelen;
	int grmode;
};

struct dlmls {
	dlm_lockspace_t *ls;
	struct kref kref;
	bool released;
};

static void release_ls(struct kref *kref)
{
	struct dlmls *ls = container_of(kref, struct dlmls, kref);

	kfree(ls);
}

static void ast(void *astarg)
{
	struct dlmlk *lk = astarg;

	if (lk->nowait) {
		WARN_ON(lk->sb.sb_status != -DLM_EUNLOCK);
		kref_put(&lk->ls->kref, release_ls);
		kfree(lk);
	} else {
		complete(&lk->compl);
	}
}

static int dlm_lock_sync(struct dlmlk *lk,
			 int mode, uint32_t _flags,
			 bool interruptible)
{
	unsigned long flags = _flags;
	int rv;

	/* invalid sb_status because racing API with prev status */
	lk->sb.sb_status = -1;

	/* stupid DLM_LKF_CONVERT setting */
	if (lk->grmode != DLM_LOCK_IV)
		flags |= DLM_LKF_CONVERT;

retry:
	rv = dlm_lock(lk->ls->ls, mode, &lk->sb, flags, lk->name, lk->namelen,
		      0, ast, lk, NULL);
	switch (rv) {
	case 0:
		break;
	case -EBUSY:
		/* stupid DLM API behaviour */
		mdelay(50);
		goto retry;
	default:
		goto out;
	}

	if (interruptible) {
		rv = wait_for_completion_interruptible(&lk->compl);
		if (rv == -ERESTARTSYS) {
			dlm_unlock(lk->ls->ls, lk->sb.sb_lkid,
				   DLM_LKF_CANCEL, NULL, lk);

			wait_for_completion(&lk->compl);
			switch (lk->sb.sb_status) {
			case -DLM_ECANCEL:
				rv = -EINTR;
				break;
			case 0:
				rv = 0;
				break;
			default:
				rv = -1;
				WARN_ON(1);
				break;
			}
		}
	} else {
		/* TODO waiting on demote makes only sense if DLM_LKF_VALBLK */
		wait_for_completion(&lk->compl);
	}

	/* user might can request that because the user space user
	 * makes stupid things like NL -> NL conversions.
	 */
	if (!rv)
		lk->grmode = mode;

out:
	return rv;
}

static int dlm_unlock_sync(struct dlmlk *lk, bool nowait)
{
	int rv;

	/* never did a lock change */
	if (lk->grmode == DLM_LOCK_IV)
		return 0;

	rv = dlm_unlock(lk->ls->ls, lk->sb.sb_lkid, 0,
			NULL, lk);
	if (rv)
		goto out;

	if (!nowait)
		wait_for_completion(&lk->compl);

out:
	return rv;
}

struct dlmlk *dlmlk_alloc(struct dlmls *ls, const void *name, size_t namelen)
{
	struct dlmlk *lk;

	lk = kzalloc_obj(*lk, GFP_NOFS);
	if (!lk)
		return NULL;

	lk->ls = ls;
	kref_get(&ls->kref);
	lk->sb.sb_lvbptr = lk->lvb;
	init_completion(&lk->compl);
	memcpy(lk->name, name, namelen);
	lk->namelen = namelen;
	lk->grmode = DLM_LOCK_IV;

	/* TODO tell DLM to perform master lookup, even before alloc */
	return lk;
}

void dlmlk_convert(struct dlmlk *lk, int mode, unsigned long flags)
{
	int rv;

	rv = dlm_lock_sync(lk, mode, flags, false);
	WARN_ON(rv);
}

int dlmlk_convert_interuptible(struct dlmlk *lk, int mode, unsigned long flags)
{
	/* demotes never run into contention, TODO more states */
	WARN_ON(mode == DLM_LOCK_NL);
	/* can return -EINTR */
	return dlm_lock_sync(lk, mode, flags, true);
}

void dlmlk_free(struct dlmlk *lk)
{
	int rv;

	/* if we already released we don't perform unlocks */
	if (!lk->ls->released) {
		rv = dlm_unlock_sync(lk, false);
		WARN_ON(rv);
	}

	kref_put(&lk->ls->kref, release_ls);
	kfree(lk);
}

void dlmlk_free_nowait(struct dlmlk *lk)
{
	int rv;

	/* if we already released we don't perform unlocks */
	if (!lk->ls->released) {
		rv = dlm_unlock_sync(lk, true);
		WARN_ON(rv);
	} else {
		kref_put(&lk->ls->kref, release_ls);
		kfree(lk);
	}
}

const struct dlm_lksb *dlmlk_sb(struct dlmlk *lk)
{
	return &lk->sb;
}

int dlmlk_grmode(struct dlmlk *lk)
{
	return lk->grmode;
}

int dlmls_new(const char *lsname, const char *clname,
	      uint32_t flags, int lvblen,
	      const struct dlm_lockspace_ops *ops,
	      void *ops_arg, int *ops_result,
	      struct dlmls **ls_ret)
{
	struct dlmls *ls;
	int rv;

	ls = kzalloc_obj(*ls, GFP_NOFS);
	if (!ls)
		return -ENOMEM;

	rv = dlm_new_lockspace(lsname, clname, flags, lvblen,
			       ops, ops_arg, ops_result, &ls->ls);
	if (!rv) {
		kref_init(&ls->kref);
		*ls_ret = ls;
	}

	return rv;
}

void dlmls_release(struct dlmls *ls, uint32_t flags)
{
	ls->released = true;
	dlm_release_lockspace(ls->ls, flags);
}

int dlmplk_lock(struct dlmls *ls, u64 number, struct file *file,
		int cmd, struct file_lock *fl)
{
	return dlm_posix_lock(ls->ls, number, file, cmd, fl);
}

int dlmplk_unlock(struct dlmls *ls, u64 number, struct file *file,
		  struct file_lock *fl)
{
	return dlm_posix_unlock(ls->ls, number, file, fl);
}

int dlmplk_cancel(struct dlmls *ls, u64 number, struct file *file,
		  struct file_lock *fl)
{
	return dlm_posix_cancel(ls->ls, number, file, fl);
}

int dlmplk_get(struct dlmls *ls, u64 number, struct file *file,
	       struct file_lock *fl)
{
	return dlm_posix_get(ls->ls, number, file, fl);
}
