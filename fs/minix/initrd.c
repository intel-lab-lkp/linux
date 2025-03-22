// SPDX-License-Identifier: GPL-2.0-only

#include <linux/fs.h>
#include <linux/initrd.h>
#include <linux/magic.h>
#include <linux/minix_fs.h>

static size_t __init detect_minixfs(void *block_data)
{
	struct minix_super_block *minixsb =
		(struct minix_super_block *)block_data;
	BUILD_BUG_ON(sizeof(*minixsb) > BLOCK_SIZE);

	if (minixsb->s_magic != MINIX_SUPER_MAGIC &&
	    minixsb->s_magic != MINIX_SUPER_MAGIC2)
		return 0;


	return minixsb->s_nzones
		<< (minixsb->s_log_zone_size + BLOCK_SIZE_BITS);
}

initrd_fs_detect(detect_minixfs, BLOCK_SIZE);
