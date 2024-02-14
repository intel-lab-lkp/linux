// SPDX-License-Identifier: GPL-2.0
/*
 * ARM Mali-C55 ISP Driver - Core driver code
 *
 * Copyright (C) 2023 Ideas on Board Oy
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/moduleparam.h>
#include <linux/mod_devicetable.h>
#include <linux/of.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/scatterlist.h>
#include <linux/string.h>

#include <media/media-entity.h>
#include <media/v4l2-device.h>
#include <media/videobuf2-dma-contig.h>

#include "mali-c55-common.h"
#include "mali-c55-registers.h"

static bool allow_config_dma = true;
module_param(allow_config_dma, bool, 0644);

static const char * const mali_c55_interrupt_names[] = {
	[MALI_C55_IRQ_ISP_START] = "ISP start",
	[MALI_C55_IRQ_ISP_DONE] = "ISP done",
	[MALI_C55_IRQ_MCM_ERROR] = "Multi-context management error",
	[MALI_C55_IRQ_BROKEN_FRAME_ERROR] = "Broken frame error",
	[MALI_C55_IRQ_MET_AF_DONE] = "AF metering done",
	[MALI_C55_IRQ_MET_AEXP_DONE] = "AEXP metering done",
	[MALI_C55_IRQ_MET_AWB_DONE] = "AWB metering done",
	[MALI_C55_IRQ_AEXP_1024_DONE] = "AEXP 1024-bit histogram done",
	[MALI_C55_IRQ_IRIDIX_MET_DONE] = "Iridix metering done",
	[MALI_C55_IRQ_LUT_INIT_DONE] = "LUT memory init done",
	[MALI_C55_IRQ_FR_Y_DONE] = "Full resolution Y plane DMA done",
	[MALI_C55_IRQ_FR_UV_DONE] = "Full resolution U/V plane DMA done",
	[MALI_C55_IRQ_DS_Y_DONE] = "Downscale Y plane DMA done",
	[MALI_C55_IRQ_DS_UV_DONE] = "Downscale U/V plane DMA done",
	[MALI_C55_IRQ_LINEARIZATION_DONE] = "Linearisation done",
	[MALI_C55_IRQ_RAW_FRONTEND_DONE] = "Raw frontend processing done",
	[MALI_C55_IRQ_NOISE_REDUCTION_DONE] = "Noise reduction done",
	[MALI_C55_IRQ_IRIDIX_DONE] = "Iridix done",
	[MALI_C55_IRQ_BAYER2RGB_DONE] = "Bayer to RGB conversion done",
	[MALI_C55_IRQ_WATCHDOG_TIMER] = "Watchdog timer timed out",
	[MALI_C55_IRQ_FRAME_COLLISION] = "Frame collision error",
	[MALI_C55_IRQ_UNUSED] = "IRQ bit unused",
	[MALI_C55_IRQ_DMA_ERROR] = "DMA error",
	[MALI_C55_IRQ_INPUT_STOPPED] = "Input port safely stopped",
	[MALI_C55_IRQ_MET_AWB_TARGET1_HIT] = "AWB metering target 1 address hit",
	[MALI_C55_IRQ_MET_AWB_TARGET2_HIT] = "AWB metering target 2 address hit"
};

static unsigned int config_space_addrs[] = {
	[MALI_C55_CONFIG_PING] = 0x0AB6C,
	[MALI_C55_CONFIG_PONG] = 0x22B2C,
};

/* System IO
 *
 * The Mali-C55 ISP has up to two configuration register spaces (called 'ping'
 * and 'pong'), with the  expectation that the 'active' space will be left
 * untouched whilst a frame is being processed and the 'inactive' space
 * configured ready to be passed during the blanking period before the next
 * frame processing starts. These spaces should ideally be set via DMA transfer
 * from a buffer rather than through individual register set operations. There
 * is also a shared global register space which should be set normally. Of
 * course, the ISP might be included in a system which lacks a suitable DMA
 * engine, and the second configuration space might not be fitted at all, which
 * means we need to support four scenarios:
 *
 * 1. Multi config space, with DMA engine.
 * 2. Multi config space, no DMA engine.
 * 3. Single config space, with DMA engine.
 * 4. Single config space, no DMA engine.
 *
 * The first case is very easy, but the rest present annoying problems. The best
 * way to solve them seems to be simply to replicate the concept of DMAing over
 * the configuration buffer even if there's no DMA engine on the board, for
 * which we rely on memcpy. To facilitate this any read/write call that is made
 * to an address within those config spaces should infact be directed to a
 * buffer that was allocated to hold them rather than the IO memory itself. The
 * actual copy of that buffer to IO mem will happen on interrupt.
 */

void mali_c55_write(struct mali_c55 *mali_c55, unsigned int addr, u32 val)
{
	struct mali_c55_ctx *ctx = mali_c55_get_active_context(mali_c55);

	if (addr >= MALI_C55_REG_CONFIG_SPACES_OFFSET) {
		spin_lock(&ctx->lock);
		addr = (addr - MALI_C55_REG_CONFIG_SPACES_OFFSET) / 4;
		((u32 *)ctx->registers)[addr] = val;
		spin_unlock(&ctx->lock);

		return;
	}

	writel(val, mali_c55->base + addr);
}

u32 mali_c55_read(struct mali_c55 *mali_c55, unsigned int addr,
		  bool force_hardware)
{
	struct mali_c55_ctx *ctx = mali_c55_get_active_context(mali_c55);
	u32 val;

	if (addr >= MALI_C55_REG_CONFIG_SPACES_OFFSET && !force_hardware) {
		spin_lock(&ctx->lock);
		addr = (addr - MALI_C55_REG_CONFIG_SPACES_OFFSET) / 4;
		val = ((u32 *)ctx->registers)[addr];
		spin_unlock(&ctx->lock);

		return val;
	}

	return readl(mali_c55->base + addr);
}

void mali_c55_update_bits(struct mali_c55 *mali_c55, unsigned int addr,
			  u32 mask, u32 val)
{
	u32 orig, tmp;

	orig = mali_c55_read(mali_c55, addr, false);

	tmp = orig & ~mask;
	tmp |= val & mask;

	if (tmp != orig)
		mali_c55_write(mali_c55, addr, tmp);
}

static int mali_c55_dma_xfer(struct mali_c55_ctx *ctx, dma_addr_t src,
			     dma_addr_t dst, enum dma_data_direction dir)
{
	struct mali_c55 *mali_c55 = ctx->mali_c55;
	struct dma_async_tx_descriptor *tx;
	enum dma_status status;
	dma_cookie_t cookie;

	tx = dmaengine_prep_dma_memcpy(mali_c55->channel, dst, src,
				       MALI_C55_CONFIG_SPACE_SIZE, 0);
	if (!tx) {
		dev_err(mali_c55->dev, "failed to prep DMA memcpy\n");
		return -EIO;
	}

	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie)) {
		dev_err(mali_c55->dev, "error submitting dma transfer\n");
		return -EIO;
	}

	status = dma_sync_wait(mali_c55->channel, cookie);
	if (status != DMA_COMPLETE) {
		dev_err(mali_c55->dev, "dma transfer failed\n");
		return -EIO;
	}

	return 0;
}

static int mali_c55_dma_read(struct mali_c55_ctx *ctx,
			     enum mali_c55_config_spaces cfg_space)
{
	struct mali_c55 *mali_c55 = ctx->mali_c55;
	struct device *dma_dev = mali_c55->channel->device->dev;
	dma_addr_t src = ctx->base + config_space_addrs[cfg_space];
	dma_addr_t dst;
	int ret;

	spin_lock(&ctx->lock);

	dst = dma_map_single(dma_dev, ctx->registers,
			     MALI_C55_CONFIG_SPACE_SIZE, DMA_FROM_DEVICE);
	if (dma_mapping_error(dma_dev, dst)) {
		dev_err(mali_c55->dev, "failed to map DMA addr\n");
		ret = -EIO;
		goto out_unlock;
	}

	ret = mali_c55_dma_xfer(ctx, src, dst, DMA_FROM_DEVICE);
	dma_unmap_single(dma_dev, dst, MALI_C55_CONFIG_SPACE_SIZE,
			 DMA_FROM_DEVICE);

out_unlock:
	spin_unlock(&ctx->lock);
	return ret;
}

static int mali_c55_dma_write(struct mali_c55_ctx *ctx,
		       enum mali_c55_config_spaces cfg_space)
{
	struct mali_c55 *mali_c55 = ctx->mali_c55;
	struct device *dma_dev = mali_c55->channel->device->dev;
	dma_addr_t dst = ctx->base + config_space_addrs[cfg_space];
	dma_addr_t src;
	int ret;

	spin_lock(&ctx->lock);

	src = dma_map_single(dma_dev, ctx->registers,
			     MALI_C55_CONFIG_SPACE_SIZE, DMA_TO_DEVICE);
	if (dma_mapping_error(dma_dev, src)) {
		dev_err(mali_c55->dev, "failed to map DMA addr\n");
		ret = -EIO;
		goto out_unlock;
	}

	ret = mali_c55_dma_xfer(ctx, src, dst, DMA_TO_DEVICE);
	dma_unmap_single(dma_dev, src, MALI_C55_CONFIG_SPACE_SIZE,
			 DMA_TO_DEVICE);

out_unlock:
	spin_unlock(&ctx->lock);
	return ret;
}

static int mali_c55_config_read(struct mali_c55_ctx *ctx,
				enum mali_c55_config_spaces cfg_space)
{
	struct mali_c55 *mali_c55 = ctx->mali_c55;

	if (mali_c55->channel && allow_config_dma) {
		return mali_c55_dma_read(ctx, cfg_space);
	} else {
		memcpy_fromio(ctx->registers,
			      mali_c55->base + config_space_addrs[cfg_space],
			      MALI_C55_CONFIG_SPACE_SIZE);
		return 0;
	}
}

int mali_c55_config_write(struct mali_c55_ctx *ctx,
			  enum mali_c55_config_spaces cfg_space)
{
	struct mali_c55 *mali_c55 = ctx->mali_c55;

	if (mali_c55->channel && allow_config_dma) {
		return mali_c55_dma_write(ctx, cfg_space);
	} else {
		memcpy_toio(mali_c55->base + config_space_addrs[cfg_space],
			    ctx->registers, MALI_C55_CONFIG_SPACE_SIZE);
		return 0;
	}
}

struct mali_c55_ctx *mali_c55_get_active_context(struct mali_c55 *mali_c55)
{
	return list_first_entry(&mali_c55->contexts, struct mali_c55_ctx, list);
}

static void mali_c55_remove_links(struct mali_c55 *mali_c55)
{
	unsigned int i;

	media_entity_remove_links(&mali_c55->tpg.sd.entity);
	media_entity_remove_links(&mali_c55->isp.sd.entity);

	for (i = 0; i < MALI_C55_NUM_RZRS; i++)
		media_entity_remove_links(&mali_c55->resizers[i].sd.entity);

	for (i = 0; i < MALI_C55_NUM_CAP_DEVS; i++)
		media_entity_remove_links(&mali_c55->cap_devs[i].vdev.entity);
}

static int mali_c55_create_links(struct mali_c55 *mali_c55)
{
	int ret;

	/* Test pattern generator to ISP */
	ret = media_create_pad_link(&mali_c55->tpg.sd.entity, 0,
				    &mali_c55->isp.sd.entity,
				    MALI_C55_ISP_PAD_SINK_VIDEO, 0);
	if (ret) {
		dev_err(mali_c55->dev, "failed to link TPG and ISP\n");
		goto err_remove_links;
	}

	/* Full resolution resizer pipe. */
	ret = media_create_pad_link(&mali_c55->isp.sd.entity,
			MALI_C55_ISP_PAD_SOURCE,
			&mali_c55->resizers[MALI_C55_RZR_FR].sd.entity,
			MALI_C55_RZR_SINK_PAD,
			MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(mali_c55->dev, "failed to link ISP and FR resizer\n");
		goto err_remove_links;
	}

	/* Full resolution bypass. */
	ret = media_create_pad_link(&mali_c55->isp.sd.entity,
				    MALI_C55_ISP_PAD_SOURCE_BYPASS,
				    &mali_c55->resizers[MALI_C55_RZR_FR].sd.entity,
				    MALI_C55_RZR_SINK_BYPASS_PAD,
				    MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(mali_c55->dev, "failed to link ISP and FR resizer\n");
		goto err_remove_links;
	}

	/* Resizer pipe to video capture nodes. */
	ret = media_create_pad_link(&mali_c55->resizers[0].sd.entity,
			MALI_C55_RZR_SOURCE_PAD,
			&mali_c55->cap_devs[MALI_C55_CAP_DEV_FR].vdev.entity,
			0, MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
	if (ret) {
		dev_err(mali_c55->dev,
			"failed to link FR resizer and video device\n");
		goto err_remove_links;
	}

	/* The downscale pipe is an optional hardware block */
	if (mali_c55->capabilities & MALI_C55_GPS_DS_PIPE_FITTED) {
		ret = media_create_pad_link(&mali_c55->isp.sd.entity,
			MALI_C55_ISP_PAD_SOURCE,
			&mali_c55->resizers[MALI_C55_RZR_DS].sd.entity,
			MALI_C55_RZR_SINK_PAD,
			MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
		if (ret) {
			dev_err(mali_c55->dev,
				"failed to link ISP and DS resizer\n");
			goto err_remove_links;
		}

		ret = media_create_pad_link(&mali_c55->resizers[1].sd.entity,
			MALI_C55_RZR_SOURCE_PAD,
			&mali_c55->cap_devs[MALI_C55_CAP_DEV_DS].vdev.entity,
			0, MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
		if (ret) {
			dev_err(mali_c55->dev,
				"failed to link DS resizer and video device\n");
			goto err_remove_links;
		}
	}

	return 0;

err_remove_links:
	mali_c55_remove_links(mali_c55);
	return ret;
}

static void mali_c55_unregister_entities(struct mali_c55 *mali_c55)
{
	mali_c55_unregister_tpg(mali_c55);
	mali_c55_unregister_isp(mali_c55);
	mali_c55_unregister_resizers(mali_c55);
	mali_c55_unregister_capture_devs(mali_c55);
}

static int mali_c55_register_entities(struct mali_c55 *mali_c55)
{
	int ret;

	ret = mali_c55_register_tpg(mali_c55);
	if (ret)
		return ret;

	ret = mali_c55_register_isp(mali_c55);
	if (ret)
		goto err_unregister_entities;

	ret = mali_c55_register_resizers(mali_c55);
	if (ret)
		goto err_unregister_entities;

	ret = mali_c55_register_capture_devs(mali_c55);
	if (ret)
		goto err_unregister_entities;

	return mali_c55_create_links(mali_c55);

err_unregister_entities:
	mali_c55_unregister_entities(mali_c55);

	return ret;
}

static u32 mali_c55_check_hwcfg(struct mali_c55 *mali_c55)
{
	u32 product, version, revision, capabilities;

	product = mali_c55_read(mali_c55, MALI_C55_REG_PRODUCT, false);
	version = mali_c55_read(mali_c55, MALI_C55_REG_VERSION, false);
	revision = mali_c55_read(mali_c55, MALI_C55_REG_REVISION, false);

	dev_info(mali_c55->dev, "Detected Mali-C55 ISP %u.%u.%u\n",
		 product, version, revision);

	capabilities = mali_c55_read(mali_c55,
				     MALI_C55_REG_GLOBAL_PARAMETER_STATUS,
				     false);
	mali_c55->capabilities = (capabilities & 0xffff);

	/* TODO: Might as well start some debugfs */
	dev_info(mali_c55->dev, "Mali-C55 capabilities: 0x%04x\n", capabilities);
	return version;
}

static void mali_c55_swap_next_config(struct mali_c55 *mali_c55)
{
	struct mali_c55_ctx *ctx = mali_c55_get_active_context(mali_c55);
	u32 curr_config, next_config;

	curr_config = mali_c55_read(mali_c55, MALI_C55_REG_PING_PONG_READ, false);
	curr_config = (curr_config & MALI_C55_REG_PING_PONG_READ_MASK) >> 2;
	next_config = ~curr_config & 1;

	mali_c55_update_bits(mali_c55, MALI_C55_REG_MCU_CONFIG,
			     MALI_C55_REG_MCU_CONFIG_WRITE_MASK,
			     next_config << 1);

	mali_c55_config_write(ctx, next_config ?
			      MALI_C55_CONFIG_PING : MALI_C55_CONFIG_PONG);
}

static irqreturn_t mali_c55_isr(int irq, void *context)
{
	struct device *dev = context;
	struct mali_c55 *mali_c55 = dev_get_drvdata(dev);
	u32 interrupt_status;
	unsigned int i, j;

	interrupt_status = mali_c55_read(mali_c55,
					 MALI_C55_REG_INTERRUPT_STATUS_VECTOR,
					 false);
	if (!interrupt_status)
		return IRQ_NONE;

	mali_c55_write(mali_c55, MALI_C55_REG_INTERRUPT_CLEAR_VECTOR,
		       interrupt_status);
	mali_c55_write(mali_c55, MALI_C55_REG_INTERRUPT_CLEAR, 0);
	mali_c55_write(mali_c55, MALI_C55_REG_INTERRUPT_CLEAR, 1);

	for (i = 0; i < MALI_C55_NUM_IRQ_BITS; i++) {
		if (!(interrupt_status & (1 << i)))
			continue;

		switch (i) {
		case MALI_C55_IRQ_ISP_START:
			for (j = i; j < MALI_C55_NUM_CAP_DEVS; j++)
				mali_c55_set_next_buffer(&mali_c55->cap_devs[j]);

			mali_c55_swap_next_config(mali_c55);

			break;
		case MALI_C55_IRQ_ISP_DONE:
			/*
			 * TODO: Where the ISP has no Pong config fitted, we'd
			 * have to do the mali_c55_swap_next_config() call here.
			 */
			break;
		case MALI_C55_IRQ_FR_Y_DONE:
			mali_c55_set_plane_done(
				&mali_c55->cap_devs[MALI_C55_CAP_DEV_FR],
				MALI_C55_PLANE_Y);
			break;
		case MALI_C55_IRQ_FR_UV_DONE:
			mali_c55_set_plane_done(
				&mali_c55->cap_devs[MALI_C55_CAP_DEV_FR],
				MALI_C55_PLANE_UV);
			break;
		case MALI_C55_IRQ_DS_Y_DONE:
			mali_c55_set_plane_done(
				&mali_c55->cap_devs[MALI_C55_CAP_DEV_DS],
				MALI_C55_PLANE_Y);
			break;
		case MALI_C55_IRQ_DS_UV_DONE:
			mali_c55_set_plane_done(
				&mali_c55->cap_devs[MALI_C55_CAP_DEV_DS],
				MALI_C55_PLANE_UV);
			break;
		default:
			/*
			 * Only the above interrupts are currently unmasked. If
			 * we receive anything else here then something weird
			 * has gone on.
			 */
			dev_err(dev, "masked interrupt %s triggered\n",
				mali_c55_interrupt_names[i]);
		}
	}

	return IRQ_HANDLED;
}

static int mali_c55_interrupt_init(struct platform_device *pdev)
{
	int ret;

	ret = platform_get_irq(pdev, 0);
	if (ret < 0)
		return ret;

	ret = devm_request_threaded_irq(&pdev->dev, ret, NULL,
					mali_c55_isr, IRQF_ONESHOT,
					dev_driver_string(&pdev->dev),
					&pdev->dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to request irq\n");
		return ret;
	}

	return 0;
}

static int mali_c55_init_context(struct mali_c55 *mali_c55)
{
	struct mali_c55_ctx *ctx;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		dev_err(mali_c55->dev, "failed to allocate new context\n");
		return -ENOMEM;
	}

	ctx->base = mali_c55->res->start;
	ctx->mali_c55 = mali_c55;

	ctx->registers = kzalloc(MALI_C55_CONFIG_SPACE_SIZE,
				 GFP_KERNEL | GFP_DMA);
	if (!ctx->registers) {
		ret = -ENOMEM;
		goto err_free_ctx;
	}

	/*
	 * The allocated memory is empty, we need to load the default
	 * register settings. We just read Ping; it's identical to Pong.
	 */
	ret = mali_c55_config_read(ctx, MALI_C55_CONFIG_PING);
	if (ret)
		goto err_free_registers;

	list_add_tail(&ctx->list, &mali_c55->contexts);

	return 0;

err_free_registers:
	kfree(ctx->registers);
err_free_ctx:
	kfree(ctx);

	return ret;
}

static int mali_c55_runtime_resume(struct device *dev)
{
	struct mali_c55 *mali_c55 = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(ARRAY_SIZE(mali_c55->clks),
				      mali_c55->clks);
	if (ret) {
		dev_err(mali_c55->dev, "failed to enable clocks\n");
		return ret;
	}

	return 0;
}

static int mali_c55_runtime_suspend(struct device *dev)
{
	struct mali_c55 *mali_c55 = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(ARRAY_SIZE(mali_c55->clks), mali_c55->clks);
	return 0;
}

static const struct dev_pm_ops mali_c55_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(mali_c55_runtime_suspend, mali_c55_runtime_resume,
			   NULL)
};

static int mali_c55_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mali_c55 *mali_c55;
	dma_cap_mask_t mask;
	u32 version;
	int ret;
	u32 val;

	mali_c55 = devm_kzalloc(dev, sizeof(*mali_c55), GFP_KERNEL);
	if (!mali_c55)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate memory\n");

	mali_c55->dev = dev;
	platform_set_drvdata(pdev, mali_c55);

	mali_c55->base = devm_platform_get_and_ioremap_resource(pdev, 0,
								&mali_c55->res);
	if (IS_ERR(mali_c55->base))
		return dev_err_probe(dev, PTR_ERR(mali_c55->base),
				     "failed to map IO memory\n");

	ret = mali_c55_interrupt_init(pdev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialise interrupts\n");

	for (unsigned int i = 0; i < ARRAY_SIZE(mali_c55_clk_names); i++)
		mali_c55->clks[i].id = mali_c55_clk_names[i];

	ret = devm_clk_bulk_get(dev, ARRAY_SIZE(mali_c55->clks), mali_c55->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to acquire clocks\n");

	pm_runtime_enable(&pdev->dev);

	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret)
		goto err_pm_runtime_disable;

	of_reserved_mem_device_init(dev);
	version = mali_c55_check_hwcfg(mali_c55);
	vb2_dma_contig_set_max_seg_size(dev, UINT_MAX);

	/* Use "software only" context management. */
	mali_c55_update_bits(mali_c55, MALI_C55_REG_MCU_CONFIG,
			     MALI_C55_REG_MCU_CONFIG_OVERRIDE_MASK,
			     MALI_C55_REG_MCU_CONFIG_OVERRIDE_MASK);

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	/*
	 * No error check, because we will just fallback on memcpy if there is
	 * no usable DMA channel on the system.
	 */
	mali_c55->channel = dma_request_channel(mask, NULL, NULL);

	INIT_LIST_HEAD(&mali_c55->contexts);
	ret = mali_c55_init_context(mali_c55);
	if (ret)
		goto err_release_dma_channel;

	mali_c55->media_dev.dev = dev;
	strscpy(mali_c55->media_dev.model, "ARM Mali-C55 ISP",
		sizeof(mali_c55->media_dev.model));
	mali_c55->media_dev.hw_revision = version;

	media_device_init(&mali_c55->media_dev);
	ret = media_device_register(&mali_c55->media_dev);
	if (ret)
		goto err_cleanup_media_device;

	mali_c55->v4l2_dev.mdev = &mali_c55->media_dev;
	ret = v4l2_device_register(dev, &mali_c55->v4l2_dev);
	if (ret) {
		dev_err(dev, "failed to register V4L2 device\n");
		goto err_unregister_media_device;
	};

	ret = mali_c55_register_entities(mali_c55);
	if (ret) {
		dev_err(dev, "failed to register entities\n");
		goto err_unregister_v4l2_device;
	}

	/* Set safe stop to ensure we're in a non-streaming state */
	mali_c55_write(mali_c55, MALI_C55_REG_INPUT_MODE_REQUEST,
		       MALI_C55_INPUT_SAFE_STOP);
	readl_poll_timeout(mali_c55->base + MALI_C55_REG_MODE_STATUS,
			   val, !val, 10 * USEC_PER_MSEC, 250 * USEC_PER_MSEC);

	/*
	 * For now, few of the ISP's features are supported (pretty much just
	 * debayering and the colour space conversion). Flag the others as being
	 * bypassed so that they don't interfere.
	 *
	 * TODO: Support more features!
	 */
	mali_c55_write(mali_c55, 0x18eac, 0x3c);
	mali_c55_write(mali_c55, 0x18eb0, 0xf);
	mali_c55_write(mali_c55, 0x18eb8, 0x3);
	mali_c55_write(mali_c55, 0x18ebc, 0x7b);
	mali_c55_write(mali_c55, 0x18ec0, 0x38);
	mali_c55_write(mali_c55, MALI_C55_REG_FR_BYPASS, 0xf);
	mali_c55_write(mali_c55, MALI_C55_REG_DS_BYPASS, 0xf);

	/*
	 * We're ready to process interrupts. Clear any that are set and then
	 * unmask them for processing.
	 */
	mali_c55_write(mali_c55, 0x30, 0xffffffff);
	mali_c55_write(mali_c55, 0x34, 0xffffffff);
	mali_c55_write(mali_c55, 0x40, 0x01);
	mali_c55_write(mali_c55, 0x40, 0x00);
	mali_c55_write(mali_c55, MALI_C55_REG_INTERRUPT_MASK_VECTOR, 0x39fc3fc);

	pm_runtime_put(&pdev->dev);

	return 0;

err_unregister_v4l2_device:
	v4l2_device_unregister(&mali_c55->v4l2_dev);
err_unregister_media_device:
	media_device_unregister(&mali_c55->media_dev);
err_cleanup_media_device:
	media_device_cleanup(&mali_c55->media_dev);
err_release_dma_channel:
	dma_release_channel(mali_c55->channel);
err_pm_runtime_disable:
	pm_runtime_disable(&pdev->dev);

	return ret;
}

static void mali_c55_remove(struct platform_device *pdev)
{
	struct mali_c55 *mali_c55 = platform_get_drvdata(pdev);
	struct mali_c55_ctx *ctx, *tmp;

	list_for_each_entry_safe(ctx, tmp, &mali_c55->contexts, list) {
		list_del(&ctx->list);
		kfree(ctx->registers);
		kfree(ctx);
	}

	mali_c55_remove_links(mali_c55);
	mali_c55_unregister_entities(mali_c55);
	v4l2_device_put(&mali_c55->v4l2_dev);
	media_device_unregister(&mali_c55->media_dev);
	media_device_cleanup(&mali_c55->media_dev);
	dma_release_channel(mali_c55->channel);
}

static const struct of_device_id mali_c55_of_match[] = {
	{
		.compatible = "arm,mali-c55",
	},
	{},
};
MODULE_DEVICE_TABLE(of, mali_c55_of_match);

static struct platform_driver mali_c55_driver = {
	.driver = {
		.name = "mali-c55",
		.of_match_table = of_match_ptr(mali_c55_of_match),
		.pm = &mali_c55_pm_ops,
	},
	.probe = mali_c55_probe,
	.remove_new = mali_c55_remove,
};

module_platform_driver(mali_c55_driver);
MODULE_AUTHOR("Daniel Scally <dan.scally@ideasonboard.com>");
MODULE_AUTHOR("Jacopo Mondi <jacopo.mondi@ideasonboard.com>");
MODULE_DESCRIPTION("ARM Mali-C55 ISP platform driver");
MODULE_LICENSE("GPL");
