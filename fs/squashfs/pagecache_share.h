/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2024, Inspur
 */
#ifndef __SQUASHFS_PAGECACHE_SHARE_H
#define __SQUASHFS_PAGECACHE_SHARE_H

#include <linux/mutex.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/rwlock.h>
#include <linux/mutex.h>

int squashfs_pcs_fill_inode(struct inode *inode);
int sqyashfs_pcs_remove(struct inode *inode);
int squashfs_pcs_init_mnt(void);
void squashfs_pcs_mnt_exit(void);

extern const struct file_operations squashfs_pcs_file_fops;
extern const struct vm_operations_struct generic_file_vm_ops;
#endif

