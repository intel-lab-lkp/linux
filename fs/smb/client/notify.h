/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Directory change notification tracking for SMB
 *
 * Copyright (c) 2025, Sang-Heon Jeon <ekffu200098@gmail.com>
 */

#ifndef _SMB_NOTIFY_H
#define _SMB_NOTIFY_H

#include "cifsglob.h"

int start_track_dir_changes(const char *path,
			    struct inode *dir_inode,
			    struct cifs_sb_info *cifs_sb);
void stop_track_sb_dir_changes(struct cifs_sb_info *cifs_sb);
void resume_track_dir_changes(void);

#endif /* _SMB_NOTIFY_H */
