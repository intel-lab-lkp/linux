// SPDX-License-Identifier: GPL-2.0

#include <linux/gpu_buddy.h>

#ifdef CONFIG_GPU_BUDDY

u64 rust_helper_gpu_buddy_block_offset(const struct gpu_buddy_block *block)
{
	return gpu_buddy_block_offset(block);
}

unsigned int rust_helper_gpu_buddy_block_order(struct gpu_buddy_block *block)
{
	return gpu_buddy_block_order(block);
}

u64 rust_helper_gpu_buddy_block_size(struct gpu_buddy *mm,
				     struct gpu_buddy_block *block)
{
	return gpu_buddy_block_size(mm, block);
}

#endif /* CONFIG_GPU_BUDDY */
