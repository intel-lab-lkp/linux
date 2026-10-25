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

struct ksmbd_work;
struct smb2_change_notify_req;
struct smb2_change_notify_rsp;

struct ksmbd_notify {
	struct fsnotify_group *group;
	struct fsnotify_mark *mark;
	struct ksmbd_file *fp;
	/* Protects filter, rename state and the queued events. */
	spinlock_t lock;
	u32 filter;
	u32 mask;
};

struct ksmbd_notify_req {
	wait_queue_head_t wait;
};

#define KSMBD_NOTIFY_NAME_EVENT_MASK	(FS_CREATE | FS_DELETE | \
					 FS_MOVED_FROM | FS_MOVED_TO)

int ksmbd_handle_notify(struct ksmbd_work *work,
			struct smb2_change_notify_req *req,
			struct smb2_change_notify_rsp *rsp);

#endif /* __SMB_SERVER_NOTIFY_H__ */
