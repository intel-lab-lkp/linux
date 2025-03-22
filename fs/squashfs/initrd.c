// SPDX-License-Identifier: GPL-2.0-only

#include <linux/initrd.h>
#include <linux/fs.h>
#include <linux/magic.h>

#include "squashfs_fs.h"

static size_t __init detect_squashfs(void *block_data)
{
	struct squashfs_super_block *squashfsb
		= (struct squashfs_super_block *)block_data;
	BUILD_BUG_ON(sizeof(*squashfsb) > BLOCK_SIZE);

		/* squashfs is at block zero too */
	if (le32_to_cpu(squashfsb->s_magic) != SQUASHFS_MAGIC)
		return 0;


	return le64_to_cpu(squashfsb->bytes_used);
}

initrd_fs_detect(detect_squashfs, 0);
