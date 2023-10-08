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

#define WMI_INSPUR_POWERMODE_EVENT_GUID "854FA5AC-58C7-451D-AAB1-57D6F4E6DDD4"
#define WMI_INSPUR_POWERMODE_BIOS_GUID "596C31E3-332D-43C9-AEE9-585493284F5D"

enum inspur_wmi_method_ids {
	INSPUR_WMI_GET_POWERMODE = 0x02,
	INSPUR_WMI_SET_POWERMODE = 0x03,
};

struct inspur_wmi_priv {
	struct input_dev *idev;
};

static int inspur_wmi_perform_query(char *guid,
		enum inspur_wmi_method_ids query_id,
		void *buffer, size_t insize, size_t outsize)
{
	union acpi_object *obj;
	int ret = 0;
	u32 wmi_outsize;
	struct acpi_buffer input = { insize, buffer};
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };

	wmi_evaluate_method(guid, 0, query_id, &input, &output);

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

	wmi_outsize = min_t(size_t, outsize, obj->buffer.length);
	memcpy(buffer, obj->buffer.pointer, wmi_outsize);

out_free:
	kfree(obj);
	return ret;
}

static ssize_t powermode_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	int ret, mode;

	ret = kstrtoint(buf, 0, &mode);
	if (ret)
		return ret;

	inspur_wmi_perform_query(WMI_INSPUR_POWERMODE_BIOS_GUID,
			INSPUR_WMI_SET_POWERMODE,
			&mode, sizeof(mode), sizeof(mode));

	return count;
}


static ssize_t powermode_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	int mode = 0;
	u8 ret;
	u8 *ret_code;

	inspur_wmi_perform_query(WMI_INSPUR_POWERMODE_BIOS_GUID,
			INSPUR_WMI_GET_POWERMODE,
			&mode, sizeof(mode), sizeof(mode));
	/*
	 *Byte [0]: Return code, 0x0 No error, 0x01 Error
	 *Byte [1]: Power Mode
	 */
	ret_code = (u8 *)(&mode);
	ret = ret_code[1];

	return sprintf(buf, "%u\n", ret);
}

DEVICE_ATTR_RW(powermode);

static struct attribute *inspur_wmi_attrs[] = {
	&dev_attr_powermode.attr,
	NULL,
};

static const struct attribute_group inspur_wmi_group = {
	.attrs = inspur_wmi_attrs,
};

const struct attribute_group *inspur_wmi_groups[] = {
	&inspur_wmi_group,
	NULL,
};

static void inspur_wmi_notify(struct wmi_device *wdev,
		union acpi_object *obj)
{
	//to do
}

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
	int err;

	priv = devm_kzalloc(&wdev->dev, sizeof(struct inspur_wmi_priv),
			GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev_set_drvdata(&wdev->dev, priv);

	err = inspur_wmi_input_setup(wdev);
	return err;
}

static void inspur_wmi_remove(struct wmi_device *wdev)
{
	struct inspur_wmi_priv *priv = dev_get_drvdata(&wdev->dev);

	input_unregister_device(priv->idev);
}

static const struct wmi_device_id inspur_wmi_id_table[] = {
	{ .guid_string = WMI_INSPUR_POWERMODE_EVENT_GUID },
	{  }
};

static struct wmi_driver inspur_wmi_driver = {
	.driver = {
		.name = "inspur-wmi",
		.dev_groups = inspur_wmi_groups,
	},
	.id_table = inspur_wmi_id_table,
	.probe = inspur_wmi_probe,
	.notify = inspur_wmi_notify,
	.remove = inspur_wmi_remove,
};

module_wmi_driver(inspur_wmi_driver);

MODULE_DEVICE_TABLE(wmi, inspur_wmi_id_table);
MODULE_AUTHOR("Ai Chao <aichao@kylinos.cn>");
MODULE_DESCRIPTION("Inspur WMI hotkeys");
MODULE_LICENSE("GPL");
