// SPDX-License-Identifier: GPL-2.0
/*
 * Directory change notification tracking for SMB
 *
 * Copyright (c) 2025, Sang-Heon Jeon <ekffu200098@gmail.com>
 *
 * References:
 * MS-SMB2 "2.2.35 SMB2 CHANGE_NOTIFY Request"
 * MS-SMB2 "2.2.36 SMB2 CHANGE_NOTIFY Response"
 * MS-SMB2 "2.7.1 FILE_NOTIFY_INFORMATION"
 * MS-SMB2 "3.3.5.19 Receiving and SMB2 CHANGE_NOTIFY Request"
 * MS-FSCC "2.6 File Attributes"
 */

#include <linux/list.h>
#include <linux/slab.h>
#include <linux/fsnotify.h>
#include "notify.h"
#include "cifsproto.h"
#include "smb2proto.h"
#include "cached_dir.h"
#include "cifs_debug.h"
#include "cifspdu.h"
#include "cifs_unicode.h"
#include "../common/smb2pdu.h"
#include "../common/smb2status.h"

#define CLEANUP_INTERVAL (30 * HZ)
#define CLEANUP_IMMEDIATE 0

enum notify_state {
	NOTIFY_STATE_RECONNECT = BIT(0),
	NOTIFY_STATE_UMOUNT = BIT(1),
	NOTIFY_STATE_NOMASK = BIT(2),
	NOTIFY_STATE_BROKEN_REQ = BIT(3),
	NOTIFY_STATE_BROKEN_RSP = BIT(4),
};

struct notify_info {
	struct inode *inode;
	const char *path;
	__le16 *utf16_path;
	struct cifs_fid cifs_fid;
	atomic_t state;
	struct list_head list;
};

static int request_change_notify(struct notify_info *info);
static void notify_cleanup_worker(struct work_struct *work);

static LIST_HEAD(notify_list);
static DEFINE_SPINLOCK(notify_list_lock);
static DECLARE_DELAYED_WORK(notify_cleanup_work, notify_cleanup_worker);

static bool is_resumeable(struct notify_info *info)
{
	return atomic_read(&info->state) == NOTIFY_STATE_RECONNECT;
}

static bool is_active(struct notify_info *info)
{
	return atomic_read(&info->state) == 0;
}

static void set_state(struct notify_info *info, int state)
{
	atomic_or(state, &info->state);
	if (!is_resumeable(info))
		mod_delayed_work(cifsiod_wq, &notify_cleanup_work,
			CLEANUP_IMMEDIATE);
}

static void clear_state(struct notify_info *info, int state)
{
	atomic_and(~state, &info->state);
}

static int fsnotify_send(__u32 mask,
			 struct inode *parent,
			 struct file_notify_information *fni,
			 u32 cookie)
{
	char *name = cifs_strndup_from_utf16(fni->FileName,
				le32_to_cpu(fni->FileNameLength), true,
				CIFS_SB(parent->i_sb)->local_nls);
	struct qstr qstr;
	int rc = 0;

	if (!name)
		return -ENOMEM;

	qstr.name = (const unsigned char *)name;
	qstr.len = strlen(name);

	rc = fsnotify_name(mask, NULL, FSNOTIFY_EVENT_NONE, parent,
			&qstr, cookie);
	cifs_dbg(FYI, "fsnotify mask=%u, name=%s, cookie=%u, w/return=%d",
		mask, name, cookie, rc);
	kfree(name);
	return rc;
}

static bool is_fsnotify_masked(struct inode *inode)
{
	if (!inode)
		return false;

	/* Minimal validation of file explore inotify */
	return inode->i_fsnotify_mask &
		(FS_CREATE | FS_DELETE | FS_MOVED_FROM | FS_MOVED_TO);
}

static void handle_file_notify_information(struct notify_info *info,
					   char *buf,
					   unsigned int buf_len)
{
	struct file_notify_information *fni;
	unsigned int next_entry_offset;
	u32 cookie;
	bool has_cookie = false;

	do {
		fni = (struct file_notify_information *)buf;
		next_entry_offset = le32_to_cpu(fni->NextEntryOffset);
		if (next_entry_offset > buf_len) {
			cifs_dbg(FYI, "invalid fni->NextEntryOffset=%u",
				next_entry_offset);
			break;
		}

		switch (le32_to_cpu(fni->Action)) {
		case FILE_ACTION_ADDED:
			fsnotify_send(FS_CREATE, info->inode, fni, 0);
			break;
		case FILE_ACTION_REMOVED:
			fsnotify_send(FS_DELETE, info->inode, fni, 0);
			break;
		case FILE_ACTION_RENAMED_OLD_NAME:
			if (!has_cookie)
				cookie = fsnotify_get_cookie();
			has_cookie = !has_cookie;
			fsnotify_send(FS_MOVED_FROM, info->inode, fni, cookie);
			break;
		case FILE_ACTION_RENAMED_NEW_NAME:
			if (!has_cookie)
				cookie = fsnotify_get_cookie();
			has_cookie = !has_cookie;
			fsnotify_send(FS_MOVED_TO, info->inode, fni, cookie);
			break;
		default:
			/* Does not occur, so no need to handle */
			break;
		}

		buf += next_entry_offset;
		buf_len -= next_entry_offset;
	} while (buf_len > 0 && next_entry_offset > 0);
}

static void handle_smb2_change_notify_rsp(struct notify_info *info,
					  struct mid_q_entry *mid)
{
	struct smb2_change_notify_rsp *rsp = mid->resp_buf;
	struct kvec rsp_iov;
	unsigned int buf_offset, buf_len;
	int rc;

	switch (rsp->hdr.Status) {
	case STATUS_SUCCESS:
		break;
	case STATUS_NOTIFY_ENUM_DIR:
		goto proceed;
	case STATUS_USER_SESSION_DELETED:
	case STATUS_NETWORK_NAME_DELETED:
	case STATUS_NETWORK_SESSION_EXPIRED:
		set_state(info, NOTIFY_STATE_RECONNECT);
		return;
	default:
		set_state(info, NOTIFY_STATE_BROKEN_RSP);
		return;
	}

	rsp_iov.iov_base = mid->resp_buf;
	rsp_iov.iov_len = mid->resp_buf_size;
	buf_offset = le16_to_cpu(rsp->OutputBufferOffset);
	buf_len = le32_to_cpu(rsp->OutputBufferLength);

	rc = smb2_validate_iov(buf_offset, buf_len, &rsp_iov,
				sizeof(struct file_notify_information));
	if (rc) {
		cifs_dbg(FYI, "stay tracking, w/smb2_validate_iov=%d", rc);
		goto proceed;
	}

	handle_file_notify_information(info,
		(char *)rsp + buf_offset, buf_len);
proceed:
	request_change_notify(info);
	return;
}

static void change_notify_callback(struct mid_q_entry *mid)
{
	struct notify_info *info = mid->callback_data;

	if (!is_active(info))
		return;

	if (!is_fsnotify_masked(info->inode)) {
		set_state(info, NOTIFY_STATE_NOMASK);
		return;
	}

	if (!mid->resp_buf) {
		if (mid->mid_state != MID_RETRY_NEEDED) {
			cifs_dbg(FYI, "stop tracking, w/mid_state=%d",
				mid->mid_state);
			set_state(info, NOTIFY_STATE_BROKEN_RSP);
			return;
		}

		set_state(info, NOTIFY_STATE_RECONNECT);
		return;
	}

	handle_smb2_change_notify_rsp(info, mid);
}

static int request_change_notify(struct notify_info *info)
{
	struct cifs_sb_info *cifs_sb = CIFS_SB(info->inode->i_sb);
	struct cifs_tcon *tcon = cifs_sb_master_tcon(cifs_sb);
	struct smb_rqst rqst;
	struct kvec iov[1];
	unsigned int xid;
	int rc;

	if (!tcon) {
		cifs_dbg(FYI, "missing tcon while request change notify");
		return -EINVAL;
	}

	memset(&rqst, 0, sizeof(struct smb_rqst));
	memset(&iov, 0, sizeof(iov));
	rqst.rq_iov = iov;
	rqst.rq_nvec = 1;

	xid = get_xid();
	rc = SMB2_notify_init(xid, &rqst, tcon, tcon->ses->server,
		info->cifs_fid.persistent_fid, info->cifs_fid.volatile_fid,
		FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME,
		false);
	free_xid(xid);
	if (rc) {
		set_state(info, NOTIFY_STATE_BROKEN_REQ);
		return rc;
	}

	rc = cifs_call_async(tcon->ses->server, &rqst, NULL,
		change_notify_callback, NULL, info, 0, NULL);
	cifs_small_buf_release(rqst.rq_iov[0].iov_base);

	if (rc)
		set_state(info, NOTIFY_STATE_BROKEN_REQ);
	return rc;
}


static void free_notify_info(struct notify_info *info)
{
	kfree(info->utf16_path);
	kfree(info->path);
	kfree(info);
}

static void cleanup_pending_mid(struct notify_info *info,
				struct cifs_tcon *tcon)
{
	LIST_HEAD(dispose_list);
	struct TCP_Server_Info *server;
	struct mid_q_entry *mid, *nmid;

	if (!tcon->ses || !tcon->ses->server)
		return;

	server = tcon->ses->server;

	spin_lock(&server->mid_queue_lock);
	list_for_each_entry_safe(mid, nmid, &server->pending_mid_q, qhead) {
		if (mid->callback_data == info) {
			mid->deleted_from_q = true;
			list_move(&mid->qhead, &dispose_list);
		}
	}
	spin_unlock(&server->mid_queue_lock);

	list_for_each_entry_safe(mid, nmid, &dispose_list, qhead) {
		list_del_init(&mid->qhead);
		release_mid(mid);
	}
}

static void close_fid(struct notify_info *info)
{
	struct cifs_tcon *tcon;

	unsigned int xid;
	int rc;

	if (!info->cifs_fid.persistent_fid && !info->cifs_fid.volatile_fid)
		return;

	tcon = cifs_sb_master_tcon(CIFS_SB(info->inode->i_sb));
	if (!tcon) {
		cifs_dbg(FYI, "missing master tcon while close");
		return;
	}

	xid = get_xid();
	rc = SMB2_close(xid, tcon, info->cifs_fid.persistent_fid,
		info->cifs_fid.volatile_fid);
	if (rc) {
		cifs_dbg(FYI, "cleanup pending mid, w/SMB2_close=%d", rc);
		cleanup_pending_mid(info, tcon);
	}
	free_xid(xid);
}

static int setup_fid(struct notify_info *info)
{
	struct cifs_sb_info *cifs_sb = CIFS_SB(info->inode->i_sb);
	struct cifs_tcon *tcon = cifs_sb_master_tcon(cifs_sb);
	struct cifs_open_parms oparms;
	__u8 oplock = 0;
	unsigned int xid;
	int rc = 0;

	if (!tcon) {
		cifs_dbg(FYI, "missing master tcon while open");
		return -EINVAL;
	}

	xid = get_xid();
	oparms = (struct cifs_open_parms) {
		.tcon = tcon,
		.path = info->path,
		.desired_access = GENERIC_READ,
		.disposition = FILE_OPEN,
		.create_options = cifs_create_options(cifs_sb, 0),
		.fid = &info->cifs_fid,
		.cifs_sb = cifs_sb,
		.reconnect = false,
	};
	rc = SMB2_open(xid, &oparms, info->utf16_path, &oplock,
			NULL, NULL, NULL, NULL);
	free_xid(xid);
	return rc;
}

static bool is_already_tracking(struct inode *dir_inode)
{
	struct notify_info *entry, *nentry;

	spin_lock(&notify_list_lock);
	list_for_each_entry_safe(entry, nentry, &notify_list, list) {
		if (is_active(entry)) {
			if (entry->inode == dir_inode) {
				spin_unlock(&notify_list_lock);
				return true;
			}

			/* Extra check since we must keep iterating */
			if (!is_fsnotify_masked(entry->inode))
				set_state(entry, NOTIFY_STATE_NOMASK);
		}
	}
	spin_unlock(&notify_list_lock);

	return false;
}

static bool is_tracking_supported(struct cifs_sb_info *cifs_sb)
{
	struct cifs_tcon *tcon = cifs_sb_master_tcon(cifs_sb);

	if (!tcon->ses || !tcon->ses->server)
		return false;
	return tcon->ses->server->dialect >= SMB20_PROT_ID;
}

int start_track_dir_changes(const char *path,
			    struct inode *dir_inode,
			    struct cifs_sb_info *cifs_sb)
{
	struct notify_info *info;
	int rc;

	if (!is_tracking_supported(cifs_sb))
		return -EINVAL;

	if (!is_fsnotify_masked(dir_inode))
		return -EINVAL;

	if (is_already_tracking(dir_inode))
		return 1;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->path = kstrdup(path, GFP_KERNEL);
	if (!info->path) {
		free_notify_info(info);
		return -ENOMEM;
	}
	info->utf16_path = cifs_convert_path_to_utf16(path, cifs_sb);
	if (!info->utf16_path) {
		free_notify_info(info);
		return -ENOMEM;
	}
	info->inode = dir_inode;

	rc = setup_fid(info);
	if (rc) {
		free_notify_info(info);
		return rc;
	}
	rc = request_change_notify(info);
	if (rc) {
		close_fid(info);
		free_notify_info(info);
		return rc;
	}

	spin_lock(&notify_list_lock);
	list_add(&info->list, &notify_list);
	spin_unlock(&notify_list_lock);
	return rc;
}

void stop_track_sb_dir_changes(struct cifs_sb_info *cifs_sb)
{
	struct notify_info *entry, *nentry;

	if (!list_empty(&notify_list)) {
		spin_lock(&notify_list_lock);
		list_for_each_entry_safe(entry, nentry, &notify_list, list) {
			if (cifs_sb == CIFS_SB(entry->inode->i_sb)) {
				set_state(entry, NOTIFY_STATE_UMOUNT);
				continue;
			}

			/* Extra check since we must keep iterating */
			if (!is_fsnotify_masked(entry->inode))
				set_state(entry, NOTIFY_STATE_NOMASK);
		}
		spin_unlock(&notify_list_lock);
	}
}

void resume_track_dir_changes(void)
{
	LIST_HEAD(resume_list);
	struct notify_info *entry, *nentry;
	struct cifs_tcon *tcon;

	if (list_empty(&notify_list))
		return;

	spin_lock(&notify_list_lock);
	list_for_each_entry_safe(entry, nentry, &notify_list, list) {
		if (!is_fsnotify_masked(entry->inode)) {
			set_state(entry, NOTIFY_STATE_NOMASK);
			continue;
		}

		if (is_resumeable(entry)) {
			tcon = cifs_sb_master_tcon(CIFS_SB(entry->inode->i_sb));
			spin_lock(&tcon->tc_lock);
			if (tcon->status == TID_GOOD) {
				spin_unlock(&tcon->tc_lock);
				list_move(&entry->list, &resume_list);
				continue;
			}
			spin_unlock(&tcon->tc_lock);
		}
	}
	spin_unlock(&notify_list_lock);

	list_for_each_entry_safe(entry, nentry, &resume_list, list) {
		if (setup_fid(entry)) {
			list_del(&entry->list);
			free_notify_info(entry);
			continue;
		}

		if (request_change_notify(entry)) {
			list_del(&entry->list);
			close_fid(entry);
			free_notify_info(entry);
			continue;
		}

		clear_state(entry, NOTIFY_STATE_RECONNECT);
	}

	if (!list_empty(&resume_list)) {
		spin_lock(&notify_list_lock);
		list_splice(&resume_list, &notify_list);
		spin_unlock(&notify_list_lock);
	}
}

static void notify_cleanup_worker(struct work_struct *work)
{
	LIST_HEAD(cleanup_list);
	struct notify_info *entry, *nentry;

	if (list_empty(&notify_list))
		return;

	spin_lock(&notify_list_lock);
	list_for_each_entry_safe(entry, nentry, &notify_list, list) {
		if (!is_resumeable(entry) && !is_active(entry))
			list_move(&entry->list, &cleanup_list);
	}
	spin_unlock(&notify_list_lock);

	list_for_each_entry_safe(entry, nentry, &cleanup_list, list) {
		list_del(&entry->list);
		close_fid(entry);
		free_notify_info(entry);
	}
	mod_delayed_work(cifsiod_wq, &notify_cleanup_work, CLEANUP_INTERVAL);
}
