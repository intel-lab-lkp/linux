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
#include <linux/tee_drv.h>
#include <linux/uuid.h>

#define TZ_TA_MEM_UUID_MTK		"4477588a-8476-11e2-ad15-e41f1390d676"

#define TEE_PARAM_NUM			4

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
	const char			*uuid;
	const int			tee_impl_id;

	int	(*memory_alloc)(struct secure_heap *sec_heap, struct secure_buffer *sec_buf);
	void	(*memory_free)(struct secure_heap *sec_heap, struct secure_buffer *sec_buf);

	/* Protect/unprotect the memory */
	int	(*secure_the_memory)(struct secure_heap *sec_heap, struct secure_buffer *sec_buf);
	void	(*unsecure_the_memory)(struct secure_heap *sec_heap, struct secure_buffer *sec_buf);
};

struct secure_heap {
	const char			*name;
	const enum secure_memory_type	mem_type;

	struct tee_context		*tee_ctx;
	u32				tee_session;

	const struct secure_heap_prv_data *data;
};

static int tee_ctx_match(struct tee_ioctl_version_data *ver, const void *data)
{
	const struct secure_heap_prv_data *d = data;

	return ver->impl_id == d->tee_impl_id;
}

static int secure_heap_tee_session_init(struct secure_heap *sec_heap)
{
	struct tee_param t_param[TEE_PARAM_NUM] = {0};
	struct tee_ioctl_open_session_arg arg = {0};
	const struct secure_heap_prv_data *data = sec_heap->data;
	uuid_t ta_mem_uuid;
	int ret;

	sec_heap->tee_ctx = tee_client_open_context(NULL, tee_ctx_match, data, NULL);
	if (IS_ERR(sec_heap->tee_ctx)) {
		pr_err_once("%s: open context failed, ret=%ld\n", sec_heap->name,
			    PTR_ERR(sec_heap->tee_ctx));
		return -ENODEV;
	}

	arg.num_params = TEE_PARAM_NUM;
	arg.clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	ret = uuid_parse(data->uuid, &ta_mem_uuid);
	if (ret)
		goto close_context;
	memcpy(&arg.uuid, &ta_mem_uuid.b, sizeof(ta_mem_uuid));

	ret = tee_client_open_session(sec_heap->tee_ctx, &arg, t_param);
	if (ret < 0 || arg.ret) {
		pr_err_once("%s: open session failed, ret=%d:%d\n",
			    sec_heap->name, ret, arg.ret);
		ret = -EINVAL;
		goto close_context;
	}
	sec_heap->tee_session = arg.session;
	return 0;

close_context:
	tee_client_close_context(sec_heap->tee_ctx);
	return ret;
}

/* The memory allocating is within the TEE. */
const struct secure_heap_prv_data mtk_sec_mem_data = {
	.uuid			= TZ_TA_MEM_UUID_MTK,
	.tee_impl_id		= TEE_IMPL_ID_OPTEE,
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
	const struct secure_heap_prv_data *data = sec_heap->data;
	struct secure_buffer *sec_buf;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	int ret;

	/*
	 * If uuid is valid, It requires enter TEE to protect buffers. However
	 * TEE probe may be late. Initialize the secure session the first time
	 * we request the secure buffer.
	 */
	if (data->uuid && !sec_heap->tee_session) {
		ret = secure_heap_tee_session_init(sec_heap);
		if (ret)
			return ERR_PTR(ret);
	}

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
		.data		= &mtk_sec_mem_data,
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
