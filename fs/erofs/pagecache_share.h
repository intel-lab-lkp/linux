/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024, Alibaba Cloud
 */
#ifndef __EROFS_PAGECACHE_SHARE_H
#define __EROFS_PAGECACHE_SHARE_H

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/rwlock.h>
#include <linux/mutex.h>
#include "internal.h"

int erofs_pcs_init_mnt(void);
void erofs_pcs_free_mnt(void);
void erofs_pcs_fill_inode(struct inode *inode);

extern const struct vm_operations_struct generic_file_vm_ops;

#endif
