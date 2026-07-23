// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *   SMB2 CHANGE_NOTIFY
 *
 *   Copyright (C) 2026 KylinSoft Co., Ltd. All rights reserved.
 *   Author(s): ChenXiaoSong <chenxiaosong@kylinos.cn>
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
	struct ksmbd_file *fp = NULL;
	int err = 0;

	fp = ksmbd_notify_validate_req(work, req, rsp);
	if (IS_ERR(fp)) {
		err = PTR_ERR(fp);
		goto out;
	}

	ksmbd_fd_put(work, fp);
	rsp->hdr.Status = STATUS_NOT_IMPLEMENTED;
	err = -EOPNOTSUPP;

out:
	if (err)
		pr_err("Failed to handle notify request: %d, status: 0x%x\n",
		       err, le32_to_cpu(rsp->hdr.Status));
	if (rsp->hdr.Status != STATUS_SUCCESS)
		smb2_set_err_rsp(work);
	return err;
}
