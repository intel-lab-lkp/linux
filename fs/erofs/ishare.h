/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024, Alibaba Cloud
 */
#ifndef __EROFS_ISHARE_H
#define __EROFS_ISHARE_H

#include <linux/fs.h>
#include <linux/spinlock.h>
#include "internal.h"

#ifdef CONFIG_EROFS_FS_INODE_SHARE

int erofs_ishare_init(struct super_block *sb);
void erofs_ishare_exit(struct super_block *sb);
bool erofs_ishare_fill_inode(struct inode *inode);
void erofs_ishare_free_inode(struct inode *inode);

#else

static inline int erofs_ishare_init(struct super_block *sb) { return 0; }
static inline void erofs_ishare_exit(struct super_block *sb) {}
static inline bool erofs_ishare_fill_inode(struct inode *inode) { return false; }
static inline void erofs_ishare_free_inode(struct inode *inode) {}

#endif // CONFIG_EROFS_FS_INODE_SHARE

#endif
