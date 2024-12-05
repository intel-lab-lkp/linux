// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Alienware AlienFX control
 *
 * Copyright (C) 2014 Dell Inc <Dell.Client.Kernel@dell.com>
 */

#include <linux/platform_device.h>
#include <linux/leds.h>
#include <linux/wmi.h>
#include "alienware-wmi.h"

struct wmax_basic_args {
	u8 arg;
};

/*
 * Helpers used for zone control
 */
static int parse_rgb(const char *buf, struct color_platform *colors)
{
	long unsigned int rgb;
	int ret;
	union color_union {
		struct color_platform cp;
		int package;
	} repackager;

	ret = kstrtoul(buf, 16, &rgb);
	if (ret)
		return ret;

	/* RGB triplet notation is 24-bit hexadecimal */
	if (rgb > 0xFFFFFF)
		return -EINVAL;

	repackager.package = rgb & 0x0f0f0f0f;
	pr_debug("alienware-wmi: r: %d g:%d b: %d\n",
		 repackager.cp.red, repackager.cp.green, repackager.cp.blue);
	*colors = repackager.cp;
	return 0;
}

/*
 * Individual RGB zone control
 */
static ssize_t zone_show(struct device *dev, struct device_attribute *attr,
			 char *buf, u8 location)
{
	struct alienfx_priv *priv;
	struct color_platform *colors;

	priv = dev_get_drvdata(dev);
	colors = &priv->colors[location];

	return sprintf(buf, "red: %d, green: %d, blue: %d\n",
		       colors->red, colors->green, colors->blue);

}

static ssize_t zone_set(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count, u8 location)
{
	struct alienfx_priv *priv;
	struct alienfx_platdata *pdata;
	struct color_platform *colors;
	int ret;

	priv = dev_get_drvdata(dev);
	pdata = dev_get_platdata(dev);

	colors = &priv->colors[location];
	ret = parse_rgb(buf, colors);
	if (ret)
		return ret;

	ret = pdata->ops.upd_led(priv, pdata->wdev, location);

	return ret ? ret : count;
}

#define ALIENWARE_ZONE_SHOW_FUNC(_num)					\
	static ssize_t zone0##_num##_show(struct device *dev,		\
					struct device_attribute *attr,	\
					char *buf)			\
	{								\
		return zone_show(dev, attr, buf, _num);			\
	}

#define ALIENWARE_ZONE_STORE_FUNC(_num)					\
	static ssize_t zone0##_num##_store(struct device *dev,		\
					struct device_attribute *attr,	\
					const char *buf, size_t count)	\
	{								\
		return zone_set(dev, attr, buf, count, _num);		\
	}

#define ALIENWARE_ZONE_ATTR(_num)					\
	ALIENWARE_ZONE_SHOW_FUNC(_num)					\
	ALIENWARE_ZONE_STORE_FUNC(_num)					\
	static DEVICE_ATTR_RW(zone0##_num)

ALIENWARE_ZONE_ATTR(0);
ALIENWARE_ZONE_ATTR(1);
ALIENWARE_ZONE_ATTR(2);
ALIENWARE_ZONE_ATTR(3);

/*
 * Lighting control state device attribute (Global)
 */
static ssize_t lighting_control_state_show(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct alienfx_priv *priv;

	priv = dev_get_drvdata(dev);

	if (priv->lighting_control_state == LEGACY_BOOTING)
		return sysfs_emit(buf, "[booting] running suspend\n");
	else if (priv->lighting_control_state == LEGACY_SUSPEND)
		return sysfs_emit(buf, "booting running [suspend]\n");
	return sysfs_emit(buf, "booting [running] suspend\n");
}

static ssize_t lighting_control_state_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct alienfx_priv *priv;
	struct alienfx_platdata *pdata;
	u8 val;

	priv = dev_get_drvdata(dev);
	pdata = dev_get_platdata(dev);

	if (strcmp(buf, "booting\n") == 0)
		val = LEGACY_BOOTING;
	else if (strcmp(buf, "suspend\n") == 0)
		val = LEGACY_SUSPEND;
	else
		val = pdata->running_code;

	priv->lighting_control_state = val;
	pr_debug("alienware-wmi: updated control state to %d\n",
		 priv->lighting_control_state);

	return count;
}

static DEVICE_ATTR_RW(lighting_control_state);

static umode_t zone_attr_visible(struct kobject *kobj,
				 struct attribute *attr, int n)
{
	struct device *dev;
	struct alienfx_platdata *pdata;

	dev = container_of(kobj, struct device, kobj);
	pdata = dev_get_platdata(dev);

	return n < pdata->num_zones + 1 ? 0644 : 0;
}

static bool zone_group_visible(struct kobject *kobj)
{
	struct device *dev;
	struct alienfx_platdata *pdata;

	dev = container_of(kobj, struct device, kobj);
	pdata = dev_get_platdata(dev);

	return pdata->num_zones > 0;
}
DEFINE_SYSFS_GROUP_VISIBLE(zone);

static struct attribute *zone_attrs[] = {
	&dev_attr_lighting_control_state.attr,
	&dev_attr_zone00.attr,
	&dev_attr_zone01.attr,
	&dev_attr_zone02.attr,
	&dev_attr_zone03.attr,
	NULL
};

static struct attribute_group zone_attribute_group = {
	.name = "rgb_zones",
	.is_visible = SYSFS_GROUP_VISIBLE(zone),
	.attrs = zone_attrs,
};

/*
 * LED Brightness (Global)
 */
static void global_led_set(struct led_classdev *led_cdev,
			   enum led_brightness brightness)
{
	struct alienfx_priv *priv;
	struct alienfx_platdata *pdata;
	int ret;

	priv = container_of(led_cdev, struct alienfx_priv, global_led);
	pdata = dev_get_platdata(&priv->pdev->dev);

	priv->global_brightness = brightness;

	ret = pdata->ops.upd_brightness(priv, pdata->wdev, brightness);
	if (ret)
		pr_err("LED brightness update failed\n");
}

static enum led_brightness global_led_get(struct led_classdev *led_cdev)
{
	struct alienfx_priv *priv;

	priv = container_of(led_cdev, struct alienfx_priv, global_led);

	return priv->global_brightness;
}

/*
 *	The HDMI mux sysfs node indicates the status of the HDMI input mux.
 *	It can toggle between standard system GPU output and HDMI input.
 */
static ssize_t show_hdmi_cable(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct alienfx_platdata *pdata;
	acpi_status status;
	u32 out_data;
	struct wmax_basic_args in_args = {
		.arg = 0,
	};

	pdata = dev_get_platdata(dev);

	status = alienware_wmi_command(pdata->wdev, WMAX_METHOD_HDMI_CABLE,
				       &in_args, sizeof(in_args), &out_data);

	if (ACPI_SUCCESS(status)) {
		if (out_data == 0)
			return sysfs_emit(buf, "[unconnected] connected unknown\n");
		else if (out_data == 1)
			return sysfs_emit(buf, "unconnected [connected] unknown\n");
	}
	pr_err("alienware-wmi: unknown HDMI cable status: %d\n", status);
	return sysfs_emit(buf, "unconnected connected [unknown]\n");
}

static ssize_t show_hdmi_source(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct alienfx_platdata *pdata;
	acpi_status status;
	u32 out_data;
	struct wmax_basic_args in_args = {
		.arg = 0,
	};

	pdata = dev_get_platdata(dev);

	status = alienware_wmi_command(pdata->wdev, WMAX_METHOD_HDMI_STATUS,
				       &in_args, sizeof(in_args), &out_data);

	if (ACPI_SUCCESS(status)) {
		if (out_data == 1)
			return sysfs_emit(buf, "[input] gpu unknown\n");
		else if (out_data == 2)
			return sysfs_emit(buf, "input [gpu] unknown\n");
	}
	pr_err("alienware-wmi: unknown HDMI source status: %u\n", status);
	return sysfs_emit(buf, "input gpu [unknown]\n");
}

static ssize_t toggle_hdmi_source(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct alienfx_platdata *pdata;
	acpi_status status;
	struct wmax_basic_args args;

	pdata = dev_get_platdata(dev);

	if (strcmp(buf, "gpu\n") == 0)
		args.arg = 1;
	else if (strcmp(buf, "input\n") == 0)
		args.arg = 2;
	else
		args.arg = 3;
	pr_debug("alienware-wmi: setting hdmi to %d : %s", args.arg, buf);

	status = alienware_wmi_command(pdata->wdev, WMAX_METHOD_HDMI_SOURCE,
				       &args, sizeof(args), NULL);

	if (ACPI_FAILURE(status))
		pr_err("alienware-wmi: HDMI toggle failed: results: %u\n",
		       status);
	return count;
}

static DEVICE_ATTR(cable, S_IRUGO, show_hdmi_cable, NULL);
static DEVICE_ATTR(source, S_IRUGO | S_IWUSR, show_hdmi_source,
		   toggle_hdmi_source);

static bool hdmi_group_visible(struct kobject *kobj)
{
	struct device *dev;
	struct alienfx_platdata *pdata;

	dev = container_of(kobj, struct device, kobj);
	pdata = dev_get_platdata(dev);

	return pdata->hdmi_mux;
}
DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(hdmi);

static struct attribute *hdmi_attrs[] = {
	&dev_attr_cable.attr,
	&dev_attr_source.attr,
	NULL,
};

static const struct attribute_group hdmi_attribute_group = {
	.name = "hdmi",
	.is_visible = SYSFS_GROUP_VISIBLE(hdmi),
	.attrs = hdmi_attrs,
};

/*
 * Alienware GFX amplifier support
 * - Currently supports reading cable status
 * - Leaving expansion room to possibly support dock/undock events later
 */
static ssize_t show_amplifier_status(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct alienfx_platdata *pdata;
	acpi_status status;
	u32 out_data;
	struct wmax_basic_args in_args = {
		.arg = 0,
	};

	pdata = dev_get_platdata(dev);

	status = alienware_wmi_command(pdata->wdev, WMAX_METHOD_AMPLIFIER_CABLE,
				       &in_args, sizeof(in_args), &out_data);
	if (ACPI_SUCCESS(status)) {
		if (out_data == 0)
			return sysfs_emit(buf, "[unconnected] connected unknown\n");
		else if (out_data == 1)
			return sysfs_emit(buf, "unconnected [connected] unknown\n");
	}
	pr_err("alienware-wmi: unknown amplifier cable status: %d\n", status);
	return sysfs_emit(buf, "unconnected connected [unknown]\n");
}

static DEVICE_ATTR(status, S_IRUGO, show_amplifier_status, NULL);

static bool amplifier_group_visible(struct kobject *kobj)
{
	struct device *dev;
	struct alienfx_platdata *pdata;

	dev = container_of(kobj, struct device, kobj);
	pdata = dev_get_platdata(dev);

	return pdata->amplifier;
}
DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(amplifier);

static struct attribute *amplifier_attrs[] = {
	&dev_attr_status.attr,
	NULL,
};

static const struct attribute_group amplifier_attribute_group = {
	.name = "amplifier",
	.is_visible = SYSFS_GROUP_VISIBLE(amplifier),
	.attrs = amplifier_attrs,
};

/*
 * Deep Sleep Control support
 * - Modifies BIOS setting for deep sleep control allowing extra wakeup events
 */
static ssize_t show_deepsleep_status(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct alienfx_platdata *pdata;
	acpi_status status;
	u32 out_data;
	struct wmax_basic_args in_args = {
		.arg = 0,
	};

	pdata = dev_get_platdata(dev);

	status = alienware_wmi_command(pdata->wdev, WMAX_METHOD_DEEP_SLEEP_STATUS,
				       &in_args, sizeof(in_args), &out_data);
	if (ACPI_SUCCESS(status)) {
		if (out_data == 0)
			return sysfs_emit(buf, "[disabled] s5 s5_s4\n");
		else if (out_data == 1)
			return sysfs_emit(buf, "disabled [s5] s5_s4\n");
		else if (out_data == 2)
			return sysfs_emit(buf, "disabled s5 [s5_s4]\n");
	}
	pr_err("alienware-wmi: unknown deep sleep status: %d\n", status);
	return sysfs_emit(buf, "disabled s5 s5_s4 [unknown]\n");
}

static ssize_t toggle_deepsleep(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct alienfx_platdata *pdata;
	acpi_status status;
	struct wmax_basic_args args;

	pdata = dev_get_platdata(dev);

	if (strcmp(buf, "disabled\n") == 0)
		args.arg = 0;
	else if (strcmp(buf, "s5\n") == 0)
		args.arg = 1;
	else
		args.arg = 2;
	pr_debug("alienware-wmi: setting deep sleep to %d : %s", args.arg, buf);

	status = alienware_wmi_command(pdata->wdev, WMAX_METHOD_DEEP_SLEEP_CONTROL,
				       &args, sizeof(args), NULL);

	if (ACPI_FAILURE(status))
		pr_err("alienware-wmi: deep sleep control failed: results: %u\n",
			status);
	return count;
}

static DEVICE_ATTR(deepsleep, S_IRUGO | S_IWUSR, show_deepsleep_status, toggle_deepsleep);

static bool deepsleep_group_visible(struct kobject *kobj)
{
	struct device *dev;
	struct alienfx_platdata *pdata;

	dev = container_of(kobj, struct device, kobj);
	pdata = dev_get_platdata(dev);

	return pdata->deepslp;
}
DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(deepsleep);

static struct attribute *deepsleep_attrs[] = {
	&dev_attr_deepsleep.attr,
	NULL,
};

static const struct attribute_group deepsleep_attribute_group = {
	.name = "deepsleep",
	.is_visible = SYSFS_GROUP_VISIBLE(deepsleep),
	.attrs = deepsleep_attrs,
};

/*
 * Platform Driver
 */
static int alienfx_probe(struct platform_device *pdev)
{
	struct alienfx_priv *priv;
	struct alienfx_platdata *pdata;
	struct led_classdev *leds;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	platform_set_drvdata(pdev, priv);

	priv->pdev = pdev;

	pdata = dev_get_platdata(&pdev->dev);

	priv->lighting_control_state = pdata->running_code;

	leds = &priv->global_led;
	leds->name = "alienware::global_brightness";
	leds->brightness_set = global_led_set;
	leds->brightness_get = global_led_get;
	leds->max_brightness = 0x0F;

	priv->global_brightness = priv->global_led.max_brightness;

	return devm_led_classdev_register(&pdev->dev, &priv->global_led);
}

static const struct attribute_group *alienfx_groups[] = {
	&zone_attribute_group,
	&hdmi_attribute_group,
	&amplifier_attribute_group,
	&deepsleep_attribute_group,
	NULL
};

static struct platform_driver platform_driver = {
	.driver = {
		.name = "alienware-wmi",
		.dev_groups = alienfx_groups,
	},
	.probe = alienfx_probe,
};

int alienfx_wmi_init(struct alienfx_platdata *pdata)
{
	struct platform_device *pdev;

	pdev = platform_create_bundle(&platform_driver, alienfx_probe, NULL, 0,
				      pdata, sizeof(*pdata));

	dev_set_drvdata(&pdata->wdev->dev, pdev);

	return PTR_ERR_OR_ZERO(pdev);
}

void alienfx_wmi_exit(struct wmi_device *wdev)
{
	struct platform_device *pdev;

	pdev = dev_get_drvdata(&wdev->dev);

	platform_device_unregister(pdev);
	platform_driver_unregister(&platform_driver);
}
