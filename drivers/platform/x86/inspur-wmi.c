// SPDX-License-Identifier: GPL-2.0
/*
 *  Inspur WMI hotkeys
 *
 *  Copyright (C) 2018	      Ai Chao <aichao@kylinos.cn>
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/wmi.h>

#define WMI_INSPUR_POWERMODE_BIOS_GUID "596C31E3-332D-43C9-AEE9-585493284F5D"

enum inspur_wmi_method_ids {
	INSPUR_WMI_GET_POWERMODE = 0x02,
	INSPUR_WMI_SET_POWERMODE = 0x03,
};

struct inspur_wmi_priv {
	struct input_dev *idev;
	struct wmi_device *wdev;
};

static int inspur_wmi_perform_query(struct wmi_device *wdev,
				    enum inspur_wmi_method_ids query_id,
				    void *buffer, size_t insize,
				    size_t outsize)
{
	union acpi_object *obj;
	acpi_status status;
	int ret = 0;
	struct acpi_buffer input = { insize, buffer};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	status = wmidev_evaluate_method(wdev, 0, query_id, &input, &output);
	if (ACPI_FAILURE(status)) {
		dev_err(&wdev->dev, "EC Powermode control failed: %s\n",
			acpi_format_exception(status));
		return -EIO;
	}

	obj = output.pointer;
	if (!obj)
		return -EINVAL;

	if (obj->type != ACPI_TYPE_BUFFER) {
		ret = -EINVAL;
		goto out_free;
	}

	/* Ignore output data of zero size */
	if (!outsize)
		goto out_free;

	if (obj->buffer.length != outsize) {
		ret = -EINVAL;
		goto out_free;
	}

	memcpy(buffer, obj->buffer.pointer, obj->buffer.length);

out_free:
	kfree(obj);
	return ret;
}

/**
 * Set Power Mode to EC RAM. If Power Mode value greater than 0x3,
 * return error
 * Method ID: 0x3
 * Arg: 4 Bytes
 * Byte [0]: Power Mode:
 *         0x0: Balance Mode
 *         0x1: Performance Mode
 *         0x2: Power Saver Mode
 * Return Value: 4 Bytes
 * Byte [0]: Return Code
 *         0x0: No Error
 *         0x1: Error
 */
static ssize_t powermode_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct inspur_wmi_priv *priv = dev_get_drvdata(dev);
	int ret;
	u32 mode;
	u8 *ret_code;

	ret = kstrtoint(buf, 0, &mode);
	if (ret)
		return ret;

	ret = inspur_wmi_perform_query(priv->wdev,
				       INSPUR_WMI_SET_POWERMODE,
				       &mode, sizeof(mode), sizeof(mode));

	if (ret < 0)
		return ret;

	ret_code = (u8 *)(&mode);
	if (ret_code[0])
		return -EBADRQC;

	return count;
}

/**
 * Get Power Mode from EC RAM, If Power Mode value greater than 0x3,
 * return error
 * Method ID: 0x2
 * Return Value: 4 Bytes
 * Byte [0]: Return Code
 *         0x0: No Error
 *         0x1: Error
 * Byte [1]: Power Mode
 *         0x0: Balance Mode
 *         0x1: Performance Mode
 *         0x2: Power Saver Mode
 */
static ssize_t powermode_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct inspur_wmi_priv *priv = dev_get_drvdata(dev);
	u32 mode = 0;
	int ret;
	u8 *ret_code;

	ret = inspur_wmi_perform_query(priv->wdev,
				       INSPUR_WMI_GET_POWERMODE,
				       &mode, sizeof(mode), sizeof(mode));
	if (ret < 0)
		return ret;

	ret_code = (u8 *)(&mode);
	if (ret_code[0])
		return -EBADRQC;

	return sprintf(buf, "%u\n", ret_code[1]);
}

static DEVICE_ATTR_RW(powermode);

static struct attribute *inspur_wmi_attrs[] = {
	&dev_attr_powermode.attr,
	NULL,
};

static const struct attribute_group inspur_wmi_group = {
	.attrs = inspur_wmi_attrs,
};

static const struct attribute_group *inspur_wmi_groups[] = {
	&inspur_wmi_group,
	NULL,
};

static int inspur_wmi_input_setup(struct wmi_device *wdev)
{
	struct inspur_wmi_priv *priv = dev_get_drvdata(&wdev->dev);

	priv->idev = devm_input_allocate_device(&wdev->dev);
	if (!priv->idev)
		return -ENOMEM;

	priv->idev->name = "Inspur WMI hotkeys";
	priv->idev->phys = "wmi/input0";
	priv->idev->id.bustype = BUS_HOST;
	priv->idev->dev.parent = &wdev->dev;

	return input_register_device(priv->idev);
}

static int inspur_wmi_probe(struct wmi_device *wdev, const void *context)
{
	struct inspur_wmi_priv *priv;

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wdev = wdev;
	dev_set_drvdata(&wdev->dev, priv);

	return inspur_wmi_input_setup(wdev);
}

static const struct wmi_device_id inspur_wmi_id_table[] = {
	{ .guid_string = WMI_INSPUR_POWERMODE_BIOS_GUID },
	{  }
};

static struct wmi_driver inspur_wmi_driver = {
	.driver = {
		.name = "inspur-wmi",
		.dev_groups = inspur_wmi_groups,
	},
	.id_table = inspur_wmi_id_table,
	.probe = inspur_wmi_probe,
};

module_wmi_driver(inspur_wmi_driver);

MODULE_DEVICE_TABLE(wmi, inspur_wmi_id_table);
MODULE_AUTHOR("Ai Chao <aichao@kylinos.cn>");
MODULE_DESCRIPTION("Inspur WMI hotkeys");
MODULE_LICENSE("GPL");
