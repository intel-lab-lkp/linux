// SPDX-License-Identifier: GPL-2.0-only

#include <linux/initrd.h>
#include <linux/fs.h>

#include "ext2.h"

static size_t __init detect_ext2fs(void *block_data)
{
	struct ext2_super_block *ext2sb
		= (struct ext2_super_block *)block_data;
	BUILD_BUG_ON(sizeof(*ext2sb) > BLOCK_SIZE);

	/*
	 * The 16-bit magic number is not a lot to reliably detect the
	 * filesystem. We check the revision as well to decrease the
	 * chance of false positives.
	 */
	if (le16_to_cpu(ext2sb->s_magic) != EXT2_SUPER_MAGIC ||
	    le32_to_cpu(ext2sb->s_rev_level) > EXT2_MAX_SUPP_REV)
		return 0;

	return le32_to_cpu(ext2sb->s_blocks_count)
		<< (le32_to_cpu(ext2sb->s_log_block_size) + BLOCK_SIZE_BITS);
}

initrd_fs_detect(detect_ext2fs, BLOCK_SIZE);
