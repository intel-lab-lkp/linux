// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/initrd.h>
#include <linux/magic.h>
#include <linux/romfs_fs.h>

static size_t __init detect_romfs(void *block_data)
{
	struct romfs_super_block *romfsb
		= (struct romfs_super_block *)block_data;
	BUILD_BUG_ON(sizeof(*romfsb) > BLOCK_SIZE);

	/* The definitions of ROMSB_WORD* already handle endianness. */
	if (romfsb->word0 != ROMSB_WORD0 ||
	    romfsb->word1 != ROMSB_WORD1)
		return 0;

	return be32_to_cpu(romfsb->size);
}

initrd_fs_detect(detect_romfs, 0);
