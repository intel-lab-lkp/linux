// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *   SMB2 CHANGE_NOTIFY
 *
 *   Copyright (C) 2026 KylinSoft Co., Ltd. All rights reserved.
 *   Author(s): ChenXiaoSong <chenxiaosong@kylinos.cn>
 *
 */

#include <linux/slab.h>
#include <linux/wait.h>

#include "glob.h"
#include "../common/smb2status.h"
#include "connection.h"
#include "ksmbd_work.h"
#include "notify.h"
#include "smb_common.h"
#include "smb2pdu.h"
#include "vfs_cache.h"

struct ksmbd_notify_req {
	wait_queue_head_t wait;
};

static void smb2_notify_cancel(void **argv)
{
	struct ksmbd_notify_req *notify_req = argv[0];

	ksmbd_debug(NOTIFY, "Wake pending notify request\n");
	wake_up(&notify_req->wait);
}

static struct ksmbd_file *
ksmbd_notify_validate_req(struct ksmbd_work *work,
			  struct smb2_change_notify_req *req,
			  struct smb2_change_notify_rsp *rsp)
{
	struct ksmbd_file *fp;
	int err;

	fp = ksmbd_lookup_fd_slow(work, req->VolatileFileId,
				  req->PersistentFileId);
	if (!fp) {
		pr_err("Invalid file id for notify, fid %llu:%llu\n",
		       le64_to_cpu(req->PersistentFileId),
		       le64_to_cpu(req->VolatileFileId));
		rsp->hdr.Status = STATUS_FILE_CLOSED;
		return ERR_PTR(-ENOENT);
	}

	ksmbd_debug(NOTIFY,
		    "fid %llu:%llu, handle notify request, filter 0x%x, flags 0x%x\n",
		    fp->persistent_id, fp->volatile_id,
		    le32_to_cpu(req->CompletionFilter), le16_to_cpu(req->Flags));

	if (!S_ISDIR(file_inode(fp->filp)->i_mode)) {
		pr_err("Notify file id is not a directory, fid %llu:%llu\n",
		       fp->persistent_id, fp->volatile_id);
		rsp->hdr.Status = STATUS_NOT_A_DIRECTORY;
		err = -ENOTDIR;
		goto err_put_fp;
	}

	if (!(fp->daccess & FILE_LIST_DIRECTORY_LE)) {
		pr_err("No permission to monitor directory, fid %llu:%llu\n",
		       fp->persistent_id, fp->volatile_id);
		rsp->hdr.Status = STATUS_ACCESS_DENIED;
		err = -EACCES;
		goto err_put_fp;
	}

	if (le32_to_cpu(req->OutputBufferLength) >
	    work->conn->vals->max_trans_size) {
		pr_err("Notify output buffer length %u exceeds maximum %u\n",
		       le32_to_cpu(req->OutputBufferLength),
		       work->conn->vals->max_trans_size);
		rsp->hdr.Status = STATUS_INVALID_PARAMETER;
		err = -EINVAL;
		goto err_put_fp;
	}

	return fp;

err_put_fp:
	ksmbd_fd_put(work, fp);
	return ERR_PTR(err);
}

static int ksmbd_notify_wait(struct ksmbd_work *work,
			     struct ksmbd_file *fp,
			     struct ksmbd_notify_req *notify_req)
{
	int err;

	spin_lock(&fp->f_lock);
	list_add_tail(&work->fp_entry, &fp->blocked_works);
	spin_unlock(&fp->f_lock);

	ksmbd_debug(NOTIFY, "Notify request pending, async id %d\n",
		    work->async_id);
	smb2_send_interim_resp(work, STATUS_PENDING);

	err = wait_event_interruptible(notify_req->wait,
				       READ_ONCE(work->state) !=
					       KSMBD_WORK_ACTIVE);
	if (err && READ_ONCE(work->state) == KSMBD_WORK_ACTIVE) {
		pr_err("Notify wait interrupted, async id %d: %d\n",
		       work->async_id, err);
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
	struct ksmbd_notify_req *notify_req = NULL;
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

	notify_req = kzalloc_obj(*notify_req, KSMBD_DEFAULT_GFP);
	if (!notify_req) {
		pr_err("Failed to allocate notify request\n");
		rsp->hdr.Status = STATUS_INSUFFICIENT_RESOURCES;
		err = -ENOMEM;
		goto out;
	}

	argv = kmalloc_obj(*argv, KSMBD_DEFAULT_GFP);
	if (!argv) {
		pr_err("Failed to allocate notify cancel arguments\n");
		rsp->hdr.Status = STATUS_INSUFFICIENT_RESOURCES;
		err = -ENOMEM;
		goto out;
	}

	init_waitqueue_head(&notify_req->wait);
	argv[0] = notify_req;
	err = setup_async_work(work, smb2_notify_cancel, argv);
	if (err) {
		pr_err("Failed to set up asynchronous notify work: %d\n", err);
		rsp->hdr.Status = STATUS_INSUFFICIENT_RESOURCES;
		goto out;
	}
	async_work = true;

	err = ksmbd_notify_wait(work, fp, notify_req);

	if (work->state == KSMBD_WORK_CANCELLED) {
		ksmbd_debug(NOTIFY, "Notify request cancelled, async id %d\n",
			    work->async_id);
		rsp->hdr.Status = STATUS_CANCELLED;
		smb2_send_interim_resp(work, STATUS_CANCELLED);
		work->send_no_response = 1;
	} else if (work->state == KSMBD_WORK_CLOSED) {
		ksmbd_debug(NOTIFY, "Notify handle closed, async id %d\n",
			    work->async_id);
		rsp->hdr.Status = STATUS_NOTIFY_CLEANUP;
		smb2_send_interim_resp(work, STATUS_NOTIFY_CLEANUP);
		work->send_no_response = 1;
	}

out:
	if (err)
		pr_err("Failed to handle notify request: %d, status: 0x%x\n",
		       err, le32_to_cpu(rsp->hdr.Status));
	if (rsp->hdr.Status != STATUS_SUCCESS && !work->send_no_response)
		smb2_set_err_rsp(work);
	if (async_work)
		release_async_work(work);
	else
		kfree(argv);
	if (notify_req)
		kfree(notify_req);
	if (fp)
		ksmbd_fd_put(work, fp);
	return err;
}
