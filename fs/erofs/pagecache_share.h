/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024, Alibaba Cloud
 */
#ifndef __EROFS_PAGECACHE_SHARE_H
#define __EROFS_PAGECACHE_SHARE_H

#include <linux/mutex.h>
#include "internal.h"

void erofs_pcs_fill_inode(struct inode *inode);
int erofs_pcs_add(struct inode *inode);
int erofs_pcs_remove(struct inode *inode);

#endif
