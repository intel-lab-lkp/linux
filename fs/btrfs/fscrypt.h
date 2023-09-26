/* SPDX-License-Identifier: GPL-2.0 */

#ifndef BTRFS_FSCRYPT_H
#define BTRFS_FSCRYPT_H

#include <linux/fscrypt.h>
#include "extent_io.h"

#include "fs.h"

#ifdef CONFIG_FS_ENCRYPTION
bool btrfs_fscrypt_match_name(struct fscrypt_name *fname,
			      struct extent_buffer *leaf,
			      unsigned long de_name, u32 de_name_len);

#else
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
#endif /* CONFIG_FS_ENCRYPTION */

extern const struct fscrypt_operations btrfs_fscrypt_ops;

#endif /* BTRFS_FSCRYPT_H */
