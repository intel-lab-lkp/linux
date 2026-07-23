/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 *
 *   SMB2 CHANGE_NOTIFY
 *
 *   Copyright (C) 2026 KylinSoft Co., Ltd. All rights reserved.
 *   Author(s): ChenXiaoSong <chenxiaosong@kylinos.cn>
 *
 */

#ifndef __SMB_SERVER_NOTIFY_H__
#define __SMB_SERVER_NOTIFY_H__

struct ksmbd_work;
struct smb2_change_notify_req;
struct smb2_change_notify_rsp;

int ksmbd_handle_notify(struct ksmbd_work *work,
			struct smb2_change_notify_req *req,
			struct smb2_change_notify_rsp *rsp);

#endif /* __SMB_SERVER_NOTIFY_H__ */
