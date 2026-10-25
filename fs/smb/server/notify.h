/* SPDX-License-Identifier: GPL-2.0-or-later */
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

#ifndef __SMB_SERVER_NOTIFY_H__
#define __SMB_SERVER_NOTIFY_H__

#include <linux/fsnotify_backend.h>
#include <linux/sizes.h>

struct ksmbd_work;
struct ksmbd_file;
struct smb2_change_notify_req;
struct smb2_change_notify_rsp;

struct ksmbd_notify_event {
	struct list_head list;
	u32 action;
	u64 when;
	size_t name_len;
	size_t alloc_size;
	char name[];
};

struct ksmbd_notify {
	struct fsnotify_group *group;
	struct fsnotify_mark *mark;
	struct fsnotify_mark *tree_mark;
	struct ksmbd_file *fp;
	/* Protects filter, rename state and the queued events. */
	spinlock_t lock;
	struct list_head events;
	unsigned int num_events;
	/* Includes reservations, rename state and events owned by requests. */
	size_t event_bytes;
	bool overflow;
	u32 filter;
	u32 mask;
	bool watch_tree;
};

struct ksmbd_notify_req {
	wait_queue_head_t wait;
};

#define KSMBD_NOTIFY_MAX_BYTES		SZ_64K
#define KSMBD_NOTIFY_NAME_EVENT_MASK	(FS_CREATE | FS_DELETE | \
					 FS_MOVED_FROM | FS_MOVED_TO)

int ksmbd_handle_notify(struct ksmbd_work *work,
			struct smb2_change_notify_req *req,
			struct smb2_change_notify_rsp *rsp);
void ksmbd_notify_remove(struct ksmbd_file *fp);

#endif /* __SMB_SERVER_NOTIFY_H__ */
