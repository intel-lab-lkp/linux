// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/initrd.h>
#include <uapi/linux/cramfs_fs.h>

/*
 * The filesystem start maybe padded by this many bytes to make space
 * for boot loaders.
 */
#define CRAMFS_PAD_OFFSET 512

static size_t __init check_cramfs_sb(struct cramfs_super *cramfsb)
{
	if (cramfsb->magic != CRAMFS_MAGIC)
		return 0;

	return cramfsb->size;
}

static size_t __init detect_cramfs(void *block_data)
{
	size_t fssize;

	BUILD_BUG_ON(sizeof(struct cramfs_super) + CRAMFS_PAD_OFFSET
		     > BLOCK_SIZE);

	fssize = check_cramfs_sb((struct cramfs_super *)block_data);
	if (fssize)
		return fssize;

	/*
	 * The header padding doesn't influence the total length of
	 * the filesystem.
	 */
	block_data = (char *)block_data + CRAMFS_PAD_OFFSET;
	fssize = check_cramfs_sb((struct cramfs_super *)block_data);
	return fssize;
}

initrd_fs_detect(detect_cramfs, 0);
