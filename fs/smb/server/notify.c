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

#include <linux/fsnotify_backend.h>
#include <linux/dcache.h>
#include "glob.h"
#include "../common/smb2status.h"
#include "connection.h"
#include "ksmbd_work.h"
#include "notify.h"
#include "smb_common.h"
#include "smb2pdu.h"
#include "vfs_cache.h"
#include "mgmt/user_session.h"

static const struct {
	u32 notify_mask;
	u32 fsnotify_mask;
} ksmbd_notify_mapping[] = {
	{ FILE_NOTIFY_CHANGE_FILE_NAME,
	  KSMBD_NOTIFY_NAME_EVENT_MASK },
	{ FILE_NOTIFY_CHANGE_DIR_NAME,
	  KSMBD_NOTIFY_NAME_EVENT_MASK },
	{ FILE_NOTIFY_CHANGE_ATTRIBUTES,
	  FS_ATTRIB | FS_MOVED_FROM | FS_MOVED_TO | FS_MODIFY },
	{ FILE_NOTIFY_CHANGE_SIZE, FS_MODIFY },
	{ FILE_NOTIFY_CHANGE_LAST_WRITE, FS_ATTRIB | FS_MODIFY },
	{ FILE_NOTIFY_CHANGE_LAST_ACCESS, FS_ATTRIB | FS_ACCESS },
	{ FILE_NOTIFY_CHANGE_EA, FS_ATTRIB },
	{ FILE_NOTIFY_CHANGE_SECURITY, FS_ATTRIB },
};

static u32 ksmbd_notify_map(u32 filter)
{
	size_t i;
	u32 mask = 0;

	for (i = 0; i < ARRAY_SIZE(ksmbd_notify_mapping); i++) {
		if (ksmbd_notify_mapping[i].notify_mask & filter)
			mask |= ksmbd_notify_mapping[i].fsnotify_mask;
	}

	ksmbd_debug(NOTIFY,
		    "Mapped completion filter 0x%x to fsnotify mask 0x%x\n",
		    filter, mask);

	return mask;
}

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

	ksmbd_debug(NOTIFY, "Wake pending notify request\n");
	wake_up(&notify_req->wait);
}

static int ksmbd_notify_handle_inode_event(struct ksmbd_notify *notify,
					   u32 mask, struct inode *inode,
					   struct inode *dir,
					   const struct qstr *file_name,
					   u32 cookie)
{
	struct inode *event_inode = dir ?: inode;
	struct ksmbd_file *fp;

	fp = notify->fp;

	ksmbd_debug(NOTIFY,
		    "fid %llu:%llu, notify event: mask=0x%08x inode=%llu name=%.*s cookie=%u\n",
		    fp->persistent_id, fp->volatile_id, mask,
		    event_inode ? (unsigned long long)event_inode->i_ino : 0,
		    file_name ? file_name->len : 0,
		    file_name ? (const char *)file_name->name : "", cookie);

	return 0;
}

static char *ksmbd_notify_tree_name(struct ksmbd_notify *notify,
				    const void *data, int data_type,
				    struct inode *dir,
				    const struct qstr *file_name,
				    struct qstr *tree_name)
{
	struct dentry *root = file_dentry(notify->fp->filp);
	struct inode *inode = fsnotify_data_inode(data, data_type);
	struct dentry *dentry;
	char *buf = NULL, *root_buf = NULL, *name = NULL;
	char *path, *root_path, *relative;
	size_t file_name_len = file_name ? file_name->len : 0;
	size_t prefix_len, root_len, len, i;

	/* Name events refer to @file_name below @dir. */
	if (file_name && dir) {
		dentry = d_find_alias(dir);
	} else {
		dentry = fsnotify_data_dentry(data, data_type);
		if (dentry)
			dget(dentry);
		else
			dentry = inode ? d_find_alias(inode) : NULL;
	}
	if (!dentry)
		return ERR_PTR(-ENOENT);

	/* A superblock mark also reports events outside the watched tree. */
	if (!is_subdir(dentry, root)) {
		name = ERR_PTR(-EXDEV);
		goto out_dput;
	}

	buf = kmalloc(PATH_MAX, KSMBD_DEFAULT_GFP);
	root_buf = kmalloc(PATH_MAX, KSMBD_DEFAULT_GFP);
	if (!buf || !root_buf) {
		name = ERR_PTR(-ENOMEM);
		goto out_free_bufs;
	}

	path = dentry_path_raw(dentry, buf, PATH_MAX);
	root_path = dentry_path_raw(root, root_buf, PATH_MAX);
	if (IS_ERR(path) || IS_ERR(root_path)) {
		name = ERR_PTR(IS_ERR(path) ? PTR_ERR(path) : PTR_ERR(root_path));
		goto out_free_bufs;
	}

	root_len = strlen(root_path);
	if (root_len == 1 && root_path[0] == '/') {
		relative = path + 1;
	} else if (!strncmp(path, root_path, root_len) &&
		   (path[root_len] == '/' || path[root_len] == '\0')) {
		relative = path + root_len;
		if (*relative == '/')
			relative++;
	} else {
		/* The watched directory was renamed between the two snapshots. */
		name = ERR_PTR(-EAGAIN);
		goto out_free_bufs;
	}

	prefix_len = strlen(relative);
	if (prefix_len > SIZE_MAX - file_name_len - 2) {
		name = ERR_PTR(-EOVERFLOW);
		goto out_free_bufs;
	}
	len = prefix_len + file_name_len + (prefix_len && file_name ? 1 : 0);
	name = kmalloc(len + 1, KSMBD_DEFAULT_GFP);
	if (!name) {
		name = ERR_PTR(-ENOMEM);
		goto out_free_bufs;
	}

	memcpy(name, relative, prefix_len);
	if (prefix_len && file_name)
		name[prefix_len++] = '\\';
	if (file_name_len)
		memcpy(name + prefix_len, file_name->name, file_name_len);
	name[len] = '\0';
	for (i = 0; i < len; i++) {
		if (name[i] == '/')
			name[i] = '\\';
	}
	tree_name->name = name;
	tree_name->len = len;

out_free_bufs:
	kfree(root_buf);
	kfree(buf);
out_dput:
	dput(dentry);
	return name;
}

static int ksmbd_notify_handle_event(struct fsnotify_group *group, u32 mask,
				     const void *data, int data_type,
				     struct inode *dir,
				     const struct qstr *file_name, u32 cookie,
				     struct fsnotify_iter_info *iter_info)
{
	struct ksmbd_notify *notify = group->private;
	struct qstr tree_name = {};
	struct inode *inode;
	char *name = NULL;
	int err;

	if (!READ_ONCE(notify->watch_tree)) {
		/* Non-tree watches accept events from the root inode mark only. */
		if (!fsnotify_iter_inode_mark(iter_info) &&
		    !fsnotify_iter_parent_mark(iter_info))
			return 0;
	} else {
		name = ksmbd_notify_tree_name(notify, data, data_type, dir,
					      file_name, &tree_name);
		if (IS_ERR(name)) {
			if (PTR_ERR(name) == -ENOMEM)
				pr_err("Failed to allocate tree notify event name\n");
			return 0;
		}
		file_name = &tree_name;
	}

	inode = fsnotify_data_inode(data, data_type);
	err = ksmbd_notify_handle_inode_event(notify, mask, inode, dir,
					      file_name, cookie);
	kfree(name);
	return err;
}

static void ksmbd_notify_free_mark(struct fsnotify_mark *mark)
{
	kfree(mark);
}

static const struct fsnotify_ops ksmbd_notify_fsnotify_ops = {
	.handle_event = ksmbd_notify_handle_event,
	.free_mark = ksmbd_notify_free_mark,
};

static struct fsnotify_mark *
ksmbd_notify_add_mark(struct ksmbd_notify *notify, u32 mask, void *obj,
		      unsigned int obj_type)
{
	struct fsnotify_mark *mark;
	int err;

	mark = kzalloc_obj(*mark, KSMBD_DEFAULT_GFP);
	if (!mark) {
		pr_err("Failed to allocate fsnotify mark\n");
		return ERR_PTR(-ENOMEM);
	}

	fsnotify_init_mark(mark, notify->group);
	mark->mask = mask | FS_EVENT_ON_CHILD;
	err = fsnotify_add_mark(mark, obj, obj_type, 0);
	if (err) {
		pr_err("Failed to add fsnotify mark, type %u: %d\n",
		       obj_type, err);
		goto err_put_mark;
	}

	return mark;

err_put_mark:
	fsnotify_put_mark(mark);
	return ERR_PTR(err);
}

static void ksmbd_notify_destroy_marks(struct ksmbd_notify *notify)
{
	if (notify->tree_mark) {
		fsnotify_destroy_mark(notify->tree_mark, notify->group);
		fsnotify_put_mark(notify->tree_mark);
	}
	if (notify->mark) {
		fsnotify_destroy_mark(notify->mark, notify->group);
		fsnotify_put_mark(notify->mark);
	}
	fsnotify_wait_marks_destroyed();
	fsnotify_put_group(notify->group);
}

static int ksmbd_notify_add(struct ksmbd_file *fp, u32 mask, u32 filter,
			    bool watch_tree,
			    struct ksmbd_notify **notify_out)
{
	struct ksmbd_notify *notify;
	struct fsnotify_mark *mark, *tree_mark;
	int err = 0;

	notify = kzalloc_obj(*notify, KSMBD_DEFAULT_GFP);
	if (!notify) {
		pr_err("Failed to allocate notify watch\n");
		err = -ENOMEM;
		goto out;
	}

	notify->fp = fp;
	spin_lock_init(&notify->lock);
	notify->filter = filter;
	notify->mask = mask;
	notify->watch_tree = watch_tree;

	notify->group = fsnotify_alloc_group(&ksmbd_notify_fsnotify_ops, 0);
	if (IS_ERR(notify->group)) {
		err = PTR_ERR(notify->group);
		pr_err("Failed to allocate fsnotify group: %d\n", err);
		kfree(notify);
		goto out;
	}
	notify->group->private = notify;

	mark = ksmbd_notify_add_mark(notify, mask, file_inode(fp->filp),
				     FSNOTIFY_OBJ_TYPE_INODE);
	if (IS_ERR(mark)) {
		err = PTR_ERR(mark);
		goto err_destroy_marks;
	}
	notify->mark = mark;

	if (watch_tree) {
		/*
		 * FS_EVENT_ON_CHILD covers only one level. A filesystem mark
		 * supplies recursive events; the callback limits them to the
		 * directory rooted at fp.
		 */
		tree_mark = ksmbd_notify_add_mark(notify, mask,
						  file_inode(fp->filp)->i_sb,
						  FSNOTIFY_OBJ_TYPE_SB);
		if (IS_ERR(tree_mark)) {
			err = PTR_ERR(tree_mark);
			goto err_destroy_marks;
		}
		notify->tree_mark = tree_mark;
	}

	*notify_out = notify;
	ksmbd_debug(NOTIFY,
		    "Added fsnotify mark, inode %llu, mask 0x%x, filter 0x%x, watch tree %d\n",
		    (unsigned long long)file_inode(fp->filp)->i_ino, mark->mask,
		    filter, watch_tree);
	goto out;

err_destroy_marks:
	ksmbd_notify_destroy_marks(notify);
	kfree(notify);

out:
	return err;
}

static struct ksmbd_file *
ksmbd_notify_validate_req(struct ksmbd_work *work,
			  struct smb2_change_notify_req *req,
			  struct smb2_change_notify_rsp *rsp)
{
	struct ksmbd_file *fp;
	int err;

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

	ksmbd_debug(NOTIFY,
		    "fid %llu:%llu, handle notify request, filter 0x%x, flags 0x%x\n",
		    fp->persistent_id, fp->volatile_id,
		    le32_to_cpu(req->CompletionFilter), le16_to_cpu(req->Flags));

	if (le32_to_cpu(req->OutputBufferLength) >
	    work->conn->vals->max_trans_size) {
		pr_err("Notify output buffer length %u exceeds maximum %u\n",
		       le32_to_cpu(req->OutputBufferLength),
		       work->conn->vals->max_trans_size);
		rsp->hdr.Status = STATUS_INVALID_PARAMETER;
		err = -EINVAL;
		goto err_put_fp;
	}

	if (le16_to_cpu(req->Flags) & ~SMB2_WATCH_TREE) {
		pr_err("Invalid notify flags 0x%x\n", le16_to_cpu(req->Flags));
		rsp->hdr.Status = STATUS_INVALID_PARAMETER;
		err = -EINVAL;
		goto err_put_fp;
	}

	if (!S_ISDIR(file_inode(fp->filp)->i_mode)) {
		pr_err("Notify file id is not a directory, fid %llu:%llu\n",
		       fp->persistent_id, fp->volatile_id);
		rsp->hdr.Status = STATUS_INVALID_PARAMETER;
		err = -EINVAL;
		goto err_put_fp;
	}

	if (!(fp->daccess & FILE_LIST_DIRECTORY_LE)) {
		pr_err("No permission to monitor directory, fid %llu:%llu\n",
		       fp->persistent_id, fp->volatile_id);
		rsp->hdr.Status = STATUS_ACCESS_DENIED;
		err = -EACCES;
		goto err_put_fp;
	}

	return fp;

err_put_fp:
	ksmbd_fd_put(work, fp);
	return ERR_PTR(err);
}

static struct ksmbd_notify *
ksmbd_notify_setup_watch(struct ksmbd_file *fp,
			 struct smb2_change_notify_req *req,
			 struct smb2_change_notify_rsp *rsp)
{
	struct ksmbd_notify *notify;
	u32 filter, mask;
	int err;

	filter = le32_to_cpu(req->CompletionFilter);
	mask = ksmbd_notify_map(filter);
	if (!mask)
		ksmbd_debug(NOTIFY,
			    "No mapped completion filter bits; request will remain pending\n");

	err = ksmbd_notify_add(fp, mask, filter,
			       le16_to_cpu(req->Flags) & SMB2_WATCH_TREE,
			       &notify);
	if (err) {
		pr_err("Failed to add notify watch, fid %llu:%llu: %d\n",
		       fp->persistent_id, fp->volatile_id, err);
		rsp->hdr.Status = STATUS_INSUFFICIENT_RESOURCES;
		return ERR_PTR(err);
	}

	return notify;
}

static int ksmbd_notify_wait(struct ksmbd_work *work,
			     struct ksmbd_notify *notify,
			     struct ksmbd_notify_req *notify_req)
{
	struct ksmbd_file *fp = notify->fp;
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

	ksmbd_debug(NOTIFY, "Notify request pending, async id %d\n",
		    work->async_id);
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
	struct ksmbd_notify *notify = NULL;
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

	notify = ksmbd_notify_setup_watch(fp, req, rsp);
	if (IS_ERR(notify)) {
		err = PTR_ERR(notify);
		notify = NULL;
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

	err = ksmbd_notify_wait(work, notify, &notify_req);
	if (err == -ENOENT) {
		rsp->hdr.Status = STATUS_NOTIFY_CLEANUP;
		goto out;
	}

	if (work->state == KSMBD_WORK_CLOSED) {
		rsp->hdr.Status = STATUS_NOTIFY_CLEANUP;
		ksmbd_debug(NOTIFY, "Notify handle closed, async id %d\n",
			    work->async_id);
	} else {
		rsp->hdr.Status = STATUS_CANCELLED;
		ksmbd_debug(NOTIFY, "Notify request cancelled, async id %d\n",
			    work->async_id);
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
	if (notify) {
		ksmbd_notify_destroy_marks(notify);
		kfree(notify);
	}
	if (fp)
		ksmbd_fd_put(work, fp);
	return err;
}
