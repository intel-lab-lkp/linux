// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Hardware monitoring driver for LattePanda Sigma EC.
 *
 * Reads fan RPM and temperatures from the Embedded Controller via
 * ACPI EC I/O ports (0x62 data, 0x66 cmd/status). The BIOS reports
 * the ACPI EC as disabled (_STA=0), so direct port I/O is used.
 *
 * Copyright (c) 2026 Mariano Abad <weimaraner@gmail.com>
 */

#include <linux/delay.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#define DRIVER_NAME	"lattepanda_sigma_ec"

/* EC I/O ports (standard ACPI EC interface) */
#define EC_DATA_PORT	0x62
#define EC_CMD_PORT	0x66	/* also status port */

/* EC commands */
#define EC_CMD_READ	0x80

/* EC status register bits */
#define EC_STATUS_OBF	0x01	/* Output Buffer Full */
#define EC_STATUS_IBF	0x02	/* Input Buffer Full */

/* EC register offsets for LattePanda Sigma */
#define EC_REG_FAN_RPM_HI	0x2E
#define EC_REG_FAN_RPM_LO	0x2F
#define EC_REG_TEMP1		0x60
#define EC_REG_TEMP2		0x70
#define EC_REG_FAN_DUTY		0x93

/* Timeout for EC operations (in microseconds) */
#define EC_TIMEOUT_US		25000
#define EC_POLL_INTERVAL_US	5

struct lattepanda_sigma_ec_data {
	struct mutex lock;	/* serialize EC access */
};

static struct platform_device *lps_ec_pdev;

static int ec_wait_ibf_clear(void)
{
	int timeout = EC_TIMEOUT_US / EC_POLL_INTERVAL_US;

	while (timeout--) {
		if (!(inb(EC_CMD_PORT) & EC_STATUS_IBF))
			return 0;
		udelay(EC_POLL_INTERVAL_US);
	}
	return -ETIMEDOUT;
}

static int ec_wait_obf_set(void)
{
	int timeout = EC_TIMEOUT_US / EC_POLL_INTERVAL_US;

	while (timeout--) {
		if (inb(EC_CMD_PORT) & EC_STATUS_OBF)
			return 0;
		udelay(EC_POLL_INTERVAL_US);
	}
	return -ETIMEDOUT;
}

static int ec_read_reg(struct lattepanda_sigma_ec_data *data, u8 reg, u8 *val)
{
	int ret;

	mutex_lock(&data->lock);

	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;

	outb(EC_CMD_READ, EC_CMD_PORT);

	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;

	outb(reg, EC_DATA_PORT);

	ret = ec_wait_obf_set();
	if (ret)
		goto out;

	*val = inb(EC_DATA_PORT);

out:
	mutex_unlock(&data->lock);
	return ret;
}

/*
 * Read a 16-bit big-endian value from two consecutive EC registers.
 * Both bytes are read within a single mutex hold to prevent tearing.
 */
static int ec_read_reg16(struct lattepanda_sigma_ec_data *data,
			 u8 reg_hi, u8 reg_lo, u16 *val)
{
	int ret;
	u8 hi, lo;

	mutex_lock(&data->lock);

	/* Read high byte */
	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(EC_CMD_READ, EC_CMD_PORT);
	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(reg_hi, EC_DATA_PORT);
	ret = ec_wait_obf_set();
	if (ret)
		goto out;
	hi = inb(EC_DATA_PORT);

	/* Read low byte */
	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(EC_CMD_READ, EC_CMD_PORT);
	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(reg_lo, EC_DATA_PORT);
	ret = ec_wait_obf_set();
	if (ret)
		goto out;
	lo = inb(EC_DATA_PORT);

	*val = ((u16)hi << 8) | lo;

out:
	mutex_unlock(&data->lock);
	return ret;
}

static int
lattepanda_sigma_ec_read_string(struct device *dev,
				enum hwmon_sensor_types type,
				u32 attr, int channel,
				const char **str)
{
	switch (type) {
	case hwmon_fan:
		*str = "CPU Fan";
		return 0;
	case hwmon_temp:
		*str = channel == 0 ? "Board Temp" : "CPU Temp";
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static umode_t
lattepanda_sigma_ec_is_visible(const void *drvdata,
			       enum hwmon_sensor_types type,
			       u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		if (attr == hwmon_fan_input || attr == hwmon_fan_label)
			return 0444;
		break;
	case hwmon_temp:
		if (attr == hwmon_temp_input || attr == hwmon_temp_label)
			return 0444;
		break;
	default:
		break;
	}
	return 0;
}

static int
lattepanda_sigma_ec_read(struct device *dev,
			 enum hwmon_sensor_types type,
			 u32 attr, int channel, long *val)
{
	struct lattepanda_sigma_ec_data *data = dev_get_drvdata(dev);
	u16 rpm;
	u8 v;
	int ret;

	switch (type) {
	case hwmon_fan:
		if (attr != hwmon_fan_input)
			return -EOPNOTSUPP;
		ret = ec_read_reg16(data, EC_REG_FAN_RPM_HI,
				    EC_REG_FAN_RPM_LO, &rpm);
		if (ret)
			return ret;
		*val = rpm;
		return 0;

	case hwmon_temp:
		if (attr != hwmon_temp_input)
			return -EOPNOTSUPP;
		ret = ec_read_reg(data,
				  channel == 0 ? EC_REG_TEMP1 : EC_REG_TEMP2,
				  &v);
		if (ret)
			return ret;
		/* hwmon temps are in millidegrees Celsius */
		*val = (long)v * 1000;
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_channel_info * const lattepanda_sigma_ec_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT | HWMON_F_LABEL),
	HWMON_CHANNEL_INFO(temp,
			   HWMON_T_INPUT | HWMON_T_LABEL,
			   HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_ops lattepanda_sigma_ec_ops = {
	.is_visible = lattepanda_sigma_ec_is_visible,
	.read = lattepanda_sigma_ec_read,
	.read_string = lattepanda_sigma_ec_read_string,
};

static const struct hwmon_chip_info lattepanda_sigma_ec_chip_info = {
	.ops = &lattepanda_sigma_ec_ops,
	.info = lattepanda_sigma_ec_info,
};

static int lattepanda_sigma_ec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct lattepanda_sigma_ec_data *data;
	struct device *hwmon;
	u8 test;
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	mutex_init(&data->lock);
	platform_set_drvdata(pdev, data);

	/* Sanity check: verify EC is responsive */
	ret = ec_read_reg(data, EC_REG_FAN_DUTY, &test);
	if (ret)
		return dev_err_probe(dev, ret,
				     "EC not responding on ports 0x%x/0x%x\n",
				     EC_DATA_PORT, EC_CMD_PORT);

	hwmon = devm_hwmon_device_register_with_info(dev, DRIVER_NAME, data,
						     &lattepanda_sigma_ec_chip_info,
						     NULL);
	if (IS_ERR(hwmon))
		return dev_err_probe(dev, PTR_ERR(hwmon),
				     "Failed to register hwmon device\n");

	dev_dbg(dev, "EC hwmon registered (fan duty: %u%%)\n", test);
	return 0;
}

static const struct dmi_system_id lattepanda_sigma_ec_dmi_table[] = {
	{
		.ident = "LattePanda Sigma",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LattePanda"),
			DMI_MATCH(DMI_PRODUCT_NAME, "LattePanda Sigma"),
		},
	},
	{ }	/* terminator */
};
MODULE_DEVICE_TABLE(dmi, lattepanda_sigma_ec_dmi_table);

static struct platform_driver lattepanda_sigma_ec_driver = {
	.probe	= lattepanda_sigma_ec_probe,
	.driver	= {
		.name = DRIVER_NAME,
	},
};

static int __init lattepanda_sigma_ec_init(void)
{
	int ret;

	if (!dmi_check_system(lattepanda_sigma_ec_dmi_table))
		return -ENODEV;

	lps_ec_pdev = platform_device_register_simple(DRIVER_NAME, -1, NULL, 0);
	if (IS_ERR(lps_ec_pdev))
		return PTR_ERR(lps_ec_pdev);

	ret = platform_driver_register(&lattepanda_sigma_ec_driver);
	if (ret) {
		platform_device_unregister(lps_ec_pdev);
		return ret;
	}

	return 0;
}

static void __exit lattepanda_sigma_ec_exit(void)
{
	platform_driver_unregister(&lattepanda_sigma_ec_driver);
	platform_device_unregister(lps_ec_pdev);
}

module_init(lattepanda_sigma_ec_init);
module_exit(lattepanda_sigma_ec_exit);

MODULE_AUTHOR("Mariano Abad <weimaraner@gmail.com>");
MODULE_DESCRIPTION("Hardware monitoring driver for LattePanda Sigma EC");
MODULE_LICENSE("GPL");
