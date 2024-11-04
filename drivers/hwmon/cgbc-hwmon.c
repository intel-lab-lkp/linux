// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * cgbc-hwmon - Congatec Board Controller hardware monitoring driver
 *
 * Copyright (C) 2024 Thomas Richard <thomas.richard@bootlin.com>
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/hwmon.h>
#include <linux/mfd/cgbc.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#define CGBC_HWMON_TYPE_TEMP	1
#define CGBC_HWMON_TYPE_IN	2
#define CGBC_HWMON_TYPE_FAN	3

#define CGBC_HWMON_CMD_SENSOR		0x77
#define CGBC_HWMON_CMD_SENSOR_DATA_SIZE	0x05

#define CGBC_HWMON_TYPE_MASK	GENMASK(6, 5)
#define CGBC_HWMON_ID_MASK	GENMASK(4, 0)
#define CGBC_HWMON_ACTIVE_BIT	BIT(7)
#define CGBC_HWMON_DATA_HIGH	GENMASK(15, 8)
#define CGBC_HWMON_DATA_LOW	GENMASK(7, 0)

struct cgbc_hwmon_sensor {
	enum hwmon_sensor_types type;
	bool active;
	u8 index;
	const char *label;
};

struct cgbc_hwmon_data {
	struct cgbc_device_data *cgbc;
	u8 nb_sensors;
	struct cgbc_hwmon_sensor *sensors;
};

static const char * const cgbc_hwmon_labels_temp[] = {
	"CPU Temperature",
	"Box Temperature",
	"Ambient Temperature",
	"Board Temperature",
	"Carrier Temperature",
	"Chipset Temperature",
	"Video Temperature",
	"Other Temperature",
	"TOPDIM Temperature",
	"BOTTOMDIM Temperature",
};

static const struct {
	enum hwmon_sensor_types type;
	const char *label;
} cgbc_hwmon_labels_in[] = {
	{ hwmon_in, "CPU Voltage" },
	{ hwmon_in, "DC Runtime Voltage" },
	{ hwmon_in, "DC Standby Voltage" },
	{ hwmon_in, "CMOS Battery Voltage" },
	{ hwmon_in, "Battery Voltage" },
	{ hwmon_in, "AC Voltage" },
	{ hwmon_in, "Other Voltage" },
	{ hwmon_in, "5V Voltage" },
	{ hwmon_in, "5V Standby Voltage" },
	{ hwmon_in, "3V3 Voltage" },
	{ hwmon_in, "3V3 Standby Voltage" },
	{ hwmon_in, "VCore A Voltage" },
	{ hwmon_in, "VCore B Voltage" },
	{ hwmon_in, "12V Voltage" },
	{ hwmon_curr, "DC Current" },
	{ hwmon_curr, "5V Current" },
	{ hwmon_curr, "12V Current" },
};

static const char * const cgbc_hwmon_labels_fan[] = {
	"CPU Fan",
	"Box Fan",
	"Ambient Fan",
	"Chipset Fan",
	"Video Fan",
	"Other Fan",
};

static int cgbc_hwmon_cmd(struct cgbc_device_data *cgbc, u8 id, u8 *data)
{
	u8 cmd[2] = {CGBC_HWMON_CMD_SENSOR, id};

	return cgbc_command(cgbc, cmd, sizeof(cmd), data, CGBC_HWMON_CMD_SENSOR_DATA_SIZE, NULL);
}

static int cgbc_hwmon_probe_sensors(struct device *dev, struct cgbc_hwmon_data *hwmon)
{
	struct cgbc_device_data *cgbc = hwmon->cgbc;
	struct cgbc_hwmon_sensor *sensor = hwmon->sensors;
	u8 data[CGBC_HWMON_CMD_SENSOR_DATA_SIZE], nb_sensors, i;
	int ret;

	ret = cgbc_hwmon_cmd(cgbc, 0, &data[0]);
	if (ret)
		return ret;

	nb_sensors = data[0];

	hwmon->sensors = devm_kzalloc(dev, sizeof(*hwmon->sensors) * nb_sensors, GFP_KERNEL);
	sensor = hwmon->sensors;

	for (i = 0; i < nb_sensors; i++) {
		u8 type, id;

		ret = cgbc_hwmon_cmd(cgbc, i, &data[0]);
		if (ret)
			return ret;

		type = FIELD_GET(CGBC_HWMON_TYPE_MASK, data[1]);
		id = FIELD_GET(CGBC_HWMON_ID_MASK, data[1]) - 1;

		if (type == CGBC_HWMON_TYPE_TEMP && id < ARRAY_SIZE(cgbc_hwmon_labels_temp)) {
			sensor->type = hwmon_temp;
			sensor->label = cgbc_hwmon_labels_temp[id];
		} else if (type == CGBC_HWMON_TYPE_IN && id < ARRAY_SIZE(cgbc_hwmon_labels_in)) {
			/*
			 * The Board Controller doesn't do differences between current and voltage
			 * sensors
			 */
			sensor->type = cgbc_hwmon_labels_in[id].type;
			sensor->label = cgbc_hwmon_labels_in[id].label;
		} else if (type == CGBC_HWMON_TYPE_FAN && id < ARRAY_SIZE(cgbc_hwmon_labels_fan)) {
			sensor->type = hwmon_fan;
			sensor->label = cgbc_hwmon_labels_fan[id];
		} else {
			dev_warn(dev, "Board Controller returned an unknown sensor (type=%d, id=%d), ignore it",
				 type, id);
			continue;
		}

		sensor->active = FIELD_GET(CGBC_HWMON_ACTIVE_BIT, data[1]);
		sensor->index = i;
		sensor++;
		hwmon->nb_sensors++;
	}

	return 0;
}

static struct cgbc_hwmon_sensor *cgbc_hwmon_find_sensor(struct cgbc_hwmon_data *hwmon,
							enum hwmon_sensor_types type, int channel)
{
	struct cgbc_hwmon_sensor *sensor = NULL;
	int i, cnt = 0;

	for (i = 0; i < hwmon->nb_sensors; i++) {
		if (hwmon->sensors[i].type == type && cnt++ == channel) {
			sensor = &hwmon->sensors[i];
			break;
		}
	}

	return sensor;
}

static int cgbc_hwmon_read(struct device *dev, enum hwmon_sensor_types type, u32 attr, int channel,
			   long *val)
{
	struct cgbc_hwmon_data *hwmon = dev_get_drvdata(dev);
	struct cgbc_hwmon_sensor *sensor = cgbc_hwmon_find_sensor(hwmon, type, channel);
	struct cgbc_device_data *cgbc = hwmon->cgbc;
	u8 data[CGBC_HWMON_CMD_SENSOR_DATA_SIZE];
	int ret;

	if (!sensor)
		return -ENODEV;

	ret = cgbc_hwmon_cmd(cgbc, sensor->index, &data[0]);
	if (ret)
		return ret;

	*val = FIELD_PREP(CGBC_HWMON_DATA_HIGH, data[3]) | FIELD_PREP(CGBC_HWMON_DATA_LOW, data[2]);

	/* For the Board Controller 1lsb = 0.1 degree centigrade */
	if (sensor->type == hwmon_temp)
		*val *= 100;

	return 0;
}

static umode_t cgbc_hwmon_is_visible(const void *_data, enum hwmon_sensor_types type, u32 attr,
				     int channel)
{
	struct cgbc_hwmon_data *data = (struct cgbc_hwmon_data *)_data;
	struct cgbc_hwmon_sensor *sensor;

	sensor = cgbc_hwmon_find_sensor(data, type, channel);
	if (!sensor)
		return 0;

	return sensor->active ? 0444 : 0;
}

static int cgbc_hwmon_read_string(struct device *dev, enum hwmon_sensor_types type, u32 attr,
				  int channel, const char **str)
{
	struct cgbc_hwmon_data *hwmon = dev_get_drvdata(dev);
	struct cgbc_hwmon_sensor *sensor = cgbc_hwmon_find_sensor(hwmon, type, channel);

	if (!sensor)
		return -ENODEV;

	*str = sensor->label;

	return 0;
}

static const struct hwmon_channel_info * const cgbc_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(in,
			   HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL,
			   HWMON_I_INPUT | HWMON_I_LABEL, HWMON_I_INPUT | HWMON_I_LABEL),
	HWMON_CHANNEL_INFO(curr,
			   HWMON_C_INPUT | HWMON_C_LABEL, HWMON_C_INPUT | HWMON_C_LABEL,
			   HWMON_C_INPUT | HWMON_C_LABEL),
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT | HWMON_F_LABEL, HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL, HWMON_F_INPUT | HWMON_F_LABEL,
			   HWMON_F_INPUT | HWMON_F_LABEL, HWMON_F_INPUT | HWMON_F_LABEL),
	NULL
};

static const struct hwmon_ops cgbc_hwmon_ops = {
	.is_visible = cgbc_hwmon_is_visible,
	.read = cgbc_hwmon_read,
	.read_string = cgbc_hwmon_read_string,
};

static const struct hwmon_chip_info cgbc_chip_info = {
	.ops = &cgbc_hwmon_ops,
	.info = cgbc_hwmon_info,
};

static int cgbc_hwmon_probe(struct platform_device *pdev)
{
	struct cgbc_device_data *cgbc = dev_get_drvdata(pdev->dev.parent);
	struct device *dev = &pdev->dev;
	struct cgbc_hwmon_data *data;
	struct device *hwmon_dev;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->cgbc = cgbc;

	platform_set_drvdata(pdev, data);

	ret = cgbc_hwmon_probe_sensors(dev, data);
	if (ret)
		return dev_err_probe(dev, ret, "failed to probe sensors");

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "cgbc_hwmon", data, &cgbc_chip_info,
							 NULL);
	return PTR_ERR_OR_ZERO(hwmon_dev);
}

static struct platform_driver cgbc_hwmon_driver = {
	.driver = {
		.name = "cgbc-hwmon",
	},
	.probe = cgbc_hwmon_probe,
};

module_platform_driver(cgbc_hwmon_driver);

MODULE_AUTHOR("Thomas Richard <thomas.richard@bootlin.com>");
MODULE_DESCRIPTION("Congatec Board Controller Hardware Monitoring Driver");
MODULE_LICENSE("GPL");
