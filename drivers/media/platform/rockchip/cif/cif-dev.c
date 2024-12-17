// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip Camera Interface (CIF) Driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2020 Maxime Chevallier <maxime.chevallier@bootlin.com>
 * Copyright (C) 2023 Mehdi Djait <mehdi.djait@bootlin.com>
 * Copyright (C) 2024 Michael Riesch <michael.riesch@wolfvision.net>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>

#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>

#include "cif-capture-dvp.h"
#include "cif-common.h"

const char *const px30_vip_clks[] = {
	"aclk",
	"hclk",
	"pclk",
};

static const struct cif_match_data px30_vip_match_data = {
	.clks = px30_vip_clks,
	.clks_num = ARRAY_SIZE(px30_vip_clks),
	.dvp = &px30_vip_dvp_match_data,
};

const char *const rk3568_vicap_clks[] = {
	"aclk",
	"hclk",
	"dclk",
	"iclk",
};

static const struct cif_match_data rk3568_vicap_match_data = {
	.clks = rk3568_vicap_clks,
	.clks_num = ARRAY_SIZE(rk3568_vicap_clks),
	.dvp = &rk3568_vicap_dvp_match_data,
};

static const struct of_device_id cif_plat_of_match[] = {
	{
		.compatible = "rockchip,px30-vip",
		.data = &px30_vip_match_data,
	},
	{
		.compatible = "rockchip,rk3568-vicap",
		.data = &rk3568_vicap_match_data,
	},
	{},
};

static void cif_notify(struct v4l2_subdev *sd, unsigned int notification,
		       void *arg)
{
	struct v4l2_device *v4l2_dev = sd->v4l2_dev;
	struct cif_device *cif =
		container_of(v4l2_dev, struct cif_device, v4l2_dev);
	struct video_device *vdev = NULL;

	if ((cif->dvp.stream.remote) && (cif->dvp.stream.remote->sd == sd))
		vdev = &cif->dvp.stream.vdev;

	if (!vdev)
		return;

	switch (notification) {
	case V4L2_DEVICE_NOTIFY_EVENT:
		v4l2_event_queue(vdev, arg);
		break;
	default:
		break;
	}
}

static int cif_subdev_notifier_bound(struct v4l2_async_notifier *notifier,
				     struct v4l2_subdev *sd,
				     struct v4l2_async_connection *asd)
{
	struct cif_device *cif_dev =
		container_of(notifier, struct cif_device, notifier);
	struct cif_remote *remote =
		container_of(asd, struct cif_remote, async_conn);
	int source_pad;
	int ret;

	source_pad = media_entity_get_fwnode_pad(&sd->entity, sd->fwnode,
						 MEDIA_PAD_FL_SOURCE);
	if (source_pad < 0) {
		dev_err(cif_dev->dev, "failed to find source pad for %s\n",
			sd->name);
		return source_pad;
	}

	remote->sd = sd;
	remote->source_pad = source_pad;

	ret = media_create_pad_link(&sd->entity, source_pad,
				    &remote->stream->vdev.entity, 0,
				    MEDIA_LNK_FL_ENABLED);
	if (ret) {
		dev_err(cif_dev->dev, "failed to link source pad of %s\n",
			sd->name);
		return ret;
	}

	return 0;
}

static int cif_subdev_notifier_complete(struct v4l2_async_notifier *notifier)
{
	struct cif_device *cif_dev =
		container_of(notifier, struct cif_device, notifier);

	return v4l2_device_register_subdev_nodes(&cif_dev->v4l2_dev);
}

static const struct v4l2_async_notifier_operations cif_subdev_notifier_ops = {
	.bound = cif_subdev_notifier_bound,
	.complete = cif_subdev_notifier_complete,
};

static int cif_subdev_notifier_register(struct cif_device *cif_dev, int index)
{
	struct v4l2_async_notifier *ntf = &cif_dev->notifier;
	struct v4l2_fwnode_endpoint *vep;
	struct cif_remote *remote;
	struct device *dev = cif_dev->dev;
	struct fwnode_handle *ep;
	int ret;

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), index, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep)
		return -ENODEV;

	if (index == 0) {
		vep = &cif_dev->dvp.vep;

		vep->bus_type = V4L2_MBUS_UNKNOWN;
		ret = v4l2_fwnode_endpoint_parse(ep, vep);
		if (ret)
			goto complete;

		if (vep->bus_type != V4L2_MBUS_BT656 &&
		    vep->bus_type != V4L2_MBUS_PARALLEL) {
			v4l2_err(&cif_dev->v4l2_dev, "unsupported bus type\n");
			goto complete;
		}

		remote = v4l2_async_nf_add_fwnode_remote(ntf, ep,
							 struct cif_remote);
		if (IS_ERR(remote)) {
			ret = PTR_ERR(remote);
			goto complete;
		}

		cif_dev->dvp.stream.remote = remote;
		remote->stream = &cif_dev->dvp.stream;
	} else {
		ret = -ENODEV;
		goto complete;
	}

complete:
	fwnode_handle_put(ep);

	return ret;
}

static void cif_subdev_notifier_unregister(struct cif_device *cif_dev,
					   int index)
{
}

static int cif_entities_register(struct cif_device *cif_dev)
{
	struct v4l2_async_notifier *ntf = &cif_dev->notifier;
	struct device *dev = cif_dev->dev;
	int ret;

	v4l2_async_nf_init(ntf, &cif_dev->v4l2_dev);
	ntf->ops = &cif_subdev_notifier_ops;

	if (cif_dev->match_data->dvp) {
		ret = cif_subdev_notifier_register(cif_dev, 0);
		if (ret) {
			dev_err(dev,
				"failed to register notifier for dvp: %d\n",
				ret);
			goto err;
		}

		ret = cif_dvp_register(cif_dev);
		if (ret) {
			dev_err(dev, "failed to register dvp: %d\n", ret);
			goto err_dvp_notifier_unregister;
		}
	}

	ret = v4l2_async_nf_register(ntf);
	if (ret)
		goto err_notifier_cleanup;

	return 0;

err_notifier_cleanup:
	if (cif_dev->match_data->dvp)
		cif_dvp_unregister(cif_dev);
err_dvp_notifier_unregister:
	if (cif_dev->match_data->dvp)
		cif_subdev_notifier_unregister(cif_dev, 0);
	v4l2_async_nf_cleanup(ntf);
err:
	return ret;
}

static void cif_entities_unregister(struct cif_device *cif_dev)
{
	v4l2_async_nf_unregister(&cif_dev->notifier);
	v4l2_async_nf_cleanup(&cif_dev->notifier);

	if (cif_dev->match_data->dvp) {
		cif_subdev_notifier_unregister(cif_dev, 0);
		cif_dvp_unregister(cif_dev);
	}
}

static irqreturn_t cif_isr(int irq, void *ctx)
{
	irqreturn_t ret = IRQ_NONE;

	if (cif_dvp_isr(irq, ctx) == IRQ_HANDLED)
		ret = IRQ_HANDLED;

	return ret;
}

static int cif_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cif_device *cif_dev;
	u32 cif_clk_delaynum = 0;
	int ret, irq, i;

	cif_dev = devm_kzalloc(dev, sizeof(*cif_dev), GFP_KERNEL);
	if (!cif_dev)
		return -ENOMEM;

	cif_dev->match_data = of_device_get_match_data(dev);
	if (!cif_dev->match_data)
		return -ENODEV;

	dev_set_drvdata(dev, cif_dev);
	cif_dev->dev = dev;

	cif_dev->base_addr = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(cif_dev->base_addr))
		return PTR_ERR(cif_dev->base_addr);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, cif_isr, IRQF_SHARED,
			       dev_driver_string(dev), dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request irq\n");
	cif_dev->irq = irq;

	cif_dev->clks_num = cif_dev->match_data->clks_num;
	for (i = 0; (i < cif_dev->clks_num) && (i < CIF_CLKS_MAX); i++)
		cif_dev->clks[i].id = cif_dev->match_data->clks[i];
	ret = devm_clk_bulk_get(dev, cif_dev->clks_num, cif_dev->clks);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get clocks\n");

	cif_dev->cif_rst = devm_reset_control_array_get_exclusive(dev);
	if (IS_ERR(cif_dev->cif_rst))
		return PTR_ERR(cif_dev->cif_rst);

	cif_dev->grf =
		syscon_regmap_lookup_by_phandle(dev->of_node, "rockchip,grf");
	if (IS_ERR(cif_dev->grf))
		cif_dev->grf = NULL;

	if (cif_dev->match_data->dvp) {
		of_property_read_u32(dev->of_node, "rockchip,cif-clk-delaynum",
				     &cif_clk_delaynum);
		cif_dev->dvp.cif_clk_delaynum = cif_clk_delaynum;
	}

	pm_runtime_enable(&pdev->dev);

	cif_dev->media_dev.dev = dev;
	strscpy(cif_dev->media_dev.model, CIF_DRIVER_NAME,
		sizeof(cif_dev->media_dev.model));
	media_device_init(&cif_dev->media_dev);

	cif_dev->v4l2_dev.mdev = &cif_dev->media_dev;
	cif_dev->v4l2_dev.notify = cif_notify;
	strscpy(cif_dev->v4l2_dev.name, CIF_DRIVER_NAME,
		sizeof(cif_dev->v4l2_dev.name));

	ret = v4l2_device_register(dev, &cif_dev->v4l2_dev);
	if (ret)
		goto err_media_dev_cleanup;

	ret = media_device_register(&cif_dev->media_dev);
	if (ret < 0) {
		dev_err(dev, "failed to register media device: %d\n", ret);
		goto err_v4l2_dev_unregister;
	}

	ret = cif_entities_register(cif_dev);
	if (ret) {
		dev_err(dev, "failed to register media entities: %d\n", ret);
		goto err_media_dev_unregister;
	}

	return 0;

err_media_dev_unregister:
	media_device_unregister(&cif_dev->media_dev);
err_v4l2_dev_unregister:
	v4l2_device_unregister(&cif_dev->v4l2_dev);
err_media_dev_cleanup:
	media_device_cleanup(&cif_dev->media_dev);
	pm_runtime_disable(&pdev->dev);
	return ret;
}

static void cif_remove(struct platform_device *pdev)
{
	struct cif_device *cif_dev = platform_get_drvdata(pdev);

	cif_entities_unregister(cif_dev);
	media_device_unregister(&cif_dev->media_dev);
	v4l2_device_unregister(&cif_dev->v4l2_dev);
	media_device_cleanup(&cif_dev->media_dev);
	pm_runtime_disable(&pdev->dev);
}

static int cif_runtime_suspend(struct device *dev)
{
	struct cif_device *cif_dev = dev_get_drvdata(dev);

	/*
	 * Reset CIF (CRU, DMA, FIFOs) to allow a clean resume.
	 * Since this resets the IOMMU too, we cannot issue this reset when
	 * resuming.
	 */
	reset_control_assert(cif_dev->cif_rst);
	udelay(5);
	reset_control_deassert(cif_dev->cif_rst);

	clk_bulk_disable_unprepare(cif_dev->clks_num, cif_dev->clks);

	return 0;
}

static int cif_runtime_resume(struct device *dev)
{
	struct cif_device *cif_dev = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(cif_dev->clks_num, cif_dev->clks);
	if (ret) {
		dev_err(dev, "failed to enable clocks\n");
		return ret;
	}

	return 0;
}

static const struct dev_pm_ops cif_plat_pm_ops = {
	.runtime_suspend = cif_runtime_suspend,
	.runtime_resume = cif_runtime_resume,
};

static struct platform_driver cif_plat_drv = {
	.driver = {
		   .name = CIF_DRIVER_NAME,
		   .of_match_table = cif_plat_of_match,
		   .pm = &cif_plat_pm_ops,
	},
	.probe = cif_probe,
	.remove_new = cif_remove,
};
module_platform_driver(cif_plat_drv);

MODULE_DESCRIPTION("Rockchip Camera Interface (CIF) platform driver");
MODULE_LICENSE("GPL");
