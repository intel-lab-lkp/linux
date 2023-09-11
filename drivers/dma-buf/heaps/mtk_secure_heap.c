// SPDX-License-Identifier: GPL-2.0
/*
 * DMABUF mtk_secure_heap exporter
 *
 * Copyright (C) 2023 MediaTek Inc.
 */
#include <linux/cma.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/tee_drv.h>
#include <linux/uuid.h>

#define TZ_TA_MEM_UUID		"4477588a-8476-11e2-ad15-e41f1390d676"

#define MTK_TEE_PARAM_NUM		4

#define TZCMD_MEM_SECURECM_UNREF	7
#define TZCMD_MEM_SECURECM_ZALLOC	15

/*
 * MediaTek secure (chunk) memory type
 *
 * @KREE_MEM_SEC_CM_TZ: static chunk memory carved out for trustzone.
 * @KREE_MEM_SEC_CM_CMA: dynamic chunk memory carved out from CMA.
 */
enum kree_mem_type {
	KREE_MEM_SEC_CM_TZ = 1,
	KREE_MEM_SEC_CM_CMA,
};

struct mtk_secure_heap_buffer {
	struct dma_heap		*heap;
	size_t			size;

	u32			sec_handle;
};

struct mtk_secure_heap {
	const char		*name;
	const enum kree_mem_type mem_type;
	u32			 mem_session;
	struct tee_context	*tee_ctx;

	struct cma		*cma;
	struct page		*cma_page;
	unsigned long		cma_paddr;
	unsigned long		cma_size;
	unsigned long		cma_used_size;
	struct mutex		lock; /* lock for cma_used_size */
};

struct mtk_secure_heap_attachment {
	struct sg_table		*table;
};

static int mtk_optee_ctx_match(struct tee_ioctl_version_data *ver, const void *data)
{
	return ver->impl_id == TEE_IMPL_ID_OPTEE;
}

static int mtk_kree_secure_session_init(struct mtk_secure_heap *sec_heap)
{
	struct tee_param t_param[MTK_TEE_PARAM_NUM] = {0};
	struct tee_ioctl_open_session_arg arg = {0};
	uuid_t ta_mem_uuid;
	int ret;

	sec_heap->tee_ctx = tee_client_open_context(NULL, mtk_optee_ctx_match,
						    NULL, NULL);
	if (IS_ERR(sec_heap->tee_ctx)) {
		pr_err("%s: open context failed, ret=%ld\n", sec_heap->name,
		       PTR_ERR(sec_heap->tee_ctx));
		return -ENODEV;
	}

	arg.num_params = MTK_TEE_PARAM_NUM;
	arg.clnt_login = TEE_IOCTL_LOGIN_PUBLIC;
	ret = uuid_parse(TZ_TA_MEM_UUID, &ta_mem_uuid);
	if (ret)
		goto close_context;
	memcpy(&arg.uuid, &ta_mem_uuid.b, sizeof(ta_mem_uuid));

	ret = tee_client_open_session(sec_heap->tee_ctx, &arg, t_param);
	if (ret < 0 || arg.ret) {
		pr_err("%s: open session failed, ret=%d:%d\n",
		       sec_heap->name, ret, arg.ret);
		ret = -EINVAL;
		goto close_context;
	}
	sec_heap->mem_session = arg.session;
	return 0;

close_context:
	tee_client_close_context(sec_heap->tee_ctx);
	return ret;
}

static int mtk_sec_mem_cma_allocate(struct mtk_secure_heap *sec_heap, size_t size)
{
	/*
	 * Allocate CMA only when allocating buffer for the first time, and just
	 * increase cma_used_size at the other times.
	 */
	mutex_lock(&sec_heap->lock);
	if (sec_heap->cma_used_size)
		goto add_size;

	mutex_unlock(&sec_heap->lock);
	sec_heap->cma_page = cma_alloc(sec_heap->cma, sec_heap->cma_size >> PAGE_SHIFT,
				       get_order(PAGE_SIZE), false);
	if (!sec_heap->cma_page)
		return -ENOMEM;

	mutex_lock(&sec_heap->lock);
add_size:
	sec_heap->cma_used_size += size;
	mutex_unlock(&sec_heap->lock);
	return sec_heap->cma_used_size;
}

static void mtk_sec_mem_cma_free(struct mtk_secure_heap *sec_heap, size_t size)
{
	bool cma_is_empty;

	mutex_lock(&sec_heap->lock);
	sec_heap->cma_used_size -= size;
	cma_is_empty = !sec_heap->cma_used_size;
	mutex_unlock(&sec_heap->lock);

	if (cma_is_empty)
		cma_release(sec_heap->cma, sec_heap->cma_page, sec_heap->cma_size >> PAGE_SHIFT);
}

static int
mtk_sec_mem_tee_service_call(struct tee_context *tee_ctx, u32 session,
			     unsigned int command, struct tee_param *params)
{
	struct tee_ioctl_invoke_arg arg = {0};
	int ret;

	arg.num_params = MTK_TEE_PARAM_NUM;
	arg.session = session;
	arg.func = command;

	ret = tee_client_invoke_func(tee_ctx, &arg, params);
	if (ret < 0 || arg.ret) {
		pr_err("%s: cmd %d ret %d:%x.\n", __func__, command, ret, arg.ret);
		ret = -EOPNOTSUPP;
	}
	return ret;
}

static int mtk_sec_mem_allocate(struct mtk_secure_heap *sec_heap,
				struct mtk_secure_heap_buffer *sec_buf)
{
	struct tee_param params[MTK_TEE_PARAM_NUM] = {0};
	u32 mem_session = sec_heap->mem_session;
	bool cma_frst_alloc = false;
	int ret;

	if (sec_heap->cma) {
		ret = mtk_sec_mem_cma_allocate(sec_heap, sec_buf->size);
		if (ret < 0)
			return ret;
		/*
		 * When CMA allocates for the first time, pass the CMA range to TEE
		 * to protect it. It's the first allocating if the cma_used_size is equal
		 * to this required buffer size.
		 */
		cma_frst_alloc = (ret == sec_buf->size);
	}

	params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	params[0].u.value.a = SZ_4K;			/* alignment */
	params[0].u.value.b = sec_heap->mem_type;	/* memory type */
	params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	params[1].u.value.a = sec_buf->size;
	params[2].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INOUT;
	if (sec_heap->cma && cma_frst_alloc) {
		params[2].u.value.a = sec_heap->cma_paddr;
		params[2].u.value.b = sec_heap->cma_size;
	}

	/* Always request zeroed buffer */
	ret = mtk_sec_mem_tee_service_call(sec_heap->tee_ctx, mem_session,
					   TZCMD_MEM_SECURECM_ZALLOC, params);
	if (ret) {
		ret = -ENOMEM;
		goto free_cma;
	}

	sec_buf->sec_handle = params[2].u.value.a;
	return 0;

free_cma:
	if (sec_heap->cma)
		mtk_sec_mem_cma_free(sec_heap, sec_buf->size);
	return ret;
}

static void mtk_sec_mem_release(struct mtk_secure_heap *sec_heap,
				struct mtk_secure_heap_buffer *sec_buf)
{
	struct tee_param params[MTK_TEE_PARAM_NUM] = {0};
	u32 mem_session = sec_heap->mem_session;

	params[0].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_INPUT;
	params[0].u.value.a = sec_buf->sec_handle;
	params[1].attr = TEE_IOCTL_PARAM_ATTR_TYPE_VALUE_OUTPUT;

	mtk_sec_mem_tee_service_call(sec_heap->tee_ctx, mem_session,
				     TZCMD_MEM_SECURECM_UNREF, params);

	if (sec_heap->cma)
		mtk_sec_mem_cma_free(sec_heap, sec_buf->size);
}

static int mtk_sec_heap_attach(struct dma_buf *dmabuf, struct dma_buf_attachment *attachment)
{
	struct mtk_secure_heap_buffer *sec_buf = dmabuf->priv;
	struct mtk_secure_heap_attachment *a;
	struct sg_table *table;
	int ret = 0;

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

static void mtk_sec_heap_detach(struct dma_buf *dmabuf, struct dma_buf_attachment *attachment)
{
	struct mtk_secure_heap_attachment *a = attachment->priv;

	sg_free_table(a->table);
	kfree(a->table);
	kfree(a);
}

static struct sg_table *
mtk_sec_heap_map_dma_buf(struct dma_buf_attachment *attachment, enum dma_data_direction direction)
{
	struct mtk_secure_heap_attachment *a = attachment->priv;
	struct dma_buf *dmabuf = attachment->dmabuf;
	struct mtk_secure_heap_buffer *sec_buf = dmabuf->priv;
	struct sg_table *table = a->table;

	/*
	 * Technically dma_address refers to the address used by HW, But for secure buffer
	 * we don't know its dma_address in kernel, Instead, we only know its "secure handle".
	 * Thus use this property to save the "secure handle", and the user will use it to
	 * obtain the real address in secure world.
	 */
	sg_dma_address(table->sgl) = sec_buf->sec_handle;
	sg_dma_len(table->sgl) = sec_buf->size;

	return table;
}

static void
mtk_sec_heap_unmap_dma_buf(struct dma_buf_attachment *attachment, struct sg_table *table,
			   enum dma_data_direction direction)
{
	struct mtk_secure_heap_attachment *a = attachment->priv;

	WARN_ON(a->table != table);
	sg_dma_address(table->sgl) = 0;
}

static int
mtk_sec_heap_dma_buf_begin_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction direction)
{
	return -EPERM;
}

static int
mtk_sec_heap_dma_buf_end_cpu_access(struct dma_buf *dmabuf, enum dma_data_direction direction)
{
	return -EPERM;
}

static int mtk_sec_heap_dma_buf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	return -EPERM;
}

static void mtk_sec_heap_free(struct dma_buf *dmabuf)
{
	struct mtk_secure_heap_buffer *sec_buf = dmabuf->priv;
	struct mtk_secure_heap *sec_heap = dma_heap_get_drvdata(sec_buf->heap);

	mtk_sec_mem_release(sec_heap, sec_buf);
	kfree(sec_buf);
}

static const struct dma_buf_ops mtk_sec_heap_buf_ops = {
	.attach		= mtk_sec_heap_attach,
	.detach		= mtk_sec_heap_detach,
	.map_dma_buf	= mtk_sec_heap_map_dma_buf,
	.unmap_dma_buf	= mtk_sec_heap_unmap_dma_buf,
	.begin_cpu_access = mtk_sec_heap_dma_buf_begin_cpu_access,
	.end_cpu_access	= mtk_sec_heap_dma_buf_end_cpu_access,
	.mmap		= mtk_sec_heap_dma_buf_mmap,
	.release	= mtk_sec_heap_free,
};

static struct dma_buf *
mtk_sec_heap_allocate(struct dma_heap *heap, size_t size,
		      unsigned long fd_flags, unsigned long heap_flags)
{
	struct mtk_secure_heap *sec_heap = dma_heap_get_drvdata(heap);
	struct mtk_secure_heap_buffer *sec_buf;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	int ret;

	/*
	 * TEE probe may be late. Initialise the secure session in the first
	 * allocating secure buffer.
	 */
	if (!sec_heap->mem_session) {
		ret = mtk_kree_secure_session_init(sec_heap);
		if (ret)
			return ERR_PTR(ret);
	}

	sec_buf = kzalloc(sizeof(*sec_buf), GFP_KERNEL);
	if (!sec_buf)
		return ERR_PTR(-ENOMEM);

	sec_buf->size = size;
	sec_buf->heap = heap;

	ret = mtk_sec_mem_allocate(sec_heap, sec_buf);
	if (ret)
		goto err_free_buf;
	exp_info.exp_name = dma_heap_get_name(heap);
	exp_info.ops = &mtk_sec_heap_buf_ops;
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
	mtk_sec_mem_release(sec_heap, sec_buf);
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
	{
		.name		= "mtk_svp_cma",
		.mem_type	= KREE_MEM_SEC_CM_CMA,
	},
};

static int __init mtk_secure_cma_init(struct reserved_mem *rmem)
{
	struct mtk_secure_heap *sec_heap = NULL;
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(mtk_sec_heap); i++) {
		if (mtk_sec_heap[i].mem_type != KREE_MEM_SEC_CM_CMA)
			continue;
		sec_heap = &mtk_sec_heap[i];
		break;
	}
	if (!sec_heap)
		return -ENOENT;

	ret = cma_init_reserved_mem(rmem->base, rmem->size, 0, sec_heap->name,
				    &sec_heap->cma);
	if (ret) {
		pr_err("%s: %s set up CMA fail\n", __func__, rmem->name);
		return ret;
	}
	sec_heap->cma_paddr = rmem->base;
	sec_heap->cma_size = rmem->size;

	return 0;
}

RESERVEDMEM_OF_DECLARE(mtk_secure_cma, "mediatek,secure_cma_chunkmem",
		       mtk_secure_cma_init);

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

		if (sec_heap->mem_type == KREE_MEM_SEC_CM_CMA) {
			if (!sec_heap->cma) {
				pr_err("CMA is not ready for %s.\n", sec_heap->name);
				continue;
			} else {
				mutex_init(&sec_heap->lock);
			}
		}

		heap = dma_heap_add(&exp_info);
		if (IS_ERR(heap))
			return PTR_ERR(heap);
	}
	return 0;
}

module_init(mtk_sec_heap_init);
MODULE_DESCRIPTION("MediaTek Secure Heap Driver");
MODULE_LICENSE("GPL");
