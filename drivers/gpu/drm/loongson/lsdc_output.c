// SPDX-License-Identifier: GPL-2.0+

#include <linux/delay.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "lsdc_drv.h"
#include "lsdc_output.h"

static struct lsdc_output_desc ls7a1000_output_port_desc[2] = {
	{
		.pipe = 0,
		.type = "DVO-0",
	},
	{
		.pipe = 1,
		.type = "DVO-1",
	},
};

static struct lsdc_output_desc ls7a2000_output_port_desc[2] = {
	{
		.pipe = 0,
		.type = "HDMI-or-VGA-0",
	},
	{
		.pipe = 1,
		.type = "HDMI-1",
	},
};

int lsdc_output_preinit(struct device *parent,
			const struct lsdc_desc *descp,
			unsigned int index,
			struct platform_device **ppdev)
{
	struct lsdc_output_desc *output_port_info;
	int ret;

	switch (to_loongson_gfx(descp)->chip_id) {
	case CHIP_LS7A1000:
		output_port_info = &ls7a1000_output_port_desc[index];
		break;
	case CHIP_LS7A2000:
		output_port_info = &ls7a2000_output_port_desc[index];
		break;
	default:
		output_port_info = NULL;
		break;
	};

	ret = loongson_create_platform_device(parent,
					      "lsdc-output-port",
					      index,
					      NULL,
					      (void *)output_port_info,
					      ppdev);
	if (ret)
		return ret;

	return 0;
}

/*
 * @dev: pointer to the port@0, port@1, ..., port@n of the dispplay controller
 * @master: pointer to the component master
 * @data: pointer to the drm device control structure
 */
static int lsdc_output_port_bind(struct device *dev,
				 struct device *master,
				 void *data)
{
	struct drm_device *drm = data;
	struct lsdc_device *ldev = to_lsdc(drm);
	const struct lsdc_kms_funcs *kms_funcs = ldev->descp->funcs;
	struct lsdc_output *output = dev_get_drvdata(dev);
	struct lsdc_display_pipe *dispipe;
	unsigned int pipe;
	int ret;

	if (!output->descp)
		return -ENODEV;

	pipe = output->descp->pipe;
	dispipe = &ldev->dispipe[pipe];

	ret = kms_funcs->output_init(drm, output, ldev->i2c[pipe], pipe);
	if (ret)
		return ret;

	dispipe->output = output;

	ldev->num_output++;

	drm_info(drm, "Output port-%d bound, type: %s\n",
		 pipe, output->descp->type);

	return 0;
}

static void lsdc_output_port_unbind(struct device *dev,
				    struct device *master,
				    void *data)
{
	struct drm_device *drm = data;
	struct lsdc_device *ldev = to_lsdc(drm);
	struct lsdc_output *output = dev_get_drvdata(dev);
	unsigned int pipe;

	pipe = output->descp->pipe;
	ldev->dispipe[pipe].output = NULL;
}

static const struct component_ops lsdc_output_port_component_ops = {
	.bind = lsdc_output_port_bind,
	.unbind = lsdc_output_port_unbind,
};

static int lsdc_output_port_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct lsdc_output *output;
	struct lsdc_output_desc *descp;
	int ret;

	descp = *(void **)dev->platform_data;
	if (!descp) {
		dev_err(dev, "No platform specific data for output port\n");
		return -ENODEV;
	}

	output = devm_kzalloc(dev, sizeof(*output), GFP_KERNEL);
	if (!output)
		return -ENOMEM;

	output->dev = dev;
	output->descp = descp;

	ret = component_add(dev, &lsdc_output_port_component_ops);
	if (ret) {
		devm_kfree(dev, output);
		dev_err(dev, "failed to register component: %d\n", ret);
		return ret;
	}

	dev_set_drvdata(dev, output);

	return 0;
}

static void lsdc_output_port_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct lsdc_output *output;

	component_del(dev, &lsdc_output_port_component_ops);

	output = dev_get_drvdata(dev);
	if (output)
		devm_kfree(dev, output);
}

static const struct platform_device_id lsdc_output_port_ids[] = {
	{ .name = "lsdc-output-port" },
	{ },
};

struct platform_driver lsdc_output_port_platform_driver = {
	.driver = {
		.name = "lsdc-output-port",
	},
	.probe = lsdc_output_port_probe,
	.remove_new = lsdc_output_port_remove,
	.id_table = lsdc_output_port_ids,
};
