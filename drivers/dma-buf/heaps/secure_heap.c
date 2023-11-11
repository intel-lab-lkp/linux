// SPDX-License-Identifier: GPL-2.0
/*
 * DMABUF secure heap exporter
 *
 * Copyright (C) 2023 MediaTek Inc.
 */

#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/slab.h>

enum secure_memory_type {
	/*
	 * MediaTek static chunk memory carved out for TrustZone. The memory
	 * management is inside the TEE.
	 */
	SECURE_MEMORY_TYPE_MTK_CM_TZ	= 1,
};

struct secure_buffer {
	struct dma_heap			*heap;
	size_t				size;
};

struct secure_heap;

struct secure_heap_prv_data {
	int	(*memory_alloc)(struct secure_heap *sec_heap, struct secure_buffer *sec_buf);
	void	(*memory_free)(struct secure_heap *sec_heap, struct secure_buffer *sec_buf);

	/* Protect/unprotect the memory */
	int	(*secure_the_memory)(struct secure_heap *sec_heap, struct secure_buffer *sec_buf);
	void	(*unsecure_the_memory)(struct secure_heap *sec_heap, struct secure_buffer *sec_buf);
};

struct secure_heap {
	const char			*name;
	const enum secure_memory_type	mem_type;

	const struct secure_heap_prv_data *data;
};

static int secure_heap_secure_memory_allocate(struct secure_heap *sec_heap,
					      struct secure_buffer *sec_buf)
{
	const struct secure_heap_prv_data *data = sec_heap->data;
	int ret;

	if (data->memory_alloc) {
		ret = data->memory_alloc(sec_heap, sec_buf);
		if (ret)
			return ret;
	}

	if (data->secure_the_memory) {
		ret = data->secure_the_memory(sec_heap, sec_buf);
		if (ret)
			goto sec_memory_free;
	}
	return 0;

sec_memory_free:
	if (data->memory_free)
		data->memory_free(sec_heap, sec_buf);
	return ret;
}

static void secure_heap_secure_memory_free(struct secure_heap *sec_heap,
					   struct secure_buffer *sec_buf)
{
	const struct secure_heap_prv_data *data = sec_heap->data;

	if (data->unsecure_the_memory)
		data->unsecure_the_memory(sec_heap, sec_buf);

	if (data->memory_free)
		data->memory_free(sec_heap, sec_buf);
}

static struct dma_buf *
secure_heap_allocate(struct dma_heap *heap, unsigned long size,
		     unsigned long fd_flags, unsigned long heap_flags)
{
	struct secure_heap *sec_heap = dma_heap_get_drvdata(heap);
	struct secure_buffer *sec_buf;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	int ret;

	sec_buf = kzalloc(sizeof(*sec_buf), GFP_KERNEL);
	if (!sec_buf)
		return ERR_PTR(-ENOMEM);

	sec_buf->size = ALIGN(size, PAGE_SIZE);
	sec_buf->heap = heap;

	ret = secure_heap_secure_memory_allocate(sec_heap, sec_buf);
	if (ret)
		goto err_free_buf;
	exp_info.exp_name = dma_heap_get_name(heap);
	exp_info.size = sec_buf->size;
	exp_info.flags = fd_flags;
	exp_info.priv = sec_buf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto err_free_sec_mem;
	}

	return dmabuf;

err_free_sec_mem:
	secure_heap_secure_memory_free(sec_heap, sec_buf);
err_free_buf:
	kfree(sec_buf);
	return ERR_PTR(ret);
}

static const struct dma_heap_ops sec_heap_ops = {
	.allocate = secure_heap_allocate,
};

static struct secure_heap secure_heaps[] = {
	{
		.name		= "secure_mtk_cm",
		.mem_type	= SECURE_MEMORY_TYPE_MTK_CM_TZ,
	},
};

static int secure_heap_init(void)
{
	struct secure_heap *sec_heap = secure_heaps;
	struct dma_heap_export_info exp_info;
	struct dma_heap *heap;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(secure_heaps); i++, sec_heap++) {
		exp_info.name = sec_heap->name;
		exp_info.ops = &sec_heap_ops;
		exp_info.priv = (void *)sec_heap;

		heap = dma_heap_add(&exp_info);
		if (IS_ERR(heap))
			return PTR_ERR(heap);
	}
	return 0;
}

module_init(secure_heap_init);
MODULE_DESCRIPTION("Secure Heap Driver");
MODULE_LICENSE("GPL");
