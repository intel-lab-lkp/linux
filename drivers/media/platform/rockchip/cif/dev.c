// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip CIF Camera Interface Driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 * Copyright (C) 2020 Maxime Chevallier <maxime.chevallier@bootlin.com>
 * Copyright (C) 2023 Mehdi Djait <mehdi.djait@bootlin.com>
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/reset.h>
#include <linux/pm_runtime.h>
#include <linux/pinctrl/consumer.h>
#include <media/v4l2-fwnode.h>

#include "capture.h"
#include "common.h"
#include "regs.h"

static int subdev_notifier_complete(struct v4l2_async_notifier *notifier)
{
	struct cif_device *cif_dev;
	struct v4l2_subdev *sd;
	int ret;

	cif_dev = container_of(notifier, struct cif_device, notifier);
	sd = cif_dev->remote.sd;

	mutex_lock(&cif_dev->media_dev.graph_mutex);

	ret = v4l2_device_register_subdev_nodes(&cif_dev->v4l2_dev);
	if (ret < 0)
		goto unlock;

	ret = media_create_pad_link(&sd->entity, 0,
				    &cif_dev->stream.vdev.entity, 0,
				    MEDIA_LNK_FL_ENABLED);
	if (ret)
		dev_err(cif_dev->dev, "failed to create link");

unlock:
	mutex_unlock(&cif_dev->media_dev.graph_mutex);
	return ret;
}

static int subdev_notifier_bound(struct v4l2_async_notifier *notifier,
				 struct v4l2_subdev *subdev,
				 struct v4l2_async_connection *asd)
{
	struct cif_device *cif_dev = container_of(notifier,
						  struct cif_device, notifier);
	int pad;

	cif_dev->remote.sd = subdev;
	pad = media_entity_get_fwnode_pad(&subdev->entity, subdev->fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (pad < 0)
		return pad;

	cif_dev->remote.pad = pad;

	return 0;
}

static const struct v4l2_async_notifier_operations subdev_notifier_ops = {
	.bound = subdev_notifier_bound,
	.complete = subdev_notifier_complete,
};

static int cif_subdev_notifier(struct cif_device *cif_dev)
{
	struct v4l2_async_notifier *ntf = &cif_dev->notifier;
	struct device *dev = cif_dev->dev;
	struct v4l2_async_connection *asd;
	struct v4l2_fwnode_endpoint vep = {
		.bus_type = V4L2_MBUS_UNKNOWN,
	};
	struct fwnode_handle *ep;
	int ret;

	v4l2_async_nf_init(ntf, &cif_dev->v4l2_dev);

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep)
		return -ENODEV;

	ret = v4l2_fwnode_endpoint_parse(ep, &vep);
	if (ret)
		goto complete;

	if (vep.bus_type != V4L2_MBUS_BT656 &&
	    vep.bus_type != V4L2_MBUS_PARALLEL) {
		v4l2_err(&cif_dev->v4l2_dev, "unsupported bus type\n");
		goto complete;
	}

	asd = v4l2_async_nf_add_fwnode_remote(ntf, ep,
					      struct v4l2_async_connection);
	if (IS_ERR(asd)) {
		ret = PTR_ERR(asd);
		goto complete;
	}

	ntf->ops = &subdev_notifier_ops;

	ret = v4l2_async_nf_register(ntf);
	if (ret)
		v4l2_async_nf_cleanup(ntf);

complete:
	fwnode_handle_put(ep);

	return ret;
}

static struct clk_bulk_data px30_cif_clks[] = {
	{ .id = "aclk", },
	{ .id = "hclk", },
	{ .id = "pclk", },
};

static const struct cif_match_data px30_cif_match_data = {
	.clks = px30_cif_clks,
	.clks_num = ARRAY_SIZE(px30_cif_clks),
};

static const struct of_device_id cif_plat_of_match[] = {
	{
		.compatible = "rockchip,px30-vip",
		.data = &px30_cif_match_data,
	},
	{},
};

static int cif_get_resource(struct platform_device *pdev,
			    struct cif_device *cif_dev)
{
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev,
			"Unable to allocate resources for device\n");
		return -ENODEV;
	}

	cif_dev->base_addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(cif_dev->base_addr))
		return PTR_ERR(cif_dev->base_addr);

	return 0;
}

static int cif_plat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct v4l2_device *v4l2_dev;
	struct cif_device *cif_dev;
	int ret, irq;

	cif_dev = devm_kzalloc(dev, sizeof(*cif_dev), GFP_KERNEL);
	if (!cif_dev)
		return -ENOMEM;

	cif_dev->match_data = of_device_get_match_data(dev);
	if (!cif_dev->match_data)
		return -ENODEV;

	platform_set_drvdata(pdev, cif_dev);
	cif_dev->dev = dev;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, cif_irq_pingpong, IRQF_SHARED,
			       dev_driver_string(dev), dev);
	if (ret)
		return dev_err_probe(dev, ret, "request irq failed\n");

	cif_dev->irq = irq;

	ret = cif_get_resource(pdev, cif_dev);
	if (ret)
		return ret;

	ret = devm_clk_bulk_get(dev, cif_dev->match_data->clks_num,
				cif_dev->match_data->clks);
	if (ret)
		return ret;

	cif_dev->cif_rst = devm_reset_control_array_get(dev, false, false);
	if (IS_ERR(cif_dev->cif_rst))
		return PTR_ERR(cif_dev->cif_rst);

	cif_stream_init(cif_dev);
	strscpy(cif_dev->media_dev.model, "cif",
		sizeof(cif_dev->media_dev.model));
	cif_dev->media_dev.dev = &pdev->dev;
	v4l2_dev = &cif_dev->v4l2_dev;
	v4l2_dev->mdev = &cif_dev->media_dev;
	strscpy(v4l2_dev->name, "rockchip-cif", sizeof(v4l2_dev->name));

	ret = v4l2_device_register(cif_dev->dev, &cif_dev->v4l2_dev);
	if (ret < 0)
		return ret;

	media_device_init(&cif_dev->media_dev);

	ret = media_device_register(&cif_dev->media_dev);
	if (ret < 0)
		goto err_unreg_v4l2_dev;

	/* Create & register platform subdev. */
	ret = cif_register_stream_vdev(cif_dev);
	if (ret < 0)
		goto err_unreg_media_dev;

	ret = cif_subdev_notifier(cif_dev);
	if (ret < 0) {
		v4l2_err(&cif_dev->v4l2_dev,
			 "Failed to register subdev notifier(%d)\n", ret);
		goto err_unreg_stream_vdev;
	}

	cif_set_default_format(cif_dev);
	pm_runtime_enable(&pdev->dev);

	return 0;

err_unreg_stream_vdev:
	cif_unregister_stream_vdev(cif_dev);
err_unreg_media_dev:
	media_device_unregister(&cif_dev->media_dev);
err_unreg_v4l2_dev:
	v4l2_device_unregister(&cif_dev->v4l2_dev);
	return ret;
}

static int cif_plat_remove(struct platform_device *pdev)
{
	struct cif_device *cif_dev = platform_get_drvdata(pdev);

	pm_runtime_disable(&pdev->dev);

	media_device_unregister(&cif_dev->media_dev);
	v4l2_device_unregister(&cif_dev->v4l2_dev);
	cif_unregister_stream_vdev(cif_dev);

	return 0;
}

static int __maybe_unused cif_runtime_suspend(struct device *dev)
{
	struct cif_device *cif_dev = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(cif_dev->match_data->clks_num,
				   cif_dev->match_data->clks);

	return pinctrl_pm_select_sleep_state(dev);
}

static int __maybe_unused cif_runtime_resume(struct device *dev)
{
	struct cif_device *cif_dev = dev_get_drvdata(dev);
	int ret;

	ret = pinctrl_pm_select_default_state(dev);
	if (ret < 0)
		return ret;

	return clk_bulk_prepare_enable(cif_dev->match_data->clks_num,
				       cif_dev->match_data->clks);
}

static const struct dev_pm_ops cif_plat_pm_ops = {
	.runtime_suspend = cif_runtime_suspend,
	.runtime_resume	 = cif_runtime_resume,
};

static struct platform_driver cif_plat_drv = {
	.driver = {
		   .name = CIF_DRIVER_NAME,
		   .of_match_table = cif_plat_of_match,
		   .pm = &cif_plat_pm_ops,
	},
	.probe = cif_plat_probe,
	.remove = cif_plat_remove,
};
module_platform_driver(cif_plat_drv);

MODULE_AUTHOR("Rockchip Camera/ISP team");
MODULE_DESCRIPTION("Rockchip CIF platform driver");
MODULE_LICENSE("GPL");
