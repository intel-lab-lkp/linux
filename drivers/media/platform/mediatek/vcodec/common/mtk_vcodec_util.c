// SPDX-License-Identifier: GPL-2.0
/*
* Copyright (c) 2016 MediaTek Inc.
* Author: PC Chen <pc.chen@mediatek.com>
*	Tiffany Lin <tiffany.lin@mediatek.com>
*/

#include <linux/dma-heap.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <uapi/linux/dma-heap.h>

#include "../decoder/mtk_vcodec_dec_drv.h"
#include "../encoder/mtk_vcodec_enc_drv.h"
#include "../decoder/mtk_vcodec_dec_hw.h"

#if defined(CONFIG_DEBUG_FS)
int mtk_vcodec_dbg;
EXPORT_SYMBOL(mtk_vcodec_dbg);

int mtk_v4l2_dbg_level;
EXPORT_SYMBOL(mtk_v4l2_dbg_level);
#endif

void __iomem *mtk_vcodec_get_reg_addr(void __iomem **reg_base, unsigned int reg_idx)
{
	if (reg_idx >= NUM_MAX_VCODEC_REG_BASE) {
		pr_err(MTK_DBG_V4L2_STR "Invalid arguments, reg_idx=%d", reg_idx);
		return NULL;
	}
	return reg_base[reg_idx];
}
EXPORT_SYMBOL(mtk_vcodec_get_reg_addr);

int mtk_vcodec_write_vdecsys(struct mtk_vcodec_dec_ctx *ctx, unsigned int reg,
			     unsigned int val)
{
	struct mtk_vcodec_dec_dev *dev = ctx->dev;

	if (dev->vdecsys_regmap)
		return regmap_write(dev->vdecsys_regmap, reg, val);

	writel(val, dev->reg_base[VDEC_SYS] + reg);

	return 0;
}
EXPORT_SYMBOL(mtk_vcodec_write_vdecsys);

static int mtk_vcodec_mem_alloc_nor(void *priv, struct mtk_vcodec_mem *mem)
{
	enum mtk_instance_type inst_type = *((unsigned int *)priv);
	struct platform_device *plat_dev;
	unsigned long size = mem->size;
	int id;

	if (inst_type == MTK_INST_ENCODER) {
		struct mtk_vcodec_enc_ctx *enc_ctx = priv;

		plat_dev = enc_ctx->dev->plat_dev;
		id = enc_ctx->id;
	} else {
		struct mtk_vcodec_dec_ctx *dec_ctx = priv;

		plat_dev = dec_ctx->dev->plat_dev;
		id = dec_ctx->id;
	}

	mem->va = dma_alloc_coherent(&plat_dev->dev, size, &mem->dma_addr, GFP_KERNEL);
	if (!mem->va) {
		mtk_v4l2_err(plat_dev, "%s dma_alloc size=%ld failed!",
			     dev_name(&plat_dev->dev), size);
		return -ENOMEM;
	}

	mtk_v4l2_debug(plat_dev, 3, "[%d] - va = %p dma = 0x%lx size = 0x%lx", id, mem->va,
		       (unsigned long)mem->dma_addr, size);

	return 0;
}

static int mtk_vcodec_mem_alloc_sec(struct mtk_vcodec_dec_ctx *ctx, struct mtk_vcodec_mem *mem)
{
	struct device *dev = &ctx->dev->plat_dev->dev;
	struct dma_buf *dma_buffer;
	struct dma_heap *vdec_heap;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	unsigned long size = mem->size;
	int ret = 0;

	if (!size)
		return -EINVAL;

	vdec_heap = dma_heap_find("mtk_svp");
	if (!vdec_heap) {
		mtk_v4l2_vdec_err(ctx, "dma heap find failed!");
		return -EPERM;
	}

	dma_buffer = dma_heap_buffer_alloc(vdec_heap, size, DMA_HEAP_VALID_FD_FLAGS,
					   DMA_HEAP_VALID_HEAP_FLAGS);
	if (IS_ERR_OR_NULL(dma_buffer)) {
		mtk_v4l2_vdec_err(ctx, "dma heap alloc size=0x%lx failed!", size);
		return PTR_ERR(dma_buffer);
	}

	attach = dma_buf_attach(dma_buffer, dev);
	if (IS_ERR_OR_NULL(attach)) {
		mtk_v4l2_vdec_err(ctx, "dma attach size=0x%lx failed!", size);
		ret = PTR_ERR(attach);
		goto err_attach;
	}

	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR_OR_NULL(sgt)) {
		mtk_v4l2_vdec_err(ctx, "dma map attach size=0x%lx failed!", size);
		ret = PTR_ERR(sgt);
		goto err_sgt;
	}

	mem->va = dma_buffer;
	mem->dma_addr = (dma_addr_t)sg_dma_address((sgt)->sgl);

	if (!mem->va || !mem->dma_addr) {
		mtk_v4l2_vdec_err(ctx, "dma buffer size=0x%lx failed!", size);
		ret = -EPERM;
		goto err_addr;
	}

	mem->attach = attach;
	mem->sgt = sgt;

	return 0;
err_addr:
	dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
err_sgt:
	dma_buf_detach(dma_buffer, attach);
err_attach:
	dma_buf_put(dma_buffer);

	return ret;
}

static void mtk_vcodec_mem_free_nor(void *priv, struct mtk_vcodec_mem *mem)
{
	enum mtk_instance_type inst_type = *((unsigned int *)priv);
	struct platform_device *plat_dev;
	unsigned long size = mem->size;
	int id;

	if (inst_type == MTK_INST_ENCODER) {
		struct mtk_vcodec_enc_ctx *enc_ctx = priv;

		plat_dev = enc_ctx->dev->plat_dev;
		id = enc_ctx->id;
	} else {
		struct mtk_vcodec_dec_ctx *dec_ctx = priv;

		plat_dev = dec_ctx->dev->plat_dev;
		id = dec_ctx->id;
	}

	if (!mem->va) {
		mtk_v4l2_err(plat_dev, "%s dma_free size=%ld failed!",
			     dev_name(&plat_dev->dev), size);
		return;
	}

	mtk_v4l2_debug(plat_dev, 3, "[%d] - va = %p dma = 0x%lx size = 0x%lx", id, mem->va,
		       (unsigned long)mem->dma_addr, size);

	dma_free_coherent(&plat_dev->dev, size, mem->va, mem->dma_addr);
	mem->va = NULL;
	mem->dma_addr = 0;
	mem->size = 0;
}

static void mtk_vcodec_mem_free_sec(struct mtk_vcodec_mem *mem)
{
	dma_buf_unmap_attachment(mem->attach, mem->sgt, DMA_BIDIRECTIONAL);
	dma_buf_detach((struct dma_buf *)mem->va, mem->attach);
	dma_buf_put((struct dma_buf *)mem->va);

	mem->attach = NULL;
	mem->sgt = NULL;
	mem->va = NULL;
	mem->dma_addr = 0;
	mem->size = 0;
}

int mtk_vcodec_mem_alloc(void *priv, struct mtk_vcodec_mem *mem)
{
	enum mtk_instance_type inst_type = *((unsigned int *)priv);
	int ret;

	if (inst_type == MTK_INST_DECODER) {
		struct mtk_vcodec_dec_ctx *dec_ctx = priv;

		if (dec_ctx->is_secure_playback) {
			ret = mtk_vcodec_mem_alloc_sec(dec_ctx, mem);
			goto alloc_end;
		}
	}

	ret = mtk_vcodec_mem_alloc_nor(priv, mem);
alloc_end:

	return ret;
}
EXPORT_SYMBOL(mtk_vcodec_mem_alloc);

void mtk_vcodec_mem_free(void *priv, struct mtk_vcodec_mem *mem)
{
	enum mtk_instance_type inst_type = *((unsigned int *)priv);

	if (inst_type == MTK_INST_DECODER) {
		struct mtk_vcodec_dec_ctx *dec_ctx = priv;

		if (dec_ctx->is_secure_playback) {
			mtk_vcodec_mem_free_sec(mem);
			return;
		}
	}

	mtk_vcodec_mem_free_nor(priv, mem);
}
EXPORT_SYMBOL(mtk_vcodec_mem_free);

void *mtk_vcodec_get_hw_dev(struct mtk_vcodec_dec_dev *dev, int hw_idx)
{
	if (hw_idx >= MTK_VDEC_HW_MAX || hw_idx < 0 || !dev->subdev_dev[hw_idx]) {
		dev_err(&dev->plat_dev->dev, "hw idx is out of range:%d", hw_idx);
		return NULL;
	}

	return dev->subdev_dev[hw_idx];
}
EXPORT_SYMBOL(mtk_vcodec_get_hw_dev);

void mtk_vcodec_set_curr_ctx(struct mtk_vcodec_dec_dev *vdec_dev,
			     struct mtk_vcodec_dec_ctx *ctx, int hw_idx)
{
	unsigned long flags;
	struct mtk_vdec_hw_dev *subdev_dev;

	spin_lock_irqsave(&vdec_dev->irqlock, flags);
	if (vdec_dev->vdec_pdata->is_subdev_supported) {
		subdev_dev = mtk_vcodec_get_hw_dev(vdec_dev, hw_idx);
		if (!subdev_dev) {
			dev_err(&vdec_dev->plat_dev->dev, "Failed to get hw dev");
			spin_unlock_irqrestore(&vdec_dev->irqlock, flags);
			return;
		}
		subdev_dev->curr_ctx = ctx;
	} else {
		vdec_dev->curr_ctx = ctx;
	}
	spin_unlock_irqrestore(&vdec_dev->irqlock, flags);
}
EXPORT_SYMBOL(mtk_vcodec_set_curr_ctx);

struct mtk_vcodec_dec_ctx *mtk_vcodec_get_curr_ctx(struct mtk_vcodec_dec_dev *vdec_dev,
						   unsigned int hw_idx)
{
	unsigned long flags;
	struct mtk_vcodec_dec_ctx *ctx;
	struct mtk_vdec_hw_dev *subdev_dev;

	spin_lock_irqsave(&vdec_dev->irqlock, flags);
	if (vdec_dev->vdec_pdata->is_subdev_supported) {
		subdev_dev = mtk_vcodec_get_hw_dev(vdec_dev, hw_idx);
		if (!subdev_dev) {
			dev_err(&vdec_dev->plat_dev->dev, "Failed to get hw dev");
			spin_unlock_irqrestore(&vdec_dev->irqlock, flags);
			return NULL;
		}
		ctx = subdev_dev->curr_ctx;
	} else {
		ctx = vdec_dev->curr_ctx;
	}
	spin_unlock_irqrestore(&vdec_dev->irqlock, flags);
	return ctx;
}
EXPORT_SYMBOL(mtk_vcodec_get_curr_ctx);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Mediatek video codec driver");
MODULE_IMPORT_NS(DMA_BUF);
