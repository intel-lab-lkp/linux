// SPDX-License-Identifier: GPL-2.0
/*
 * DMABUF mtk_secure_heap exporter
 *
 * Copyright (C) 2023 MediaTek Inc.
 */

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/slab.h>

/*
 * MediaTek secure (chunk) memory type
 *
 * @KREE_MEM_SEC_CM_TZ: static chunk memory carved out for trustzone.
 */
enum kree_mem_type {
	KREE_MEM_SEC_CM_TZ = 1,
};

struct mtk_secure_heap_buffer {
	struct dma_heap		*heap;
	size_t			size;
};

struct mtk_secure_heap {
	const char		*name;
	const enum kree_mem_type mem_type;
};

static struct dma_buf *
mtk_sec_heap_allocate(struct dma_heap *heap, size_t size,
		      unsigned long fd_flags, unsigned long heap_flags)
{
	struct mtk_secure_heap_buffer *sec_buf;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	int ret;

	sec_buf = kzalloc(sizeof(*sec_buf), GFP_KERNEL);
	if (!sec_buf)
		return ERR_PTR(-ENOMEM);

	sec_buf->size = size;
	sec_buf->heap = heap;

	exp_info.exp_name = dma_heap_get_name(heap);
	exp_info.size = sec_buf->size;
	exp_info.flags = fd_flags;
	exp_info.priv = sec_buf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto err_free_buf;
	}

	return dmabuf;

err_free_buf:
	kfree(sec_buf);
	return ERR_PTR(ret);
}

static const struct dma_heap_ops mtk_sec_heap_ops = {
	.allocate	= mtk_sec_heap_allocate,
};

static struct mtk_secure_heap mtk_sec_heap[] = {
	{
		.name		= "mtk_svp",
		.mem_type	= KREE_MEM_SEC_CM_TZ,
	},
};

static int mtk_sec_heap_init(void)
{
	struct mtk_secure_heap *sec_heap = mtk_sec_heap;
	struct dma_heap_export_info exp_info;
	struct dma_heap *heap;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mtk_sec_heap); i++, sec_heap++) {
		exp_info.name = sec_heap->name;
		exp_info.ops = &mtk_sec_heap_ops;
		exp_info.priv = (void *)sec_heap;

		heap = dma_heap_add(&exp_info);
		if (IS_ERR(heap))
			return PTR_ERR(heap);
	}
	return 0;
}

module_init(mtk_sec_heap_init);
MODULE_DESCRIPTION("MediaTek Secure Heap Driver");
MODULE_LICENSE("GPL");
