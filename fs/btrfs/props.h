/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2014 Filipe David Borba Manana <fdmanana@gmail.com>
 */

#ifndef BTRFS_PROPS_H
#define BTRFS_PROPS_H

#include <linux/types.h>
#include <linux/compiler_types.h>

/*
 * Max length of compression algorithm:level string.
 *
 * For now the longest possible string is "zstd:-15", which is
 * 8 characters + 1 terminating null byte.
 * Rounding it up to the closest power of 2 gives 16.
 */
#define BTRFS_COMPRESS_PROP_MAX_LEN 16

struct btrfs_inode;
struct btrfs_path;
struct btrfs_trans_handle;

int __init btrfs_props_init(void);

int btrfs_set_prop(struct btrfs_trans_handle *trans, struct btrfs_inode *inode,
		   const char *name, const char *value, size_t value_len,
		   int flags);
int btrfs_validate_prop(const struct btrfs_inode *inode, const char *name,
			const char *value, size_t value_len);
bool btrfs_ignore_prop(const struct btrfs_inode *inode, const char *name);

int btrfs_load_inode_props(struct btrfs_inode *inode, struct btrfs_path *path);

int btrfs_inode_inherit_props(struct btrfs_trans_handle *trans,
			      struct btrfs_inode *inode,
			      const struct btrfs_inode *dir);

#endif
