// SPDX-License-Identifier: GPL-2.0

#include <linux/dma-fence.h>

void rust_helper_dma_fence_get(struct dma_fence *f)
{
	dma_fence_get(f);
}

void rust_helper_dma_fence_put(struct dma_fence *f)
{
	dma_fence_put(f);
}

bool rust_helper_dma_fence_begin_signalling(void)
{
	return dma_fence_begin_signalling();
}

void rust_helper_dma_fence_end_signalling(bool cookie)
{
	dma_fence_end_signalling(cookie);
}

bool rust_helper_dma_fence_is_signaled(struct dma_fence *f)
{
	return dma_fence_is_signaled(f);
}
