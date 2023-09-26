/* SPDX-License-Identifier: GPL-2.0 */

#ifndef BTRFS_FSCRYPT_H
#define BTRFS_FSCRYPT_H

#include <linux/fscrypt.h>
#include "extent_io.h"

#include "fs.h"

struct btrfs_fscrypt_ctx {
	u8 ctx[BTRFS_MAX_EXTENT_CTX_SIZE];
	size_t size;
};

#ifdef CONFIG_FS_ENCRYPTION
int btrfs_fscrypt_get_disk_name(struct extent_buffer *leaf,
				struct btrfs_dir_item *di,
				struct fscrypt_str *qstr);

bool btrfs_fscrypt_match_name(struct fscrypt_name *fname,
			      struct extent_buffer *leaf,
			      unsigned long de_name, u32 de_name_len);
int btrfs_fscrypt_load_extent_info(struct btrfs_inode *inode,
				   struct extent_map *em,
				   struct btrfs_fscrypt_ctx *ctx);
int btrfs_fscrypt_save_extent_info(struct btrfs_inode *inode,
				   struct btrfs_path *path,
				   struct fscrypt_extent_info *fi);
size_t btrfs_fscrypt_extent_context_size(struct btrfs_inode *inode);

#else
static inline int btrfs_fscrypt_save_extent_info(struct btrfs_inode *inode,
						 struct btrfs_path *path,
						 struct fscrypt_extent_info *fi)
{
	return 0;
}

static inline int btrfs_fscrypt_load_extent_info(struct btrfs_inode *inode,
						 struct extent_map *em,
						 struct btrfs_fscrypt_ctx *ctx)
{
	return 0;
}

static inline int btrfs_fscrypt_get_disk_name(struct extent_buffer *leaf,
					      struct btrfs_dir_item *di,
					      struct fscrypt_str *qstr)
{
	return 0;
}

static inline bool btrfs_fscrypt_match_name(struct fscrypt_name *fname,
					    struct extent_buffer *leaf,
					    unsigned long de_name,
					    u32 de_name_len)
{
	if (de_name_len != fname_len(fname))
		return false;
	return !memcmp_extent_buffer(leaf, fname->disk_name.name, de_name,
				     de_name_len);
}

static inline size_t btrfs_fscrypt_extent_context_size(struct btrfs_inode *inode)
{
	return 0;
}
#endif /* CONFIG_FS_ENCRYPTION */

extern const struct fscrypt_operations btrfs_fscrypt_ops;

#endif /* BTRFS_FSCRYPT_H */
