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
#include <linux/tee_drv.h>
#include <linux/uuid.h>

#define TZ_TA_MEM_UUID		"4477588a-8476-11e2-ad15-e41f1390d676"

#define MTK_TEE_PARAM_NUM		4

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
	u32			 mem_session;
	struct tee_context	*tee_ctx;
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
