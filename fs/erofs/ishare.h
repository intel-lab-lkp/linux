/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024, Alibaba Cloud
 */
#ifndef __EROFS_ISHARE_H
#define __EROFS_ISHARE_H

#include <linux/fs.h>
#include <linux/spinlock.h>
#include "internal.h"

struct erofs_read_ctx {
	struct file *file; /* may be NULL */
	struct inode *inode;
};

#ifdef CONFIG_EROFS_FS_INODE_SHARE

int erofs_ishare_init(struct super_block *sb);
void erofs_ishare_exit(struct super_block *sb);
bool erofs_ishare_fill_inode(struct inode *inode);
void erofs_ishare_free_inode(struct inode *inode);

/* read/readahead */
void erofs_read_begin(struct erofs_read_ctx *rdctx);
void erofs_read_end(struct erofs_read_ctx *rdctx);

struct inode *erofs_ishare_iget(struct inode *inode);
void erofs_ishare_iput(struct inode *realinode);

#else

static inline int erofs_ishare_init(struct super_block *sb) { return 0; }
static inline void erofs_ishare_exit(struct super_block *sb) {}
static inline bool erofs_ishare_fill_inode(struct inode *inode) { return false; }
static inline void erofs_ishare_free_inode(struct inode *inode) {}

static inline void erofs_read_begin(struct erofs_read_ctx *rdctx) {}
static inline void erofs_read_end(struct erofs_read_ctx *rdctx) {}

static inline struct inode *erofs_ishare_iget(struct inode *inode) { return inode; }
static inline void erofs_ishare_iput(struct inode *realinode) {}

#endif // CONFIG_EROFS_FS_INODE_SHARE

#endif
