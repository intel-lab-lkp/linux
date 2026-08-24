// SPDX-License-Identifier: (GPL-2.0-or-later OR MIT)
/*
 * Rockchip ISP2 Driver - Base driver
 *
 * Copyright (C) 2019 Collabora, Ltd.
 * Copyright (C) 2026 Ideas on Board Oy.
 *
 * Based on Rockchip ISP1 driver by Rockchip Electronics Co., Ltd.
 * Copyright (C) 2017 Rockchip Electronics Co., Ltd.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/iommu.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mc.h>

#include "rkisp2-common.h"

struct rkisp2_isr_data {
	const char *name;
	irqreturn_t (*isr)(int irq, void *ctx);
	u32 line_mask;
};

/* ----------------------------------------------------------------------------
 * Power
 */

static int __maybe_unused rkisp2_runtime_suspend(struct device *dev)
{
	struct rkisp2_device *rkisp2 = dev_get_drvdata(dev);

	rkisp2->irqs_enabled = false;
	/* Make sure the IRQ handler will see the above */
	mb();

	/*
	 * Wait until any running IRQ handler has returned. The IRQ handler
	 * may get called even after this (as it's a shared interrupt line)
	 * but the 'irqs_enabled' flag will make the handler return immediately.
	 */
	for (unsigned int il = 0; il < ARRAY_SIZE(rkisp2->irqs); ++il) {
		if (rkisp2->irqs[il] == -1)
			continue;

		/* Skip if the irq line is the same as previous */
		if (il == 0 || rkisp2->irqs[il - 1] != rkisp2->irqs[il])
			synchronize_irq(rkisp2->irqs[il]);
	}

	clk_bulk_disable_unprepare(rkisp2->clk_size, rkisp2->clks);
	return pinctrl_pm_select_sleep_state(dev);
}

static int __maybe_unused rkisp2_runtime_resume(struct device *dev)
{
	struct rkisp2_device *rkisp2 = dev_get_drvdata(dev);
	int ret;

	ret = pinctrl_pm_select_default_state(dev);
	if (ret)
		return ret;
	ret = clk_bulk_prepare_enable(rkisp2->clk_size, rkisp2->clks);
	if (ret)
		return ret;

	rkisp2->irqs_enabled = true;
	/* Make sure the IRQ handler will see the above */
	mb();

	rkisp2_write(rkisp2, ISP3X_SWS_CFG, RKISP2_CIF_SWS_MIPI_DROP_FRM_DIS);

	return 0;
}

static const struct dev_pm_ops rkisp2_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(rkisp2_runtime_suspend, rkisp2_runtime_resume, NULL)
};

/* ----------------------------------------------------------------------------
 * Core
 */

static int rkisp2_create_links(struct rkisp2_device *rkisp2)
{
	unsigned int i;
	int ret;

	for (i = 0; i < 2; i++) {
		struct media_entity *capture =
			&rkisp2->capture_devs[i].vnode.vdev.entity;

		ret = media_create_pad_link(&rkisp2->isp.sd.entity,
					    RKISP2_ISP_PAD_SOURCE_VIDEO_MAIN + i,
					    capture, 0,
					    MEDIA_LNK_FL_ENABLED |
					    MEDIA_LNK_FL_IMMUTABLE);
		if (ret)
			return ret;
	}

	for (i = 0; i < rkisp2->dmarx.num_chans; i++) {
		struct media_entity *source =
			&rkisp2->dmarx.chan[i].vnode.vdev.entity;
		u32 flags = (i == RKISP2_RAWRD0 ?  MEDIA_LNK_FL_ENABLED : 0);
		ret = media_create_pad_link(source, 0,
					    &rkisp2->isp.sd.entity,
					    RKISP2_ISP_PAD_SINK_VIDEO_DMA_BASE + i,
					    flags);
		if (ret)
			return ret;
	}

	/* params links */
	ret = media_create_pad_link(&rkisp2->params.vnode.vdev.entity, 0,
				    &rkisp2->isp.sd.entity,
				    RKISP2_ISP_PAD_SINK_PARAMS,
				    MEDIA_LNK_FL_ENABLED |
				    MEDIA_LNK_FL_IMMUTABLE);
	if (ret)
		return ret;


	return 0;
}

static void rkisp2_entities_unregister(struct rkisp2_device *rkisp2)
{
	rkisp2_params_unregister(rkisp2);
	rkisp2_dmarx_unregister(rkisp2);
	rkisp2_capture_devs_unregister(rkisp2);
	rkisp2_isp_unregister(rkisp2);
}

static int rkisp2_entities_register(struct rkisp2_device *rkisp2)
{
	int ret;

	ret = rkisp2_isp_register(rkisp2);
	if (ret)
		goto error;

	rkisp2_resizer_devs_init(rkisp2);

	ret = rkisp2_capture_devs_register(rkisp2);
	if (ret)
		goto error;

	ret = rkisp2_dmarx_register(rkisp2);
	if (ret)
		goto error;

	ret = rkisp2_params_register(rkisp2);
	if (ret)
		goto error;

	ret = rkisp2_create_links(rkisp2);
	if (ret)
		goto error;

	return 0;

error:
	rkisp2_entities_unregister(rkisp2);
	return ret;
}

static const char * const rk3588_isp_clks[] = {
	"aclk",
	"hclk",
	"clk_core",
	"clk_core_marvin",
	"clk_core_vicap",
};

static const struct rkisp2_isr_data rk3588_isp_isrs[] = {
	{ "isp_irq", rkisp2_isp_isr, BIT(RKISP2_IRQ_ISP) },
	{ "mi_irq", rkisp2_capture_isr, BIT(RKISP2_IRQ_MI) },
};

static const struct rkisp2_info rk3588_isp_info = {
	.clks = rk3588_isp_clks,
	.clk_size = ARRAY_SIZE(rk3588_isp_clks),
	.isrs = rk3588_isp_isrs,
	.isr_size = ARRAY_SIZE(rk3588_isp_isrs),
	.isp_ver = RKISP3_V0,
	.features = RKISP2_FEATURE_DUAL_CROP,
	.max_width = 4672,
	.max_height = 3504,
};

static const struct of_device_id rkisp2_of_match[] = {
	{
		.compatible = "rockchip,rk3588-isp",
		.data = &rk3588_isp_info,
	},
	{},
};
MODULE_DEVICE_TABLE(of, rkisp2_of_match);

static int rkisp2_probe(struct platform_device *pdev)
{
	const struct rkisp2_info *info;
	struct device *dev = &pdev->dev;
	struct rkisp2_device *rkisp2;
	struct v4l2_device *v4l2_dev;
	unsigned int i;
	int ret, irq;
	u32 cif_id;

	rkisp2 = devm_kzalloc(dev, sizeof(*rkisp2), GFP_KERNEL);
	if (!rkisp2)
		return -ENOMEM;

	info = of_device_get_match_data(dev);
	rkisp2->info = info;

	dev_set_drvdata(dev, rkisp2);
	rkisp2->dev = dev;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	mutex_init(&rkisp2->stream_lock);

	rkisp2->base_addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(rkisp2->base_addr))
		return PTR_ERR(rkisp2->base_addr);

	for (unsigned int il = 0; il < ARRAY_SIZE(rkisp2->irqs); ++il)
		rkisp2->irqs[il] = -1;

	for (i = 0; i < info->isr_size; i++) {
		irq = info->isrs[i].name
		    ? platform_get_irq_byname(pdev, info->isrs[i].name)
		    : platform_get_irq(pdev, i);
		if (irq < 0)
			return irq;

		for (unsigned int il = 0; il < ARRAY_SIZE(rkisp2->irqs); ++il) {
			if (info->isrs[i].line_mask & BIT(il))
				rkisp2->irqs[il] = irq;
		}

		ret = devm_request_irq(dev, irq, info->isrs[i].isr, IRQF_SHARED,
				       dev_driver_string(dev), dev);
		if (ret) {
			dev_err(dev, "request irq failed: %d\n", ret);
			return ret;
		}
	}

	for (i = 0; i < info->clk_size; i++)
		rkisp2->clks[i].id = info->clks[i];
	ret = devm_clk_bulk_get(dev, info->clk_size, rkisp2->clks);
	if (ret)
		return ret;
	rkisp2->clk_size = info->clk_size;

	pm_runtime_enable(&pdev->dev);

	ret = pm_runtime_resume_and_get(&pdev->dev);
	if (ret)
		goto err_pm_runtime_disable;

	cif_id = rkisp2_read(rkisp2, RKISP2_CIF_VI_ID);
	dev_dbg(rkisp2->dev, "CIF_ID 0x%08x\n", cif_id);

	pm_runtime_put(&pdev->dev);

	rkisp2->media_dev.hw_revision = info->isp_ver;
	strscpy(rkisp2->media_dev.model, RKISP2_DRIVER_NAME,
		sizeof(rkisp2->media_dev.model));
	rkisp2->media_dev.dev = &pdev->dev;
	strscpy(rkisp2->media_dev.bus_info, RKISP2_BUS_INFO,
		sizeof(rkisp2->media_dev.bus_info));
	media_device_init(&rkisp2->media_dev);

	v4l2_dev = &rkisp2->v4l2_dev;
	v4l2_dev->mdev = &rkisp2->media_dev;
	strscpy(v4l2_dev->name, RKISP2_DRIVER_NAME, sizeof(v4l2_dev->name));

	ret = v4l2_device_register(rkisp2->dev, &rkisp2->v4l2_dev);
	if (ret)
		goto err_media_dev_cleanup;

	ret = media_device_register(&rkisp2->media_dev);
	if (ret)
		goto err_unreg_v4l2_dev;

	ret = rkisp2_entities_register(rkisp2);
	if (ret)
		goto err_unreg_media_dev;

	ret = v4l2_device_register_subdev_nodes(&rkisp2->v4l2_dev);
	if (ret)
		goto err_unreg_entities;

	rkisp2_debug_init(rkisp2);

	return 0;

err_unreg_entities:
	rkisp2_entities_unregister(rkisp2);
err_unreg_media_dev:
	media_device_unregister(&rkisp2->media_dev);
err_unreg_v4l2_dev:
	v4l2_device_unregister(&rkisp2->v4l2_dev);
err_media_dev_cleanup:
	media_device_cleanup(&rkisp2->media_dev);
err_pm_runtime_disable:
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static void rkisp2_remove(struct platform_device *pdev)
{
	struct rkisp2_device *rkisp2 = platform_get_drvdata(pdev);

	v4l2_async_nf_unregister(&rkisp2->notifier);
	v4l2_async_nf_cleanup(&rkisp2->notifier);

	rkisp2_entities_unregister(rkisp2);
	rkisp2_debug_cleanup(rkisp2);

	media_device_unregister(&rkisp2->media_dev);
	v4l2_device_unregister(&rkisp2->v4l2_dev);

	media_device_cleanup(&rkisp2->media_dev);

	pm_runtime_disable(&pdev->dev);
}

static struct platform_driver rkisp2_drv = {
	.driver = {
		.name = RKISP2_DRIVER_NAME,
		.of_match_table = of_match_ptr(rkisp2_of_match),
		.pm = &rkisp2_pm_ops,
	},
	.probe = rkisp2_probe,
	.remove = rkisp2_remove,
};

module_platform_driver(rkisp2_drv);
MODULE_DESCRIPTION("Rockchip ISP2 platform driver");
MODULE_LICENSE("Dual MIT/GPL");
