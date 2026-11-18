/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Code for supporting dma-buf I/O maps in the NVMe host drivers.
 * Copyright (C) 2026 Pavel Begunkov <asml.silence@gmail.com>
 */

#include <linux/dma-resv.h>

#include "dmabuf.h"

static void nvme_dmabuf_invalidate_mappings(struct dma_buf_attachment *attach)
{
	struct dma_buf_io_ctx *ctx = attach->importer_priv;

	dma_buf_io_invalidate_mappings(ctx);
}

const struct dma_buf_attach_ops nvme_dmabuf_importer_ops = {
	.invalidate_mappings	= nvme_dmabuf_invalidate_mappings,
	.allow_peer2peer	= true,
};
EXPORT_SYMBOL_GPL(nvme_dmabuf_importer_ops);

static struct dma_buf_io_map *nvme_dma_buf_io_map(struct dma_buf_io_ctx *ctx)
{
	unsigned nr_entries = ctx->dmabuf->size / NVME_CTRL_PAGE_SIZE;
	struct nvme_dma_buf_io_ctx *nvme_ctx = ctx->dev_priv;
	struct dma_buf_attachment *attach = nvme_ctx->attach;
	unsigned long tmp, i = 0;
	struct nvme_dmabuf_map *map;
	struct scatterlist *sg;
	struct sg_table *sgt;
	int ret;

	dma_resv_assert_held(ctx->dmabuf->resv);

	if (ctx->dmabuf->size % NVME_CTRL_PAGE_SIZE)
		return ERR_PTR(-EINVAL);

	map = kvmalloc_flex(*map, dma_list, nr_entries);
	if (!map)
		return ERR_PTR(-ENOMEM);
	map->nr_entries = nr_entries;

	sgt = dma_buf_map_attachment(attach, ctx->dir);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		sgt = NULL;
		goto err;
	}

	for_each_sgtable_dma_sg(sgt, sg, tmp) {
		dma_addr_t dma_addr = sg_dma_address(sg);
		unsigned long sg_len = sg_dma_len(sg);

		if ((sg_len % NVME_CTRL_PAGE_SIZE) ||
		    (dma_addr % NVME_CTRL_PAGE_SIZE)) {
			ret = -EINVAL;
			goto err;
		}
		while (sg_len) {
			map->dma_list[i++] = dma_addr;
			dma_addr += NVME_CTRL_PAGE_SIZE;
			sg_len -= NVME_CTRL_PAGE_SIZE;
		}
	}

	ret = dma_buf_io_init_map(ctx, &map->base, sgt);
	if (ret)
		goto err;
	map->sgt = sgt;
	return &map->base;
err:
	if (sgt)
		dma_buf_unmap_attachment(attach, sgt, ctx->dir);
	kvfree(map);
	return ERR_PTR(ret);
}

static void nvme_dma_buf_io_unmap(struct dma_buf_io_ctx *ctx,
		struct dma_buf_io_map *map_base)
{
	struct nvme_dma_buf_io_ctx *nvme_ctx = ctx->dev_priv;
	struct nvme_dmabuf_map *map = to_nvme_dmabuf_map(map_base);

	dma_resv_assert_held(ctx->dmabuf->resv);

	dma_buf_unmap_attachment(nvme_ctx->attach, map->sgt, ctx->dir);
	kvfree(map);
}

static void nvme_dma_buf_io_release(struct dma_buf_io_ctx *base_ctx)
{
	struct nvme_dma_buf_io_ctx *ctx = base_ctx->dev_priv;
	struct nvme_dma_buf_list *buf_list = ctx->buf_list;

	mutex_lock(&buf_list->lock);
	if (!list_empty(&ctx->list)) {
		dma_buf_detach(ctx->base->dmabuf, ctx->attach);
		list_del_init(&ctx->list);
	}
	mutex_unlock(&buf_list->lock);

	nvme_put_ctrl(ctx->ctrl);
	kfree(ctx);
}

void __nvme_remove_dmabufs(struct nvme_dma_buf_list *list)
{
	struct nvme_dma_buf_io_ctx *ctx, *tmp;

	mutex_lock(&list->lock);
	list->dead = true;
	list_for_each_entry_safe(ctx, tmp, &list->ctxs, list) {
		dma_buf_io_detach(ctx->base);
		dma_buf_detach(ctx->base->dmabuf, ctx->attach);
		list_del_init(&ctx->list);
	}
	mutex_unlock(&list->lock);
}
EXPORT_SYMBOL_GPL(__nvme_remove_dmabufs);

/**
 * __nvme_dmabufs_use_owner - check whether any context has the given owner
 * @list:	the controller's dma-buf context list
 * @owner:	the owner to look for
 *
 * Return: true if a dma-buf context in @list was attached for @owner.
 */
bool __nvme_dmabufs_use_owner(struct nvme_dma_buf_list *list, void *owner)
{
	struct nvme_dma_buf_io_ctx *ctx;
	bool found = false;

	mutex_lock(&list->lock);
	list_for_each_entry(ctx, &list->ctxs, list) {
		if (ctx->owner == owner) {
			found = true;
			break;
		}
	}
	mutex_unlock(&list->lock);
	return found;
}
EXPORT_SYMBOL_GPL(__nvme_dmabufs_use_owner);

static const struct dma_buf_io_ops nvme_dma_buf_io_ops = {
	.map		= nvme_dma_buf_io_map,
	.unmap		= nvme_dma_buf_io_unmap,
	.release	= nvme_dma_buf_io_release,
};

/**
 * __nvme_init_dma_buf_io_ctx_locked - initialise a dma-buf I/O context
 * @ctrl:	controller the context issues I/O to
 * @buf_list:	the controller's dma-buf context list
 * @attach:	attachment of the dma-buf, see __nvme_init_dma_buf_io_ctx()
 * @owner:	identifies the device @attach was made for, may be NULL
 * @ctx:	dma-buf I/O context to initialise
 *
 * Locked variant of __nvme_init_dma_buf_io_ctx(), for transports that select
 * the device and create @attach under @buf_list->lock, which the caller must
 * hold. The attachment requirements and ownership rules are the same.
 *
 * Return: 0 on success, or a negative errno code otherwise.
 */
int __nvme_init_dma_buf_io_ctx_locked(struct nvme_ctrl *ctrl,
		struct nvme_dma_buf_list *buf_list,
		struct dma_buf_attachment *attach, void *owner,
		struct dma_buf_io_ctx *ctx)
	__must_hold(&buf_list->lock)
{
	struct nvme_dma_buf_io_ctx *nvme_ctx;
	int ret;

	lockdep_assert_held(&buf_list->lock);

	if (buf_list->dead) {
		ret = -ENODEV;
		goto err_detach;
	}

	nvme_ctx = kzalloc_obj(*nvme_ctx);
	if (!nvme_ctx) {
		ret = -ENOMEM;
		goto err_detach;
	}

	nvme_get_ctrl(ctrl);
	list_add(&nvme_ctx->list, &buf_list->ctxs);
	nvme_ctx->attach = attach;
	nvme_ctx->owner = owner;
	nvme_ctx->base = ctx;
	nvme_ctx->ctrl = ctrl;
	nvme_ctx->buf_list = buf_list;
	ctx->dev_priv = nvme_ctx;
	ctx->dev_ops = &nvme_dma_buf_io_ops;
	return 0;

err_detach:
	dma_buf_detach(ctx->dmabuf, attach);
	return ret;
}
EXPORT_SYMBOL_GPL(__nvme_init_dma_buf_io_ctx_locked);

/*
 * @attach must have been created with nvme_dmabuf_importer_ops and @ctx as
 * the importer private data. The context takes ownership of it, and it is
 * detached on failure or when the context is released.
 */
int __nvme_init_dma_buf_io_ctx(struct nvme_ctrl *ctrl,
		struct nvme_dma_buf_list *buf_list,
		struct dma_buf_attachment *attach,
		struct dma_buf_io_ctx *ctx)
{
	int ret;

	mutex_lock(&buf_list->lock);
	ret = __nvme_init_dma_buf_io_ctx_locked(ctrl, buf_list, attach, NULL,
						ctx);
	mutex_unlock(&buf_list->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(__nvme_init_dma_buf_io_ctx);

MODULE_IMPORT_NS("DMA_BUF");
