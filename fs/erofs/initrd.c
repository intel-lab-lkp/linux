// SPDX-License-Identifier: GPL-2.0-only

#include <linux/initrd.h>

#include "internal.h"

static size_t __init detect_erofs(void *block_data)
{
	struct erofs_super_block *erofsb = block_data;

	BUILD_BUG_ON(sizeof(*erofsb) > BLOCK_SIZE);

	if (le32_to_cpu(erofsb->magic) != EROFS_SUPER_MAGIC_V1)
		return 0;

	return le32_to_cpu(erofsb->blocks) << erofsb->blkszbits;
}

initrd_fs_detect(detect_erofs, EROFS_SUPER_OFFSET);
