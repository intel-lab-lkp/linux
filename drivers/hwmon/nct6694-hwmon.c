// SPDX-License-Identifier: GPL-2.0
/*
 * Nuvoton NCT6694 HWMON driver based on USB interface.
 *
 * Copyright (C) 2024 Nuvoton Technology Corp.
 */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hwmon.h>
#include <linux/platform_device.h>
#include <linux/mfd/nct6694.h>

#define DRVNAME "nct6694-hwmon"

/* Host interface */
#define REQUEST_RPT_MOD			0xFF
#define REQUEST_HWMON_MOD		0x00

/* Report Channel */
#define HWMON_FIN_IDX(x)		(0x50 + ((x) * 2))
#define HWMON_FIN_STS(x)		(0x6E + (x))
#define HWMON_PWM_IDX(x)		(0x70 + (x))

/* Message Channel*/
/* Command 00h */
#define REQUEST_HWMON_CMD0_LEN		0x40
#define REQUEST_HWMON_CMD0_OFFSET	0x0000	/* OFFSET = SEL|CMD */
#define HWMON_FIN_EN(x)			(0x04 + (x))
#define HWMON_PWM_FREQ_IDX(x)		(0x30 + (x))
/* Command 02h */
#define REQUEST_HWMON_CMD2_LEN		0x90
#define REQUEST_HWMON_CMD2_OFFSET	0x0002	/* OFFSET = SEL|CMD */
#define HWMON_SMI_CTRL_IDX		0x00
#define HWMON_FIN_LIMIT_IDX(x)		(0x70 + ((x) * 2))
#define HWMON_CMD2_HYST_MASK		0x1F
/* Command 03h */
#define REQUEST_HWMON_CMD3_LEN		0x08
#define REQUEST_HWMON_CMD3_OFFSET	0x0003	/* OFFSET = SEL|CMD */

struct nct6694_hwmon_data {
	struct nct6694 *nct6694;

	/* Make sure read & write commands are consecutive */
	struct mutex hwmon_lock;
};

#define NCT6694_HWMON_FAN_CONFIG (HWMON_F_ENABLE | HWMON_F_INPUT | \
				  HWMON_F_MIN | HWMON_F_MIN_ALARM)
#define NCT6694_HWMON_PWM_CONFIG (HWMON_PWM_INPUT | HWMON_PWM_FREQ)

static const struct hwmon_channel_info *nct6694_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   NCT6694_HWMON_FAN_CONFIG,	/* FIN0 */
			   NCT6694_HWMON_FAN_CONFIG,	/* FIN1 */
			   NCT6694_HWMON_FAN_CONFIG,	/* FIN2 */
			   NCT6694_HWMON_FAN_CONFIG,	/* FIN3 */
			   NCT6694_HWMON_FAN_CONFIG,	/* FIN4 */
			   NCT6694_HWMON_FAN_CONFIG,	/* FIN5 */
			   NCT6694_HWMON_FAN_CONFIG,	/* FIN6 */
			   NCT6694_HWMON_FAN_CONFIG,	/* FIN7 */
			   NCT6694_HWMON_FAN_CONFIG,	/* FIN8 */
			   NCT6694_HWMON_FAN_CONFIG),	/* FIN9 */

	HWMON_CHANNEL_INFO(pwm,
			   NCT6694_HWMON_PWM_CONFIG,	/* PWM0 */
			   NCT6694_HWMON_PWM_CONFIG,	/* PWM1 */
			   NCT6694_HWMON_PWM_CONFIG,	/* PWM2 */
			   NCT6694_HWMON_PWM_CONFIG,	/* PWM3 */
			   NCT6694_HWMON_PWM_CONFIG,	/* PWM4 */
			   NCT6694_HWMON_PWM_CONFIG,	/* PWM5 */
			   NCT6694_HWMON_PWM_CONFIG,	/* PWM6 */
			   NCT6694_HWMON_PWM_CONFIG,	/* PWM7 */
			   NCT6694_HWMON_PWM_CONFIG,	/* PWM8 */
			   NCT6694_HWMON_PWM_CONFIG),	/* PWM9 */
	NULL
};

static int nct6694_fan_read(struct device *dev, u32 attr, int channel,
			    long *val)
{
	struct nct6694_hwmon_data *data = dev_get_drvdata(dev);
	unsigned char buf[2];
	int ret;

	switch (attr) {
	case hwmon_fan_enable:
		ret = nct6694_read_msg(data->nct6694, REQUEST_HWMON_MOD,
				       REQUEST_HWMON_CMD0_OFFSET,
				       REQUEST_HWMON_CMD0_LEN,
				       HWMON_FIN_EN(channel / 8),
				       1, buf);
		if (ret)
			return -EINVAL;

		*val = buf[0] & BIT(channel % 8) ? 1 : 0;

		break;

	case hwmon_fan_input:
		ret = nct6694_read_msg(data->nct6694, REQUEST_RPT_MOD,
				       HWMON_FIN_IDX(channel), 2, 0,
				       2, buf);
		if (ret)
			return -EINVAL;

		*val = (buf[1] | (buf[0] << 8)) & 0xFFFF;

		break;

	case hwmon_fan_min:
		ret = nct6694_read_msg(data->nct6694, REQUEST_HWMON_MOD,
				       REQUEST_HWMON_CMD2_OFFSET,
				       REQUEST_HWMON_CMD2_LEN,
				       HWMON_FIN_LIMIT_IDX(channel),
				       2, buf);
		if (ret)
			return -EINVAL;

		*val = (buf[1] | (buf[0] << 8)) & 0xFFFF;

		break;

	case hwmon_fan_min_alarm:
		ret = nct6694_read_msg(data->nct6694, REQUEST_RPT_MOD,
				       HWMON_FIN_STS(channel / 8),
				       1, 0, 1, buf);
		if (ret)
			return -EINVAL;

		*val = buf[0] & BIT(channel % 8);

		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int nct6694_pwm_read(struct device *dev, u32 attr, int channel,
			    long *val)
{
	struct nct6694_hwmon_data *data = dev_get_drvdata(dev);
	unsigned char buf;
	int ret;

	switch (attr) {
	case hwmon_pwm_input:
		ret = nct6694_read_msg(data->nct6694, REQUEST_RPT_MOD,
				       HWMON_PWM_IDX(channel),
				       1, 0, 1, &buf);
		if (ret)
			return -EINVAL;

		*val = buf;

		break;
	case hwmon_pwm_freq:
		ret = nct6694_read_msg(data->nct6694, REQUEST_HWMON_MOD,
				       REQUEST_HWMON_CMD0_OFFSET,
				       REQUEST_HWMON_CMD0_LEN,
				       HWMON_PWM_FREQ_IDX(channel),
				       1, &buf);
		if (ret)
			return -EINVAL;

		*val = buf * 25000 / 255;

		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int nct6694_fan_write(struct device *dev, u32 attr, int channel,
			     long val)
{
	struct nct6694_hwmon_data *data = dev_get_drvdata(dev);
	unsigned char enable_buf[REQUEST_HWMON_CMD0_LEN] = {0};
	unsigned char buf[REQUEST_HWMON_CMD2_LEN] = {0};
	u16 fan_val = (u16)val;
	int ret;

	switch (attr) {
	case hwmon_fan_enable:
		mutex_lock(&data->hwmon_lock);
		ret = nct6694_read_msg(data->nct6694, REQUEST_HWMON_MOD,
				       REQUEST_HWMON_CMD0_OFFSET,
				       REQUEST_HWMON_CMD0_LEN, 0,
				       REQUEST_HWMON_CMD0_LEN,
				       enable_buf);
		if (ret)
			goto err;

		if (val)
			enable_buf[HWMON_FIN_EN(channel / 8)] |= BIT(channel % 8);
		else
			enable_buf[HWMON_FIN_EN(channel / 8)] &= ~BIT(channel % 8);

		ret = nct6694_write_msg(data->nct6694, REQUEST_HWMON_MOD,
					REQUEST_HWMON_CMD0_OFFSET,
					REQUEST_HWMON_CMD0_LEN, enable_buf);
		if (ret)
			goto err;

		break;

	case hwmon_fan_min:
		mutex_lock(&data->hwmon_lock);
		ret = nct6694_read_msg(data->nct6694, REQUEST_HWMON_MOD,
				       REQUEST_HWMON_CMD2_OFFSET,
				       REQUEST_HWMON_CMD2_LEN, 0,
				       REQUEST_HWMON_CMD2_LEN, buf);
		if (ret)
			goto err;

		buf[HWMON_FIN_LIMIT_IDX(channel)] = (u8)((fan_val >> 8) & 0xFF);
		buf[HWMON_FIN_LIMIT_IDX(channel) + 1] = (u8)(fan_val & 0xFF);
		ret = nct6694_write_msg(data->nct6694, REQUEST_HWMON_MOD,
					REQUEST_HWMON_CMD2_OFFSET,
					REQUEST_HWMON_CMD2_LEN, buf);
		if (ret)
			goto err;

		break;

	default:
		ret = -EOPNOTSUPP;
		goto err;
	}

err:
	mutex_unlock(&data->hwmon_lock);
	return ret;
}

static int nct6694_read(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long *val)
{
	switch (type) {
	case hwmon_fan:	/* in RPM */
		return nct6694_fan_read(dev, attr, channel, val);

	case hwmon_pwm:	/* in value 0~255 */
		return nct6694_pwm_read(dev, attr, channel, val);

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static int nct6694_write(struct device *dev, enum hwmon_sensor_types type,
			 u32 attr, int channel, long val)
{
	switch (type) {
	case hwmon_fan:
		return nct6694_fan_write(dev, attr, channel, val);
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static umode_t nct6694_is_visible(const void *data, enum hwmon_sensor_types type,
				  u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		switch (attr) {
		case hwmon_fan_enable:
		case hwmon_fan_min:
			return 0644;

		case hwmon_fan_input:
		case hwmon_fan_min_alarm:
			return 0444;

		default:
			return 0;
		}

	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
		case hwmon_pwm_freq:
			return 0444;
		default:
			return 0;
		}

	default:
		return 0;
	}

	return 0;
}

static const struct hwmon_ops nct6694_hwmon_ops = {
	.is_visible = nct6694_is_visible,
	.read = nct6694_read,
	.write = nct6694_write,
};

static const struct hwmon_chip_info nct6694_chip_info = {
	.ops = &nct6694_hwmon_ops,
	.info = nct6694_info,
};

static int nct6694_hwmon_init(struct nct6694_hwmon_data *data)
{
	unsigned char buf[REQUEST_HWMON_CMD2_LEN] = {0};
	int ret;

	/* Set Fan input Real Time alarm mode */
	mutex_lock(&data->hwmon_lock);
	ret = nct6694_read_msg(data->nct6694, REQUEST_HWMON_MOD,
			       REQUEST_HWMON_CMD2_OFFSET,
			       REQUEST_HWMON_CMD2_LEN, 0,
			       REQUEST_HWMON_CMD2_LEN, buf);
	if (ret)
		goto err;

	buf[HWMON_SMI_CTRL_IDX] = 0x02;

	ret = nct6694_write_msg(data->nct6694, REQUEST_HWMON_MOD,
				REQUEST_HWMON_CMD2_OFFSET,
				REQUEST_HWMON_CMD2_LEN, buf);
	if (ret)
		goto err;

err:
	mutex_unlock(&data->hwmon_lock);
	return ret;
}

static int nct6694_hwmon_probe(struct platform_device *pdev)
{
	struct nct6694_hwmon_data *data;
	struct nct6694 *nct6694 = dev_get_drvdata(pdev->dev.parent);
	struct device *hwmon_dev;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->nct6694 = nct6694;
	mutex_init(&data->hwmon_lock);
	platform_set_drvdata(pdev, data);

	ret = nct6694_hwmon_init(data);
	if (ret)
		return -EIO;

	/* Register hwmon device to HWMON framework */
	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev,
							 "nct6694", data,
							 &nct6694_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev)) {
		dev_err(&pdev->dev, "%s: Failed to register hwmon device!\n",
			__func__);
		return PTR_ERR(hwmon_dev);
	}

	return 0;
}

static struct platform_driver nct6694_hwmon_driver = {
	.driver = {
		.name	= DRVNAME,
	},
	.probe		= nct6694_hwmon_probe,
};

static int __init nct6694_init(void)
{
	int err;

	err = platform_driver_register(&nct6694_hwmon_driver);
	if (!err) {
		if (err)
			platform_driver_unregister(&nct6694_hwmon_driver);
	}

	return err;
}
subsys_initcall(nct6694_init);

static void __exit nct6694_exit(void)
{
	platform_driver_unregister(&nct6694_hwmon_driver);
}
module_exit(nct6694_exit);

MODULE_DESCRIPTION("USB-HWMON driver for NCT6694");
MODULE_AUTHOR("Ming Yu <tmyu0@nuvoton.com>");
MODULE_LICENSE("GPL");
