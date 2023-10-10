// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VIP Camera Interface Driver
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

#include "dev.h"
#include "regs.h"

#define RK_VIP_VERNO_LEN	10

struct vip_match_data {
	int chip_id;
	const char * const *clks;
	const char * const *rsts;
	int clks_num;
	int rsts_num;
};

static int rk_vip_create_links(struct rk_vip_device *vip_dev)
{
	struct v4l2_subdev *sd = vip_dev->sensor.sd;
	int ret;

	ret = media_create_pad_link(&sd->entity, 0,
				    &vip_dev->stream.vdev.entity, 0,
				    MEDIA_LNK_FL_ENABLED);
	if (ret) {
		dev_err(vip_dev->dev, "failed to create link");
		return ret;
	}

	return 0;
}

static int subdev_notifier_complete(struct v4l2_async_notifier *notifier)
{
	struct rk_vip_device *vip_dev;
	int ret;

	vip_dev = container_of(notifier, struct rk_vip_device, notifier);

	mutex_lock(&vip_dev->media_dev.graph_mutex);

	ret = v4l2_device_register_subdev_nodes(&vip_dev->v4l2_dev);
	if (ret < 0)
		goto unlock;

	ret = rk_vip_create_links(vip_dev);
	if (ret < 0)
		goto unlock;

unlock:
	mutex_unlock(&vip_dev->media_dev.graph_mutex);
	return ret;
}

static int subdev_notifier_bound(struct v4l2_async_notifier *notifier,
				 struct v4l2_subdev *subdev,
				 struct v4l2_async_connection *asd)
{
	struct rk_vip_device *vip_dev = container_of(notifier,
					struct rk_vip_device, notifier);

	int pad;

	vip_dev->sensor.sd = subdev;
	pad = media_entity_get_fwnode_pad(&subdev->entity, subdev->fwnode,
					  MEDIA_PAD_FL_SOURCE);
	if (pad < 0)
		return pad;

	vip_dev->sensor.pad = pad;

	return 0;
}

static const struct v4l2_async_notifier_operations subdev_notifier_ops = {
	.bound = subdev_notifier_bound,
	.complete = subdev_notifier_complete,
};

static int vip_subdev_notifier(struct rk_vip_device *vip_dev)
{
	struct v4l2_async_notifier *ntf = &vip_dev->notifier;
	struct device *dev = vip_dev->dev;
	struct v4l2_async_connection *asd;
	struct v4l2_fwnode_endpoint vep = {
		.bus_type = V4L2_MBUS_PARALLEL,
	};
	struct fwnode_handle *ep;
	int ret;

	v4l2_async_nf_init(ntf, &vip_dev->v4l2_dev);

	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep)
		return -EINVAL;

	ret = v4l2_fwnode_endpoint_parse(ep, &vep);
	if (ret)
		return ret;

	asd = v4l2_async_nf_add_fwnode_remote(ntf, ep,
					      struct v4l2_async_connection);
	if (IS_ERR(asd)) {
		ret = PTR_ERR(asd);
		return ret;
	}

	ntf->ops = &subdev_notifier_ops;

	fwnode_handle_put(ep);

	ret = v4l2_async_nf_register(ntf);
	return ret;
}

static int rk_vip_register_platform_subdevs(struct rk_vip_device *vip_dev)
{
	int ret;

	ret = rk_vip_register_stream_vdev(vip_dev);
	if (ret < 0)
		return ret;

	ret = vip_subdev_notifier(vip_dev);
	if (ret < 0) {
		v4l2_err(&vip_dev->v4l2_dev,
			 "Failed to register subdev notifier(%d)\n", ret);
		rk_vip_unregister_stream_vdev(vip_dev);
	}

	return 0;
}

static const char * const px30_vip_clks[] = {
	"aclk",
	"hclk",
	"pclk",
};

static const struct vip_match_data px30_vip_match_data = {
	.chip_id = CHIP_PX30_VIP,
	.clks = px30_vip_clks,
	.clks_num = ARRAY_SIZE(px30_vip_clks),
};

static const struct of_device_id rk_vip_plat_of_match[] = {
	{
		.compatible = "rockchip,px30-vip",
		.data = &px30_vip_match_data,
	},
	{},
};

void rk_vip_soft_reset(struct rk_vip_device *vip_dev)
{
	reset_control_assert(vip_dev->vip_rst);

	udelay(5);

	reset_control_deassert(vip_dev->vip_rst);
}

static int rk_vip_get_resource(struct platform_device *pdev,
			       struct rk_vip_device *vip_dev)
{
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(&pdev->dev,
			"Unable to allocate resources for device\n");
		return -ENODEV;
	}

	vip_dev->base_addr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(vip_dev->base_addr))
		return PTR_ERR(vip_dev->base_addr);

	return 0;
}

static int rk_vip_plat_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct device_node *node = pdev->dev.of_node;
	struct device *dev = &pdev->dev;
	struct v4l2_device *v4l2_dev;
	struct rk_vip_device *vip_dev;
	const struct vip_match_data *data;
	int i, ret, irq;

	match = of_match_node(rk_vip_plat_of_match, node);
	if (!match)
		return -ENODEV;

	vip_dev = devm_kzalloc(dev, sizeof(*vip_dev), GFP_KERNEL);
	if (!vip_dev)
		return -ENOMEM;

	dev_set_drvdata(dev, vip_dev);
	vip_dev->dev = dev;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(dev, irq, rk_vip_irq_pingpong, 0,
			       dev_driver_string(dev), dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "request irq failed\n");

	vip_dev->irq = irq;
	data = match->data;
	vip_dev->chip_id = data->chip_id;

	ret = rk_vip_get_resource(pdev, vip_dev);
	if (ret)
		return ret;

	for (i = 0; i < data->clks_num; i++)
		vip_dev->clks[i].id = data->clks[i];

	vip_dev->num_clk = data->clks_num;

	ret = devm_clk_bulk_get(dev, vip_dev->num_clk, vip_dev->clks);
	if (ret)
		return ret;

	vip_dev->vip_rst = devm_reset_control_array_get(dev, false, false);
	if (IS_ERR(vip_dev->vip_rst))
		return PTR_ERR(vip_dev->vip_rst);

	/* Initialize the stream */
	rk_vip_stream_init(vip_dev);
	strscpy(vip_dev->media_dev.model, "rk_vip",
		sizeof(vip_dev->media_dev.model));
	vip_dev->media_dev.dev = &pdev->dev;
	v4l2_dev = &vip_dev->v4l2_dev;
	v4l2_dev->mdev = &vip_dev->media_dev;
	strscpy(v4l2_dev->name, "rk_vip", sizeof(v4l2_dev->name));

	ret = v4l2_device_register(vip_dev->dev, &vip_dev->v4l2_dev);
	if (ret < 0)
		return ret;

	media_device_init(&vip_dev->media_dev);

	ret = media_device_register(&vip_dev->media_dev);
	if (ret < 0) {
		v4l2_err(v4l2_dev, "Failed to register media device: %d\n",
			 ret);
		goto err_unreg_v4l2_dev;
	}

	/* create & register platform subdev (from of_node) */
	ret = rk_vip_register_platform_subdevs(vip_dev);
	if (ret < 0)
		goto err_unreg_media_dev;

	vip_dev->sensor.std = V4L2_STD_NTSC;
	rk_vip_set_default_format(vip_dev);
	pm_runtime_enable(&pdev->dev);

	return 0;

err_unreg_media_dev:
	media_device_unregister(&vip_dev->media_dev);
err_unreg_v4l2_dev:
	v4l2_device_unregister(&vip_dev->v4l2_dev);
	return ret;
}

static int rk_vip_plat_remove(struct platform_device *pdev)
{
	struct rk_vip_device *vip_dev = platform_get_drvdata(pdev);

	pm_runtime_disable(&pdev->dev);

	media_device_unregister(&vip_dev->media_dev);
	v4l2_device_unregister(&vip_dev->v4l2_dev);
	rk_vip_unregister_stream_vdev(vip_dev);

	return 0;
}

static int __maybe_unused rk_vip_runtime_suspend(struct device *dev)
{
	struct rk_vip_device *vip_dev = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(vip_dev->num_clk, vip_dev->clks);

	return pinctrl_pm_select_sleep_state(dev);
}

static int __maybe_unused rk_vip_runtime_resume(struct device *dev)
{
	struct rk_vip_device *vip_dev = dev_get_drvdata(dev);
	int ret;

	ret = pinctrl_pm_select_default_state(dev);
	if (ret < 0)
		return ret;

	return clk_bulk_prepare_enable(vip_dev->num_clk, vip_dev->clks);
}

static const struct dev_pm_ops rk_vip_plat_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(rk_vip_runtime_suspend, rk_vip_runtime_resume, NULL)
};

static struct platform_driver rk_vip_plat_drv = {
	.driver = {
		   .name = VIP_DRIVER_NAME,
		   .of_match_table = rk_vip_plat_of_match,
		   .pm = &rk_vip_plat_pm_ops,
	},
	.probe = rk_vip_plat_probe,
	.remove = rk_vip_plat_remove,
};
module_platform_driver(rk_vip_plat_drv);

MODULE_AUTHOR("Rockchip Camera/ISP team");
MODULE_DESCRIPTION("Rockchip VIP platform driver");
MODULE_LICENSE("GPL");
