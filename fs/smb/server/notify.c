// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *   SMB2 CHANGE_NOTIFY
 *
 *   Copyright (C) 2026 KylinSoft Co., Ltd. All rights reserved.
 *
 *   Author(s): ChenXiaoSong <chenxiaosong@kylinos.cn>
 *              Gael Blivet <gael.blivet@gmail.com>
 *
 */

#include "glob.h"
#include "../common/smb2status.h"
#include "connection.h"
#include "ksmbd_work.h"
#include "notify.h"
#include "smb_common.h"
#include "smb2pdu.h"
#include "vfs_cache.h"
#include "mgmt/user_session.h"

/*
 * Cancel handler for a pending CHANGE_NOTIFY. Called either by
 * smb2_cancel() (conn->request_lock held, work->state already set to
 * KSMBD_WORK_CANCELLED by the caller) or by
 * set_close_state_blocked_works() (vfs_cache.c, fp->f_lock held,
 * work->state already set to KSMBD_WORK_CLOSED by the caller) -- both
 * callers hold a spinlock across this call, so it must not sleep.
 * wake_up() only wakes the waiter in smb2_notify(); it does not touch
 * fp->blocked_works itself, matching smb2_remove_blocked_lock()'s same
 * non-mutating style for the equivalent byte-range-lock wait.
 */
static void smb2_notify_cancel(void **argv)
{
	struct ksmbd_notify_req *notify_req = argv[0];

	wake_up(&notify_req->wait);
}

static struct ksmbd_file *
ksmbd_notify_validate_req(struct ksmbd_work *work,
			  struct smb2_change_notify_req *req,
			  struct smb2_change_notify_rsp *rsp)
{
	struct ksmbd_file *fp;

	if (work->next_smb2_rcv_hdr_off && req->hdr.NextCommand) {
		pr_err("Notify request is not the last compound command\n");
		rsp->hdr.Status = STATUS_INTERNAL_ERROR;
		return ERR_PTR(-EIO);
	}

	fp = ksmbd_lookup_fd_slow(work, req->VolatileFileId,
				  req->PersistentFileId);
	if (!fp) {
		pr_err("Invalid file id for notify, fid %llu:%llu\n",
		       le64_to_cpu(req->PersistentFileId),
		       le64_to_cpu(req->VolatileFileId));
		rsp->hdr.Status = STATUS_FILE_CLOSED;
		return ERR_PTR(-ENOENT);
	}

	return fp;
}

static int ksmbd_notify_wait(struct ksmbd_work *work,
			     struct ksmbd_file *fp,
			     struct ksmbd_notify_req *notify_req)
{
	int err;

	/*
	 * Handle close holds the file-table write lock while it marks the
	 * handle closed and walks blocked_works.  Hold the matching read lock
	 * across the state check and registration so close cannot finish its
	 * walk between the lookup above and this list insertion.
	 */
	read_lock(&work->sess->file_table.lock);
	if (fp->f_state != FP_INITED) {
		read_unlock(&work->sess->file_table.lock);
		return -ENOENT;
	}
	spin_lock(&fp->f_lock);
	list_add_tail(&work->fp_entry, &fp->blocked_works);
	spin_unlock(&fp->f_lock);
	read_unlock(&work->sess->file_table.lock);

	smb2_send_interim_resp(work, STATUS_PENDING);

	err = wait_event_interruptible(notify_req->wait,
				       READ_ONCE(work->state) !=
					       KSMBD_WORK_ACTIVE);
	if (err && READ_ONCE(work->state) == KSMBD_WORK_ACTIVE) {
		pr_err("Notify wait interrupted, async id %d: %d\n",
		       work->async_id, err);
		/*
		 * Woken by a signal, not a real cancel/close. There is no
		 * notification backend yet to report anything else against,
		 * so treat this the same as a client-side cancel.
		 */
		WRITE_ONCE(work->state, KSMBD_WORK_CANCELLED);
	}

	spin_lock(&fp->f_lock);
	list_del_init(&work->fp_entry);
	spin_unlock(&fp->f_lock);

	return err;
}

/**
 * ksmbd_handle_notify() - handle an SMB2 change notify request
 * @work: smb work containing notify command buffer
 * @req: SMB2 change notify request
 * @rsp: SMB2 change notify response
 *
 * Return: 0 on success, otherwise error
 */
int ksmbd_handle_notify(struct ksmbd_work *work,
			struct smb2_change_notify_req *req,
			struct smb2_change_notify_rsp *rsp)
{
	struct ksmbd_notify_req notify_req = {};
	struct ksmbd_file *fp = NULL;
	void **argv = NULL;
	bool async_work = false;
	int err = 0;

	fp = ksmbd_notify_validate_req(work, req, rsp);
	if (IS_ERR(fp)) {
		err = PTR_ERR(fp);
		fp = NULL;
		goto out;
	}

	argv = kmalloc_obj(*argv, KSMBD_DEFAULT_GFP);
	if (!argv) {
		pr_err("Failed to allocate notify cancel arguments\n");
		rsp->hdr.Status = STATUS_INSUFFICIENT_RESOURCES;
		err = -ENOMEM;
		goto out;
	}
	init_waitqueue_head(&notify_req.wait);
	argv[0] = &notify_req;

	err = setup_async_work(work, smb2_notify_cancel, argv);
	if (err) {
		pr_err("Failed to set up asynchronous notify work: %d\n", err);
		rsp->hdr.Status = STATUS_INSUFFICIENT_RESOURCES;
		goto out;
	}
	async_work = true;

	err = ksmbd_notify_wait(work, fp, &notify_req);
	if (err == -ENOENT) {
		rsp->hdr.Status = STATUS_NOTIFY_CLEANUP;
		goto out;
	}

	if (work->state == KSMBD_WORK_CLOSED) {
		rsp->hdr.Status = STATUS_NOTIFY_CLEANUP;
	} else {
		rsp->hdr.Status = STATUS_CANCELLED;
	}
	smb2_send_interim_resp(work, rsp->hdr.Status);
	work->send_no_response = 1;

out:
	if (rsp->hdr.Status != STATUS_SUCCESS && !work->send_no_response)
		smb2_set_err_rsp(work);
	if (async_work)
		release_async_work(work);
	else
		kfree(argv);
	if (fp)
		ksmbd_fd_put(work, fp);
	return err;
}
