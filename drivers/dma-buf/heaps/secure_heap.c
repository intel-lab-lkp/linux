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
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/tee_drv.h>
#include <linux/uuid.h>

#define TZ_TA_MEM_UUID_MTK		"4477588a-8476-11e2-ad15-e41f1390d676"

#define TEE_PARAM_NUM			4

enum secure_buffer_tee_cmd { /* PARAM NUM always is 4. */
	/*
	 * TZCMD_SECMEM_ZALLOC: Allocate the zeroed secure memory from TEE.
	 *
	 * [in]  value[0].a: The buffer size.
	 *       value[0].b: alignment.
	 * [in]  value[1].a: enum secure_memory_type.
	 * [out] value[3].a: The secure handle.
	 */
	TZCMD_SECMEM_ZALLOC = 0,

	/*
	 * TZCMD_SECMEM_FREE: Free secure memory.
	 *
	 * [in]  value[0].a: The secure handle of this buffer, It's value[3].a of
	 *                   TZCMD_SECMEM_ZALLOC.
	 * [out] value[1].a: return value, 0 means successful, otherwise fail.
	 */
	TZCMD_SECMEM_FREE = 1,
};

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
	/*
	 * The secure handle is a reference to a buffer within the TEE, this is
	 * a value got from TEE.
	 */
	u32				sec_handle;
};

#define TEE_MEM_COMMAND_ID_BASE_MTK	0x10000

struct secure_heap;

struct secure_heap_prv_data {
	const char			*uuid;
	const int			tee_impl_id;
	/*
	 * Different TEEs may implement different commands, and this provides an opportunity
	 * for TEEs to use the same enum secure_buffer_tee_cmd.
	 */
	const int			tee_command_id_base;

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

struct secure_heap_attachment {
	struct sg_table			*table;
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

static int
secure_heap_tee_service_call(struct tee_context *tee_ctx, u32 session,
			     unsigned int command, struct tee_param *params)
{
	struct tee_ioctl_invoke_arg arg = {0};
	int ret;

	arg.num_params = TEE_PARAM_NUM;
	arg.session = session;
	arg.func = command;

	ret = tee_client_invoke_func(tee_ctx, &arg, params);
	if (ret < 0 || arg.ret) {
		pr_err("%s: cmd %d ret %d:%x.\n", __func__, command, ret, arg.ret);
		ret = -EOPNOTSUPP;
	}
	return ret;
}

static int secure_heap_tee_secure_memory(struct secure_heap *sec_heap,
					 struct secure_buffer *sec_buf)
{
	const struct secure_heap_prv_data *data = sec_heap->data;
	struct tee_param params[TEE_PARAM_NUM] = {0};
	int ret;

	params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	params[0].u.value.a = sec_buf->size;
	params[0].u.value.b = PAGE_SIZE;
	params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	params[1].u.value.a = sec_heap->mem_type;
	params[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;

	params[3].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;
	ret = secure_heap_tee_service_call(sec_heap->tee_ctx, sec_heap->tee_session,
					   data->tee_command_id_base + TZCMD_SECMEM_ZALLOC,
					   params);
	if (ret)
		return -ENOMEM;

	sec_buf->sec_handle = params[3].u.value.a;
	return 0;
}

static void secure_heap_tee_unsecure_memory(struct secure_heap *sec_heap,
					    struct secure_buffer *sec_buf)
{
	struct tee_param params[TEE_PARAM_NUM] = {0};

	params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	params[0].u.value.a = sec_buf->sec_handle;
	params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;

	secure_heap_tee_service_call(sec_heap->tee_ctx, sec_heap->tee_session,
				     sec_heap->data->tee_command_id_base + TZCMD_SECMEM_FREE,
				     params);
	if (params[1].u.value.a)
		pr_err("%s, free buffer(0x%x) return fail(%lld) from TEE.\n",
		       sec_heap->name, sec_buf->sec_handle, params[1].u.value.a);
}

/* The memory allocating is within the TEE. */
const struct secure_heap_prv_data mtk_sec_mem_data = {
	.uuid			= TZ_TA_MEM_UUID_MTK,
	.tee_impl_id		= TEE_IMPL_ID_OPTEE,
	.tee_command_id_base	= TEE_MEM_COMMAND_ID_BASE_MTK,
	.secure_the_memory	= secure_heap_tee_secure_memory,
	.unsecure_the_memory	= secure_heap_tee_unsecure_memory,
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

static int secure_heap_attach(struct dma_buf *dmabuf, struct dma_buf_attachment *attachment)
{
	struct secure_buffer *sec_buf = dmabuf->priv;
	struct secure_heap_attachment *a;
	struct sg_table *table;
	int ret;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	table = kzalloc(sizeof(*table), GFP_KERNEL);
	if (!table) {
		ret = -ENOMEM;
		goto err_free_attach;
	}

	ret = sg_alloc_table(table, 1, GFP_KERNEL);
	if (ret)
		goto err_free_sgt;
	sg_set_page(table->sgl, 0, sec_buf->size, 0);

	a->table = table;
	attachment->priv = a;

	return 0;

err_free_sgt:
	kfree(table);
err_free_attach:
	kfree(a);
	return ret;
}

static void secure_heap_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attachment)
{
	struct secure_heap_attachment *a = attachment->priv;

	sg_free_table(a->table);
	kfree(a->table);
	kfree(a);
}

static struct sg_table *
secure_heap_map_dma_buf(struct dma_buf_attachment *attachment, enum dma_data_direction direction)
{
	struct secure_heap_attachment *a = attachment->priv;
	struct dma_buf *dmabuf = attachment->dmabuf;
	struct secure_buffer *sec_buf = dmabuf->priv;
	struct sg_table *table = a->table;

	/*
	 * Technically dma_address refers to the address used by HW, But for secure buffer
	 * we don't know its dma_address in kernel, Instead, we only know its "secure handle".
	 * Thus use this property to save the "secure handle", and the user will use it to
	 * obtain the real address in secure world.
	 *
	 * Note: CONFIG_DMA_API_DEBUG requires it to be aligned with PAGE_SIZE.
	 */
	if (sec_buf->sec_handle) {
		sg_dma_address(table->sgl) = sec_buf->sec_handle;
		sg_dma_len(table->sgl) = sec_buf->size;
	}
	return table;
}

static void
secure_heap_unmap_dma_buf(struct dma_buf_attachment *attachment, struct sg_table *table,
			  enum dma_data_direction direction)
{
	struct secure_heap_attachment *a = attachment->priv;

	WARN_ON(a->table != table);
	sg_dma_address(table->sgl) = 0;
	sg_dma_len(table->sgl) = 0;
}

static int
secure_heap_dma_buf_begin_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction direction)
{
	return -EPERM;
}

static int
secure_heap_dma_buf_end_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction direction)
{
	return -EPERM;
}

static int secure_heap_dma_buf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	return -EPERM;
}

static void secure_heap_free(struct dma_buf *dmabuf)
{
	struct secure_buffer *sec_buf = dmabuf->priv;
	struct secure_heap *sec_heap = dma_heap_get_drvdata(sec_buf->heap);

	secure_heap_secure_memory_free(sec_heap, sec_buf);
	kfree(sec_buf);
}

static const struct dma_buf_ops sec_heap_buf_ops = {
	.attach		= secure_heap_attach,
	.detach		= secure_heap_detach,
	.map_dma_buf	= secure_heap_map_dma_buf,
	.unmap_dma_buf	= secure_heap_unmap_dma_buf,
	.begin_cpu_access = secure_heap_dma_buf_begin_cpu_access,
	.end_cpu_access	= secure_heap_dma_buf_end_cpu_access,
	.mmap		= secure_heap_dma_buf_mmap,
	.release	= secure_heap_free,
};

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
	exp_info.ops = &sec_heap_buf_ops;
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
