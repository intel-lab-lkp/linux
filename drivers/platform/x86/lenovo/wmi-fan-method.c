// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Lenovo Fan Method WMI interface driver.
 *
 * This driver exposes the firmware fan table through HWMON automatic-point
 * attributes on selected Lenovo Legion Go products.
 */

#include <linux/cleanup.h>
#include <linux/component.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/unaligned.h>
#include <linux/wmi.h>

#define LENOVO_FAN_METHOD_GUID "92549549-4BDE-4F06-AC04-CE8BF898DBAA"

#define LENOVO_FAN_METHOD_GET_CURVE	5
#define LENOVO_FAN_METHOD_SET_CURVE	6
#define LENOVO_FAN_CURVE_POINTS		10
#define LENOVO_FAN_CURVE_REPLY_SIZE	88
#define LENOVO_FAN_CURVE_WRITE_SIZE	64

#define LENOVO_FAN_REPLY_SPEED_COUNT_OFFSET	0
#define LENOVO_FAN_REPLY_SPEED_OFFSET		4
#define LENOVO_FAN_REPLY_TEMP_COUNT_OFFSET	44
#define LENOVO_FAN_REPLY_TEMP_OFFSET		48

#define LENOVO_FAN_WRITE_SPEED_COUNT_OFFSET	2
#define LENOVO_FAN_WRITE_SPEED_OFFSET		6
#define LENOVO_FAN_WRITE_TEMP_TYPE_OFFSET	26
#define LENOVO_FAN_WRITE_TEMP_COUNT_OFFSET	27
#define LENOVO_FAN_WRITE_TEMP_OFFSET		31
#define LENOVO_FAN_WRITE_TRAILER_OFFSET		51
#define LENOVO_FAN_WRITE_TRAILER_VALUE_OFFSET	53

#define LENOVO_FAN_WRITE_TEMP_TYPE		1
#define LENOVO_FAN_WRITE_TRAILER		0x5a
#define LENOVO_FAN_WRITE_TRAILER_VALUE		100

static const struct dmi_system_id lwmi_fan_dmi_table[] = {
	{
		.ident = "Lenovo Legion Go 8APU1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Legion Go 8APU1"),
		},
	},
	{
		.ident = "Lenovo Legion Go S 8APU1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Legion Go S 8APU1"),
		},
	},
	{
		.ident = "Lenovo Legion Go S 8ARP1",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Legion Go S 8ARP1"),
		},
	},
	{
		.ident = "Lenovo Legion Go 8ASP2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Legion Go 8ASP2"),
		},
	},
	{
		.ident = "Lenovo Legion Go 8AHP2",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LENOVO"),
			DMI_EXACT_MATCH(DMI_PRODUCT_VERSION, "Legion Go 8AHP2"),
		},
	},
	{}
};

static const u16 lwmi_fan_temperatures[LENOVO_FAN_CURVE_POINTS] = {
	10, 20, 30, 40, 50, 60, 70, 80, 90, 100
};

struct lwmi_fan_method_curve {
	u16 speed[LENOVO_FAN_CURVE_POINTS];
	u16 temperature[LENOVO_FAN_CURVE_POINTS];
};

struct lwmi_fan_method_priv;

struct lwmi_fan_method_attr {
	struct device_attribute dev_attr;
	struct lwmi_fan_method_priv *priv;
	u8 index;
};

struct lwmi_fan_method_priv {
	struct wmi_device *wdev;
	struct mutex lock; /* Serializes all Fan Method calls. */
	struct lwmi_fan_method_attr point_attrs[LENOVO_FAN_CURVE_POINTS * 2];
	struct attribute *attrs[LENOVO_FAN_CURVE_POINTS * 2 + 1];
	struct attribute_group group;
};

static int lwmi_fan_method_get_curve(struct lwmi_fan_method_priv *priv,
				     struct lwmi_fan_method_curve *curve)
{
	u8 input[] = { 1, 1 };
	struct wmi_buffer in = {
		.length = sizeof(input),
		.data = input,
	};
	struct wmi_buffer out = {};
	const u8 *buffer;
	int ret, i;

	ret = wmidev_invoke_method(priv->wdev, 0,
				   LENOVO_FAN_METHOD_GET_CURVE, &in, &out,
				   LENOVO_FAN_CURVE_REPLY_SIZE);
	if (ret)
		return ret;

	buffer = out.data;
	if (get_unaligned_le32(buffer + LENOVO_FAN_REPLY_SPEED_COUNT_OFFSET) !=
	    LENOVO_FAN_CURVE_POINTS ||
	    get_unaligned_le32(buffer + LENOVO_FAN_REPLY_TEMP_COUNT_OFFSET) !=
	    LENOVO_FAN_CURVE_POINTS) {
		ret = -ERANGE;
		goto out_free;
	}

	for (i = 0; i < LENOVO_FAN_CURVE_POINTS; i++) {
		u32 speed = get_unaligned_le32(buffer + LENOVO_FAN_REPLY_SPEED_OFFSET +
					       i * sizeof(u32));
		u32 temperature =
			get_unaligned_le32(buffer + LENOVO_FAN_REPLY_TEMP_OFFSET +
					   i * sizeof(u32));

		if (speed > U8_MAX || temperature != lwmi_fan_temperatures[i]) {
			ret = -ERANGE;
			goto out_free;
		}

		curve->speed[i] = speed;
		curve->temperature[i] = temperature;
	}

out_free:
	kfree(out.data);
	return ret;
}

static int lwmi_fan_method_set_curve(struct lwmi_fan_method_priv *priv,
				     const struct lwmi_fan_method_curve *curve)
{
	u8 buffer[LENOVO_FAN_CURVE_WRITE_SIZE] = { 0xff, 0x01 };
	struct wmi_buffer in = {
		.length = sizeof(buffer),
		.data = buffer,
	};
	int i;

	put_unaligned_le32(LENOVO_FAN_CURVE_POINTS,
			   buffer + LENOVO_FAN_WRITE_SPEED_COUNT_OFFSET);
	for (i = 0; i < LENOVO_FAN_CURVE_POINTS; i++)
		put_unaligned_le16(curve->speed[i],
				   buffer + LENOVO_FAN_WRITE_SPEED_OFFSET +
				   i * sizeof(u16));

	buffer[LENOVO_FAN_WRITE_TEMP_TYPE_OFFSET] = LENOVO_FAN_WRITE_TEMP_TYPE;
	put_unaligned_le32(LENOVO_FAN_CURVE_POINTS,
			   buffer + LENOVO_FAN_WRITE_TEMP_COUNT_OFFSET);
	for (i = 0; i < LENOVO_FAN_CURVE_POINTS; i++)
		put_unaligned_le16(curve->temperature[i],
				   buffer + LENOVO_FAN_WRITE_TEMP_OFFSET +
				   i * sizeof(u16));

	buffer[LENOVO_FAN_WRITE_TRAILER_OFFSET] = LENOVO_FAN_WRITE_TRAILER;
	put_unaligned_le16(LENOVO_FAN_WRITE_TRAILER_VALUE,
			   buffer + LENOVO_FAN_WRITE_TRAILER_VALUE_OFFSET);

	return wmidev_invoke_procedure(priv->wdev, 0,
				       LENOVO_FAN_METHOD_SET_CURVE, &in);
}

static ssize_t lwmi_fan_method_pwm_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct lwmi_fan_method_attr *point_attr =
		container_of(attr, struct lwmi_fan_method_attr, dev_attr);
	struct lwmi_fan_method_priv *priv = point_attr->priv;
	struct lwmi_fan_method_curve curve;
	int ret;

	guard(mutex)(&priv->lock);

	ret = lwmi_fan_method_get_curve(priv, &curve);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", curve.speed[point_attr->index]);
}

static ssize_t lwmi_fan_method_pwm_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct lwmi_fan_method_attr *point_attr =
		container_of(attr, struct lwmi_fan_method_attr, dev_attr);
	struct lwmi_fan_method_priv *priv = point_attr->priv;
	struct lwmi_fan_method_curve curve;
	unsigned long pwm;
	int ret;

	ret = kstrtoul(buf, 10, &pwm);
	if (ret)
		return ret;
	if (pwm > U8_MAX)
		return -EINVAL;

	guard(mutex)(&priv->lock);

	ret = lwmi_fan_method_get_curve(priv, &curve);
	if (ret)
		return ret;

	curve.speed[point_attr->index] = pwm;
	ret = lwmi_fan_method_set_curve(priv, &curve);

	return ret ? ret : count;
}

static ssize_t lwmi_fan_method_temp_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct lwmi_fan_method_attr *point_attr =
		container_of(attr, struct lwmi_fan_method_attr, dev_attr);

	return sysfs_emit(buf, "%u\n",
			  lwmi_fan_temperatures[point_attr->index] * 1000);
}

#define LWMI_FAN_METHOD_POINT_NAMES(_point) \
	"pwm1_auto_point" #_point "_pwm", \
	"pwm1_auto_point" #_point "_temp"

static const char * const lwmi_fan_method_attr_names[] = {
	LWMI_FAN_METHOD_POINT_NAMES(1),
	LWMI_FAN_METHOD_POINT_NAMES(2),
	LWMI_FAN_METHOD_POINT_NAMES(3),
	LWMI_FAN_METHOD_POINT_NAMES(4),
	LWMI_FAN_METHOD_POINT_NAMES(5),
	LWMI_FAN_METHOD_POINT_NAMES(6),
	LWMI_FAN_METHOD_POINT_NAMES(7),
	LWMI_FAN_METHOD_POINT_NAMES(8),
	LWMI_FAN_METHOD_POINT_NAMES(9),
	LWMI_FAN_METHOD_POINT_NAMES(10),
};

#undef LWMI_FAN_METHOD_POINT_NAMES

static void lwmi_fan_method_attrs_init(struct lwmi_fan_method_priv *priv)
{
	int i;

	for (i = 0; i < LENOVO_FAN_CURVE_POINTS; i++) {
		struct lwmi_fan_method_attr *pwm = &priv->point_attrs[i * 2];
		struct lwmi_fan_method_attr *temp = &priv->point_attrs[i * 2 + 1];

		sysfs_attr_init(&pwm->dev_attr.attr);
		pwm->dev_attr.attr.name = lwmi_fan_method_attr_names[i * 2];
		pwm->dev_attr.attr.mode = 0644;
		pwm->dev_attr.show = lwmi_fan_method_pwm_show;
		pwm->dev_attr.store = lwmi_fan_method_pwm_store;
		pwm->priv = priv;
		pwm->index = i;
		priv->attrs[i * 2] = &pwm->dev_attr.attr;

		sysfs_attr_init(&temp->dev_attr.attr);
		temp->dev_attr.attr.name = lwmi_fan_method_attr_names[i * 2 + 1];
		temp->dev_attr.attr.mode = 0444;
		temp->dev_attr.show = lwmi_fan_method_temp_show;
		temp->index = i;
		priv->attrs[i * 2 + 1] = &temp->dev_attr.attr;
	}

	priv->group.attrs = priv->attrs;
}

static int lwmi_fan_method_master_bind(struct device *dev)
{
	struct lwmi_fan_method_priv *priv = dev_get_drvdata(dev);

	return component_bind_all(dev, &priv->group);
}

static void lwmi_fan_method_master_unbind(struct device *dev)
{
	component_unbind_all(dev, NULL);
}

static const struct component_master_ops lwmi_fan_method_master_ops = {
	.bind = lwmi_fan_method_master_bind,
	.unbind = lwmi_fan_method_master_unbind,
};

static int lwmi_fan_method_component_compare(struct device *dev, void *data)
{
	struct device *master = data;

	return dev->driver &&
		!strcmp(dev->driver->name, "lenovo_wmi_other") &&
		dev->parent == master->parent;
}

static int lwmi_fan_method_probe(struct wmi_device *wdev, const void *context)
{
	struct component_match *master_match = NULL;
	struct lwmi_fan_method_priv *priv;

	if (!dmi_check_system(lwmi_fan_dmi_table))
		return -ENODEV;

	priv = devm_kzalloc(&wdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->wdev = wdev;
	mutex_init(&priv->lock);
	lwmi_fan_method_attrs_init(priv);
	dev_set_drvdata(&wdev->dev, priv);

	component_match_add(&wdev->dev, &master_match,
			    lwmi_fan_method_component_compare, &wdev->dev);
	if (IS_ERR(master_match))
		return PTR_ERR(master_match);

	return component_master_add_with_match(&wdev->dev,
					       &lwmi_fan_method_master_ops,
					       master_match);
}

static void lwmi_fan_method_remove(struct wmi_device *wdev)
{
	component_master_del(&wdev->dev, &lwmi_fan_method_master_ops);
}

static const struct wmi_device_id lwmi_fan_method_id_table[] = {
	{ LENOVO_FAN_METHOD_GUID, NULL },
	{}
};

static struct wmi_driver lwmi_fan_method_driver = {
	.driver = {
		.name = "lenovo_wmi_fan_method",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.id_table = lwmi_fan_method_id_table,
	.probe = lwmi_fan_method_probe,
	.remove = lwmi_fan_method_remove,
	.no_singleton = true,
};

MODULE_DEVICE_TABLE(wmi, lwmi_fan_method_id_table);
module_wmi_driver(lwmi_fan_method_driver);

MODULE_AUTHOR("Aditya Dash <mradityadash@gmail.com>");
MODULE_DESCRIPTION("Lenovo Fan Method WMI Driver");
MODULE_LICENSE("GPL");
