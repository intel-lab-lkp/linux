// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *   SMB2 CHANGE_NOTIFY
 *
 *   Copyright (C) 2026 KylinSoft Co., Ltd. All rights reserved.
 *   Author(s): ChenXiaoSong <chenxiaosong@kylinos.cn>
 *
 */

#include <linux/fsnotify_backend.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/list_sort.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/workqueue.h>

#include "glob.h"
#include "../common/smb2status.h"
#include "connection.h"
#include "ksmbd_work.h"
#include "notify.h"
#include "smb_common.h"
#include "smb2pdu.h"
#include "vfs_cache.h"

struct ksmbd_notify_mark {
	struct fsnotify_mark mark;
	struct ksmbd_notify *notify;
};

struct ksmbd_notify_event {
	struct list_head list;
	u32 action;
	u64 when;
	size_t name_len;
	char name[];
};

struct ksmbd_notify {
	struct fsnotify_group *group;
	struct fsnotify_mark *mark;
	struct ksmbd_file *fp;
	/* Protects filter, rename state and the queued events. */
	spinlock_t lock;
	struct list_head events;
	unsigned int num_events;
	u32 filter;
	u32 mask;
	u32 max_buffer_size;
	struct ksmbd_notify_event *moved_from_event;
	u32 moved_from_mask;
	u32 moved_from_cookie;
	struct delayed_work moved_from_work;
};

struct ksmbd_notify_req {
	wait_queue_head_t wait;
};

#define KSMBD_NOTIFY_MOVED_FROM_MSECS	100
#define KSMBD_NOTIFY_NAME_EVENT_MASK	(FS_CREATE | FS_DELETE | \
					 FS_MOVED_FROM | FS_MOVED_TO)
#define KSMBD_NOTIFY_EVENT_MASK		(FS_ATTRIB | FS_MODIFY | \
					 KSMBD_NOTIFY_NAME_EVENT_MASK)

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
	{ FILE_NOTIFY_CHANGE_LAST_WRITE, FS_ATTRIB },
	{ FILE_NOTIFY_CHANGE_LAST_ACCESS, FS_ATTRIB },
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

static void ksmbd_notify_free_events(struct list_head *events)
{
	struct ksmbd_notify_event *event, *tmp;

	list_for_each_entry_safe(event, tmp, events, list) {
		list_del(&event->list);
		kfree(event);
	}
}

static bool ksmbd_notify_filter_match(struct ksmbd_notify *notify, u32 mask)
{
	u32 filter, notify_mask;

	spin_lock(&notify->lock);
	filter = notify->filter;
	notify_mask = notify->mask;
	spin_unlock(&notify->lock);

	if (mask & KSMBD_NOTIFY_NAME_EVENT_MASK) {
		if (mask & FS_ISDIR)
			return filter & FILE_NOTIFY_CHANGE_DIR_NAME;
		return filter & FILE_NOTIFY_CHANGE_FILE_NAME;
	}

	return mask & notify_mask;
}

static void smb2_notify_cancel(void **argv)
{
	struct ksmbd_notify_req *notify_req = argv[0];

	ksmbd_debug(NOTIFY, "Wake pending notify request\n");
	wake_up(&notify_req->wait);
}

static struct ksmbd_notify_event *
ksmbd_notify_alloc_event(u32 action, const struct qstr *file_name, gfp_t gfp)
{
	struct ksmbd_notify_event *event;
	size_t name_len = file_name ? file_name->len : 0;

	event = kmalloc(sizeof(*event) + name_len + 1, gfp);
	if (!event) {
		pr_err("Failed to allocate notify event, action %u, name %.*s\n",
		       action, file_name ? file_name->len : 0,
		       file_name ? (const char *)file_name->name : "");
		return NULL;
	}

	INIT_LIST_HEAD(&event->list);
	event->action = action;
	event->when = ktime_get_ns();
	event->name_len = name_len;
	if (name_len)
		memcpy(event->name, file_name->name, name_len);
	event->name[name_len] = '\0';

	return event;
}

static void
ksmbd_notify_queue_event(struct ksmbd_notify *notify,
			 struct ksmbd_notify_event *event)
{
	ksmbd_debug(NOTIFY, "Queueing notify event, action %u, name %s\n",
		    event->action, event->name);

	spin_lock(&notify->lock);
	list_add_tail(&event->list, &notify->events);
	notify->num_events++;
	spin_unlock(&notify->lock);
}

static void
ksmbd_notify_trigger_removed(struct ksmbd_notify *notify,
			     struct ksmbd_notify_event *event)
{
	ksmbd_debug(NOTIFY,
		    "Queue moved from as removed, name %s\n",
		    event->name);
	event->action = FILE_ACTION_REMOVED;
	event->when = ktime_get_ns();
	ksmbd_notify_queue_event(notify, event);
}

static void ksmbd_notify_moved_from_timeout(struct work_struct *work)
{
	struct ksmbd_notify_event *event;
	struct ksmbd_notify *notify;

	notify = container_of(to_delayed_work(work), struct ksmbd_notify,
			      moved_from_work);

	spin_lock(&notify->lock);
	event = notify->moved_from_event;
	notify->moved_from_event = NULL;
	spin_unlock(&notify->lock);

	if (!event)
		return;
	ksmbd_notify_trigger_removed(notify, event);
}

static void ksmbd_notify_save_moved_from(struct ksmbd_notify *notify,
					 u32 mask, u32 cookie,
					 const struct qstr *file_name)
{
	struct ksmbd_notify_event *event, *old_event;

	event = ksmbd_notify_alloc_event(FILE_ACTION_RENAMED_OLD_NAME,
					 file_name, KSMBD_DEFAULT_GFP);
	if (!event)
		return;

	spin_lock(&notify->lock);
	old_event = notify->moved_from_event;
	notify->moved_from_event = event;
	notify->moved_from_mask = mask;
	notify->moved_from_cookie = cookie;
	spin_unlock(&notify->lock);

	ksmbd_debug(NOTIFY,
		    "Saved moved from, mask 0x%x, name %.*s, cookie %u\n",
		    mask, file_name ? file_name->len : 0,
		    file_name ? (const char *)file_name->name : "", cookie);
	if (old_event)
		ksmbd_notify_trigger_removed(notify, old_event);

	mod_delayed_work(system_dfl_wq, &notify->moved_from_work,
			 msecs_to_jiffies(KSMBD_NOTIFY_MOVED_FROM_MSECS));
}

static bool
ksmbd_notify_handle_rename(struct ksmbd_notify *notify, u32 mask,
				 u32 cookie, const struct qstr *file_name)
{
	struct ksmbd_notify_event *from, *to;
	u32 from_mask;

	to = ksmbd_notify_alloc_event(FILE_ACTION_RENAMED_NEW_NAME, file_name,
				      KSMBD_DEFAULT_GFP);
	if (!to)
		return false;

	spin_lock(&notify->lock);
	if (!notify->moved_from_event ||
	    notify->moved_from_cookie != cookie) {
		spin_unlock(&notify->lock);
		kfree(to);
		ksmbd_debug(NOTIFY,
			    "No rename source for destination %.*s, cookie %u\n",
			    file_name ? file_name->len : 0,
			    file_name ? (const char *)file_name->name : "",
			    cookie);
		return false;
	}
	from = notify->moved_from_event;
	from_mask = notify->moved_from_mask;
	notify->moved_from_event = NULL;
	notify->moved_from_mask = 0;
	notify->moved_from_cookie = 0;
	cancel_delayed_work(&notify->moved_from_work);
	spin_unlock(&notify->lock);

	from->action = FILE_ACTION_RENAMED_OLD_NAME;
	ksmbd_debug(NOTIFY, "Matched rename %.*s to %.*s, cookie %u\n",
		    (int)from->name_len, from->name,
		    file_name ? file_name->len : 0,
		    file_name ? (const char *)file_name->name : "", cookie);

	if (!ksmbd_notify_filter_match(notify, from_mask)) {
		ksmbd_debug(NOTIFY, "Filtered rename source %.*s\n",
			    (int)from->name_len, from->name);
		kfree(from);
		from = NULL;
	}

	if (!ksmbd_notify_filter_match(notify, mask)) {
		ksmbd_debug(NOTIFY, "Filtered rename destination %.*s\n",
			    file_name ? file_name->len : 0,
			    file_name ? (const char *)file_name->name : "");
		kfree(to);
		to = NULL;
	}

	if (from)
		ksmbd_notify_queue_event(notify, from);
	if (to)
		ksmbd_notify_queue_event(notify, to);

	return true;
}

static int ksmbd_notify_handle_inode_event(struct fsnotify_mark *mark,
					   u32 mask, struct inode *inode,
					   struct inode *dir,
					   const struct qstr *file_name,
					   u32 cookie)
{
	struct ksmbd_notify_mark *notify_mark;
	struct ksmbd_notify_event *event;
	struct ksmbd_notify *notify;
	struct inode *event_inode = dir ?: inode;
	struct ksmbd_file *fp;
	u32 action;

	notify_mark = container_of(mark, struct ksmbd_notify_mark, mark);
	notify = notify_mark->notify;
	fp = notify->fp;

	ksmbd_debug(NOTIFY,
		    "fid %llu:%llu, notify event: mask=0x%08x inode=%llu name=%.*s cookie=%u\n",
		    fp->persistent_id, fp->volatile_id, mask,
		    event_inode ? (unsigned long long)event_inode->i_ino : 0,
		    file_name ? file_name->len : 0,
		    file_name ? (const char *)file_name->name : "", cookie);

	if (!(mask & KSMBD_NOTIFY_EVENT_MASK)) {
		ksmbd_debug(NOTIFY, "Ignored notify event mask 0x%x\n", mask);
		return 0;
	}

	if (mask & FS_MOVED_FROM) {
		ksmbd_notify_save_moved_from(notify, mask, cookie, file_name);
		return 0;
	}

	if (mask & FS_MOVED_TO) {
		if (ksmbd_notify_handle_rename(notify, mask, cookie,
					       file_name))
			return 0;
		action = FILE_ACTION_ADDED;
	} else if (mask & FS_CREATE) {
		action = FILE_ACTION_ADDED;
	} else if (mask & FS_DELETE) {
		action = FILE_ACTION_REMOVED;
	} else {
		action = FILE_ACTION_MODIFIED;
	}

	if (!ksmbd_notify_filter_match(notify, mask))
		return 0;

	event = ksmbd_notify_alloc_event(action, file_name,
					 KSMBD_DEFAULT_GFP);
	if (!event)
		return 0;

	ksmbd_notify_queue_event(notify, event);

	return 0;
}

static void ksmbd_notify_free_mark(struct fsnotify_mark *mark)
{
	struct ksmbd_notify_mark *notify_mark;

	notify_mark = container_of(mark, struct ksmbd_notify_mark, mark);
	kfree(notify_mark);
}

static const struct fsnotify_ops ksmbd_notify_fsnotify_ops = {
	.handle_inode_event = ksmbd_notify_handle_inode_event,
	.free_mark = ksmbd_notify_free_mark,
};

static struct fsnotify_mark *
ksmbd_notify_add_mark(struct ksmbd_notify *notify, u32 mask,
		      struct fsnotify_group **group)
{
	struct ksmbd_notify_mark *notify_mark;
	int err;

	*group = fsnotify_alloc_group(&ksmbd_notify_fsnotify_ops, 0);
	if (IS_ERR(*group)) {
		pr_err("Failed to allocate fsnotify group: %ld\n",
		       PTR_ERR(*group));
		return ERR_CAST(*group);
	}

	notify_mark = kzalloc_obj(*notify_mark, KSMBD_DEFAULT_GFP);
	if (!notify_mark) {
		pr_err("Failed to allocate fsnotify mark\n");
		err = -ENOMEM;
		goto err_put_group;
	}

	notify_mark->notify = notify;
	fsnotify_init_mark(&notify_mark->mark, *group);
	notify_mark->mark.mask = mask | FS_EVENT_ON_CHILD;
	err = fsnotify_add_inode_mark(&notify_mark->mark,
				      file_inode(notify->fp->filp), 0);
	if (err) {
		pr_err("Failed to add fsnotify mark, inode %llu: %d\n",
		       (unsigned long long)file_inode(notify->fp->filp)->i_ino,
		       err);
		goto err_put_mark;
	}

	return &notify_mark->mark;

err_put_mark:
	fsnotify_put_mark(&notify_mark->mark);
err_put_group:
	fsnotify_put_group(*group);
	return ERR_PTR(err);
}

static void ksmbd_notify_destroy_mark(struct fsnotify_group *group,
				      struct fsnotify_mark *mark)
{
	fsnotify_destroy_mark(mark, group);
	fsnotify_put_mark(mark);
	fsnotify_wait_marks_destroyed();
	fsnotify_put_group(group);
}

static int ksmbd_notify_add(struct ksmbd_file *fp, u32 mask, u32 filter,
			    u32 max_buffer_size,
			    struct ksmbd_notify **notify_out)
{
	struct ksmbd_notify *notify;
	struct fsnotify_mark *mark;
	int err = 0;

	mutex_lock(&fp->notify_lock);
	if (fp->notify) {
		spin_lock(&fp->notify->lock);
		fp->notify->filter |= filter;
		fp->notify->mask |= mask;
		spin_unlock(&fp->notify->lock);
		fsnotify_modify_mark_mask(fp->notify->mark, mask, 0);
		ksmbd_debug(NOTIFY,
			    "Updated fsnotify mark, inode %llu, mask 0x%x\n",
			    (unsigned long long)file_inode(fp->filp)->i_ino,
			    fp->notify->mark->mask);
		*notify_out = fp->notify;
		goto out;
	}

	notify = kzalloc_obj(*notify, KSMBD_DEFAULT_GFP);
	if (!notify) {
		pr_err("Failed to allocate notify watch\n");
		err = -ENOMEM;
		goto out;
	}

	notify->fp = fp;
	spin_lock_init(&notify->lock);
	INIT_LIST_HEAD(&notify->events);
	notify->filter = filter;
	notify->mask = mask;
	notify->max_buffer_size = max_buffer_size;
	INIT_DELAYED_WORK(&notify->moved_from_work,
			  ksmbd_notify_moved_from_timeout);

	mark = ksmbd_notify_add_mark(notify, mask, &notify->group);
	if (IS_ERR(mark)) {
		err = PTR_ERR(mark);
		kfree(notify);
		goto out;
	}

	notify->mark = mark;
	fp->notify = notify;
	*notify_out = notify;
	ksmbd_debug(NOTIFY,
		    "Added fsnotify mark, inode %llu, mask 0x%x, filter 0x%x, "
		    "output buffer length %u\n",
		    (unsigned long long)file_inode(fp->filp)->i_ino, mark->mask,
		    filter, max_buffer_size);

out:
	mutex_unlock(&fp->notify_lock);
	return err;
}

/**
 * ksmbd_notify_remove() - remove the notify watch for a closing handle
 * @fp: file handle whose watch is being removed
 *
 * A cancelled CHANGE_NOTIFY request leaves this watch installed. The watch is
 * owned by @fp and removed only when the file handle is finally closed.
 */
void ksmbd_notify_remove(struct ksmbd_file *fp)
{
	struct ksmbd_notify *notify;

	mutex_lock(&fp->notify_lock);
	notify = fp->notify;
	fp->notify = NULL;
	mutex_unlock(&fp->notify_lock);
	if (!notify)
		return;

	ksmbd_debug(NOTIFY,
		    "Removing fsnotify mark, inode %llu, mask 0x%x\n",
		    (unsigned long long)file_inode(fp->filp)->i_ino,
		    notify->mark->mask);
	ksmbd_notify_destroy_mark(notify->group, notify->mark);
	cancel_delayed_work_sync(&notify->moved_from_work);
	kfree(notify->moved_from_event);
	ksmbd_notify_free_events(&notify->events);
	kfree(notify);
}

static int ksmbd_notify_event_cmp(void *priv, const struct list_head *a,
				  const struct list_head *b)
{
	struct ksmbd_notify_event *event_a;
	struct ksmbd_notify_event *event_b;

	event_a = list_entry(a, struct ksmbd_notify_event, list);
	event_b = list_entry(b, struct ksmbd_notify_event, list);

	if (event_a->when < event_b->when)
		return -1;
	if (event_a->when > event_b->when)
		return 1;
	return 0;
}

static void *ksmbd_notify_encode_events(struct ksmbd_work *work,
					struct list_head *events,
					u32 max_len, size_t *data_len)
{
	struct ksmbd_notify_event *event;
	u8 *data = NULL;
	size_t len = 0;

	list_sort(NULL, events, ksmbd_notify_event_cmp);

	list_for_each_entry(event, events, list) {
		struct file_notify_information *info;
		struct ksmbd_notify_event *next;
		size_t alloc_len, name_buf_len, record_len;
		bool last = list_is_last(&event->list, events);
		__le16 *name;
		u8 *new_data;
		int name_len;

		/* Coalesce adjacent, case-sensitive duplicate records. */
		if (!last) {
			next = list_next_entry(event, list);
			if (event->action == next->action &&
			    event->name_len == next->name_len &&
			    !memcmp(event->name, next->name, event->name_len))
				continue;
		}

		name_buf_len = (event->name_len + 1) * sizeof(__le16);
		name = kmalloc(name_buf_len, KSMBD_DEFAULT_GFP);
		if (!name) {
			pr_err("Failed to allocate notify event name buffer\n");
			goto fail;
		}

		name_len = smbConvertToUTF16(name, event->name,
					     event->name_len,
					     work->conn->local_nls, 0);
		name_len *= sizeof(__le16);
		record_len = sizeof(*info) + name_len;
		alloc_len = ALIGN(record_len, 4);
		if (len > SIZE_MAX - alloc_len) {
			pr_err("Notify event data length overflow\n");
			kfree(name);
			goto fail;
		}

		new_data = kvrealloc(data, len + alloc_len, KSMBD_DEFAULT_GFP);
		if (!new_data) {
			pr_err("Failed to allocate notify event data, length %zu\n",
			       len + alloc_len);
			kfree(name);
			goto fail;
		}
		data = new_data;
		memset(data + len, 0, alloc_len);

		info = (struct file_notify_information *)(data + len);
		info->NextEntryOffset = last ? 0 : cpu_to_le32(alloc_len);
		info->Action = cpu_to_le32(event->action);
		info->FileNameLength = cpu_to_le32(name_len);
		memcpy(info->FileName, name, name_len);
		kfree(name);

		len += alloc_len;
		if (len > max_len) {
			ksmbd_debug(NOTIFY,
				    "Notify event data length %zu exceeds output buffer %u\n",
				    len, max_len);
			goto fail;
		}
	}

	*data_len = len;
	return data;

fail:
	kvfree(data);
	*data_len = 0;
	return NULL;
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
	if (!mask) {
		pr_err("Unsupported completion filter 0x%x\n", filter);
		rsp->hdr.Status = STATUS_INVALID_PARAMETER;
		return ERR_PTR(-EINVAL);
	}

	err = ksmbd_notify_add(fp, mask, filter,
			       le32_to_cpu(req->OutputBufferLength), &notify);
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

	err = ksmbd_notify_wait(work, notify, notify_req);

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
