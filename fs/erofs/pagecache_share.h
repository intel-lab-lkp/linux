/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024, Alibaba Cloud
 */
#ifndef __EROFS_PAGECACHE_SHARE_H
#define __EROFS_PAGECACHE_SHARE_H

#include <linux/fs.h>

#ifdef CONFIG_EROFS_FS_PAGE_CACHE_SHARE

int erofs_pcshr_init_mnt(void);
void erofs_pcshr_free_mnt(void);
int erofs_pcshr_fill_inode(struct inode *inode);
void erofs_pcshr_free_inode(struct inode *inode);

/* switch between the anonymous inode and the real inode */
int erofs_pcshr_read_begin(struct file *file, struct folio *folio);
void erofs_pcshr_read_end(struct file *file, struct folio *folio, int pcshr);
int erofs_pcshr_readahead_begin(struct readahead_control *rac);
void erofs_pcshr_readahead_end(struct readahead_control *rac, int pcshr);

#else

static inline int erofs_pcshr_init_mnt(void) { return 0; }
static inline void erofs_pcshr_free_mnt(void) {}
static inline int erofs_pcshr_fill_inode(struct inode *inode) { return -1; }
static inline void erofs_pcshr_free_inode(struct inode *inode) {}

static inline int erofs_pcshr_read_begin(struct file *file, struct folio *folio) { return 0; }
static inline void erofs_pcshr_read_end(struct file *file, struct folio *folio, int pcshr) {}
static inline int erofs_pcshr_readahead_begin(struct readahead_control *rac) { return 0; }
static inline void erofs_pcshr_readahead_end(struct readahead_control *rac, int pcshr) {}

#endif // CONFIG_EROFS_FS_PAGE_CACHE_SHARE

#endif
