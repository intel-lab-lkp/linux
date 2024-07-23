// SPDX-License-Identifier: GPL-2.0+

/*
 * Authors:
 *      Sui Jingfeng <sui.jingfeng@linux.dev>
 */

#include <linux/delay.h>
#include <linux/component.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "lsdc_drv.h"
#include "lsdc_output.h"

static struct property_entry ls7a1000_output_port_properties[LSDC_NUM_CRTC][3] = {
	{
		PROPERTY_ENTRY_STRING("type", "DVO"),
		PROPERTY_ENTRY_U32("reg", 0),
		{ },
	},
	{
		PROPERTY_ENTRY_STRING("type", "DVO"),
		PROPERTY_ENTRY_U32("reg", 1),
		{ },
	},
};

static struct property_entry ls7a2000_output_port_properties[LSDC_NUM_CRTC][3] = {
	{
		PROPERTY_ENTRY_STRING("type", "HDMI-or-VGA"),
		PROPERTY_ENTRY_U32("reg", 0),
		{ },
	},
	{
		PROPERTY_ENTRY_STRING("type", "HDMI"),
		PROPERTY_ENTRY_U32("reg", 1),
		{ },
	},
};

static struct property_entry *
lsdc_output_get_property_entry(const struct lsdc_desc *descp,
			       unsigned int index)
{
	if (index >= LSDC_NUM_CRTC)
		return NULL;

	switch (to_loongson_gfx(descp)->chip_id) {
	case CHIP_LS7A1000:
		return &ls7a1000_output_port_properties[index][0];
	case CHIP_LS7A2000:
		return &ls7a2000_output_port_properties[index][0];
	default:
		break;
	};

	return NULL;
}

static void lsdc_output_postfini(void *data)
{
	struct platform_device *agent = data;

	platform_device_unregister(agent);
}

int lsdc_output_preinit(struct device *parent, const struct lsdc_desc *descp)
{
	unsigned int i;

	for (i = 0; i < descp->num_of_crtc; ++i) {
		struct platform_device_info devinfo = {};
		struct platform_device *agent;
		int ret;

		devinfo.parent = parent;
		devinfo.name = "loongson.output.agent";
		devinfo.id = i;
		devinfo.properties = lsdc_output_get_property_entry(descp, i);

		agent = platform_device_register_full(&devinfo);
		if (IS_ERR(agent)) {
			dev_err(parent, "failed to register output agent\n");
			return PTR_ERR(agent);
		}

		ret = devm_add_action_or_reset(parent, lsdc_output_postfini, agent);
		if (ret)
			return ret;
	}

	return 0;
}

static int lsdc_output_bind(struct device *dev,
			    struct device *master,
			    void *data)
{
	struct drm_device *drm = data;
	struct lsdc_device *ldev = dev_get_drvdata(dev->parent);
	const struct lsdc_kms_funcs *lkmsfun = ldev->descp->funcs;
	struct lsdc_output *output = dev_get_drvdata(dev);
	unsigned int pipe = output->pipe;
	int ret;

	ret = lkmsfun->output_init(drm, output, ldev->i2c[pipe], pipe);
	if (ret)
		return ret;

	ldev->dispipe[pipe].output = output;
	ldev->num_output++;

	return 0;
}

static void lsdc_output_unbind(struct device *dev,
			       struct device *master,
			       void *data)
{
	struct lsdc_device *ldev = dev_get_drvdata(dev->parent);
	struct lsdc_output *output = dev_get_drvdata(dev);
	unsigned int pipe = output->pipe;

	ldev->dispipe[pipe].output = NULL;
}

static const struct component_ops lsdc_output_component_ops = {
	.bind = lsdc_output_bind,
	.unbind = lsdc_output_unbind,
};

static void lsdc_output_destroy(void *data)
{
	struct lsdc_output *output = (struct lsdc_output *)data;

	kfree(output);
}

static int lsdc_output_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *parent = dev->parent;
	struct lsdc_output *output;
	const char *type;
	int ret;

	output = kzalloc(sizeof(*output), GFP_KERNEL);
	if (!output)
		return -ENOMEM;

	ret = devm_add_action_or_reset(parent, lsdc_output_destroy, output);
	if (ret)
		return ret;

	output->dev = dev;
	dev_set_drvdata(dev, output);

	ret = device_property_read_u32(dev, "reg", &output->pipe);
	if (ret)
		return ret;

	ret = device_property_read_string(dev, "type", &type);
	if (ret)
		return ret;

	dev_info(parent, "%s probed, type: %s\n", dev_name(dev), type);

	return component_add(dev, &lsdc_output_component_ops);
}

static void lsdc_output_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &lsdc_output_component_ops);
}

struct platform_driver lsdc_output_platform_driver = {
	.driver = {
		.name = "loongson.output.agent",
	},
	.probe = lsdc_output_probe,
	.remove_new = lsdc_output_remove,
};
