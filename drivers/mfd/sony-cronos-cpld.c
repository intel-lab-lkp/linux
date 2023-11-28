// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * I2C device driver for Sony Cronos CPLDs
 * Copyright (C) 2015-2017  Dialog Semiconductor
 * Copyright (C) 2022  Raptor Engineering, LLC
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/mfd/core.h>
#include <linux/i2c.h>
#include <linux/mfd/sony/cronos/core.h>
#include <linux/mfd/sony/cronos/registers.h>

static struct resource cronos_wdt_resources[] = {
};

static struct resource cronos_led_resources[] = {
};

static const struct mfd_cell cronos_cpld_devs[] = {
	{
		.name          = "cronos-watchdog",
		.num_resources = ARRAY_SIZE(cronos_wdt_resources),
		.resources     = cronos_wdt_resources,
		.of_compatible = "sony,cronos-watchdog",
	},
	{
		.name          = "cronos-leds",
		.id            = 1,
		.num_resources = ARRAY_SIZE(cronos_led_resources),
		.resources     = cronos_led_resources,
		.of_compatible = "sony,cronos-leds",
	},
};

static ssize_t payload_power_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int payloadpower_val = 0;
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	ret = regmap_read(chip->regmap, CRONOS_CPLD_PAYLOAD_POWER_CTL_REG, &payloadpower_val);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "0x%02x\n", payloadpower_val);
}

static ssize_t payload_power_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t len)
{
	u8 val = 0;
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	if (kstrtou8(buf, 0, &val))
		return -EINVAL;

	ret = regmap_write(chip->regmap, CRONOS_CPLD_PAYLOAD_POWER_CTL_REG, val);
	if (ret) {
		dev_err(dev, "Failed to write value 0x%02x to address 0x%02x",
			val, CRONOS_CPLD_PAYLOAD_POWER_CTL_REG);
		return ret;
	}
	return len;
}


static ssize_t bmc_flash_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int bmcflash_val = 0;
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	ret = regmap_read(chip->regmap, CRONOS_CPLD_BMC_BOOT_FLASH_SELECT_REG, &bmcflash_val);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "0x%02x\n", bmcflash_val);
}

static ssize_t bmc_flash_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t len)
{
	u8 val = 0;
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	if (kstrtou8(buf, 0, &val))
		return -EINVAL;

	ret = regmap_write(chip->regmap, CRONOS_CPLD_BMC_BOOT_FLASH_SELECT_REG, val);
	if (ret) {
		dev_err(dev, "Failed to write value 0x%02x to address 0x%02x",
			val, CRONOS_CPLD_BMC_BOOT_FLASH_SELECT_REG);
		return ret;
	}
	return len;
}


static ssize_t switch_reset_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int switchreset_val = 0;
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	ret = regmap_read(chip->regmap, CRONOS_CPLD_SWITCH_RESET_CMD_REG, &switchreset_val);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "0x%02x\n", switchreset_val);
}

static ssize_t switch_reset_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len)
{
	unsigned int switchreset_val = 0;
	u8 val = -EINVAL;
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	if (kstrtou8(buf, 0, &val))
		return -EINVAL;

	if (val != 1)
		return -EINVAL;

	ret = regmap_read(chip->regmap, CRONOS_CPLD_SWITCH_RESET_CMD_REG, &switchreset_val);
	if (ret < 0)
		return ret;

	ret = regmap_write(chip->regmap, CRONOS_CPLD_SWITCH_RESET_CMD_REG, switchreset_val);
	if (ret) {
		dev_err(dev, "Failed to write value 0x%02x to address 0x%02x",
				switchreset_val, CRONOS_CPLD_SWITCH_RESET_CMD_REG);
		return ret;
	}
	return len;
}


static ssize_t switch_flash_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int switchflash_val = 0;
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	ret = regmap_read(chip->regmap, CRONOS_CPLD_SWITCH_BOOT_FLASH_SELECT_REG, &switchflash_val);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "0x%02x\n", switchflash_val);
}

static ssize_t switch_flash_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t len)
{
	u8 val = 0;
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	if (kstrtou8(buf, 0, &val))
		return -EINVAL;

	ret = regmap_write(chip->regmap, CRONOS_CPLD_SWITCH_BOOT_FLASH_SELECT_REG, val);
	if (ret) {
		dev_err(dev, "Failed to write value 0x%02x to address 0x%02x",
			val, CRONOS_CPLD_SWITCH_BOOT_FLASH_SELECT_REG);
		return ret;
	}
	return len;
}


static ssize_t uart_mux_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int uartmux_val = 0;
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	ret = regmap_read(chip->regmap, CRONOS_CPLD_UART_MUX_REG, &uartmux_val);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "0x%02x\n", uartmux_val);
}

static ssize_t uart_mux_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t len)
{
	u8 val = 0;
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	if (kstrtou8(buf, 0, &val))
		return -EINVAL;

	ret = regmap_write(chip->regmap, CRONOS_CPLD_UART_MUX_REG, val);
	if (ret) {
		dev_err(dev, "Failed to write value 0x%02x to address 0x%02x",
			val, CRONOS_CPLD_UART_MUX_REG);
		return ret;
	}
	return len;
}


static ssize_t led_get_brightness(struct sony_cronos_cpld *chip, unsigned int reg, char *buf)
{
	unsigned int brightness_val;
	int ret = -EIO;

	ret = regmap_read(chip->regmap, reg, &brightness_val);
	if (ret != 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "0x%02x\n", brightness_val);
}

static ssize_t led_set_brightness(struct sony_cronos_cpld *chip, unsigned int reg, const char *buf,
	size_t len)
{
	u8 val = 0;
	int ret = -EIO;

	if (kstrtou8(buf, 0, &val))
		return -EINVAL;

	ret = regmap_update_bits(chip->regmap, reg, CRONOS_CPLD_LEDS_BRIGHTNESS_SET_MASK, val);
	if (ret) {
		dev_err(chip->dev, "Failed to write value 0x%02x to address 0x%02x", val, reg);
		return ret;
	}
	return len;
}

static ssize_t brightness_red_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	return led_get_brightness(chip, CRONOS_CPLD_BRIGHTNESS_RED_REG, buf);
}

static ssize_t brightness_red_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	return led_set_brightness(chip, CRONOS_CPLD_BRIGHTNESS_RED_REG, buf, len);
}

static ssize_t brightness_green_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	return led_get_brightness(chip, CRONOS_CPLD_BRIGHTNESS_GREEN_REG, buf);
}

static ssize_t brightness_green_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	return led_set_brightness(chip, CRONOS_CPLD_BRIGHTNESS_GREEN_REG, buf, len);
}

static ssize_t brightness_blue_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	return led_get_brightness(chip, CRONOS_CPLD_BRIGHTNESS_BLUE_REG, buf);
}

static ssize_t brightness_blue_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t len)
{
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	return led_set_brightness(chip, CRONOS_CPLD_BRIGHTNESS_BLUE_REG, buf, len);
}


static ssize_t revision_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u16 revision = 0;
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	ret = regmap_bulk_read(chip->regmap, CRONOS_CPLD_REVISION_LOW_REG, &revision, 2);
	if (ret)
		return -EIO;

	return snprintf(buf, PAGE_SIZE, "0x%04x\n", revision);
}

static ssize_t device_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u16 device_id = 0;
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	ret = regmap_bulk_read(chip->regmap, CRONOS_CPLD_DEVICE_ID_LOW_REG, &device_id, 2);
	if (ret)
		return -EIO;

	return snprintf(buf, PAGE_SIZE, "0x%04x\n", device_id);
}

static ssize_t bmc_mac_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	u8 bmc_mac[6];
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	ret = regmap_bulk_read(chip->regmap, CRONOS_CPLD_BMC_MAC_LOW_REG, bmc_mac, 6);
	if (ret)
		return -EIO;

	return snprintf(buf, PAGE_SIZE, "%pM\n", bmc_mac);
}

static ssize_t status_2_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	unsigned int last_boot = 0;
	int ret = -EIO;
	struct sony_cronos_cpld *chip = dev_get_drvdata(dev);

	ret = regmap_read(chip->regmap, CRONOS_CPLD_STATUS_2_REG, &last_boot);
	if (ret < 0)
		return ret;

	return snprintf(buf, PAGE_SIZE, "0x%02x\n", last_boot);
}


static DEVICE_ATTR_RO(revision);
static DEVICE_ATTR_RO(device_id);
static DEVICE_ATTR_RO(bmc_mac);
static DEVICE_ATTR_RO(status_2);

static DEVICE_ATTR_RW(uart_mux);
static DEVICE_ATTR_RW(switch_flash);
static DEVICE_ATTR_RW(switch_reset);
static DEVICE_ATTR_RW(bmc_flash);
static DEVICE_ATTR_RW(payload_power);

static DEVICE_ATTR_RW(brightness_red);
static DEVICE_ATTR_RW(brightness_green);
static DEVICE_ATTR_RW(brightness_blue);
static struct attribute *cronos_cpld_sysfs_entries[] = {
	&dev_attr_revision.attr,
	&dev_attr_device_id.attr,
	&dev_attr_bmc_mac.attr,
	&dev_attr_status_2.attr,
	&dev_attr_uart_mux.attr,
	&dev_attr_switch_flash.attr,
	&dev_attr_switch_reset.attr,
	&dev_attr_bmc_flash.attr,
	&dev_attr_payload_power.attr,
	&dev_attr_brightness_red.attr,
	&dev_attr_brightness_green.attr,
	&dev_attr_brightness_blue.attr,
	NULL,
};

static const struct attribute_group cronos_cpld_attr_group = {
	.attrs	= cronos_cpld_sysfs_entries,
};

static int sony_cronos_get_device_type(struct sony_cronos_cpld *chip)
{
	int device_id;
	int byte;
	int ret;

	ret = regmap_read(chip->regmap, CRONOS_CPLD_DEVICE_ID_HIGH_REG, &byte);
	if (ret < 0) {
		dev_err(chip->dev, "Cannot read chip ID.\n");
		return -EIO;
	}
	device_id = byte << 8;
	ret = regmap_read(chip->regmap, CRONOS_CPLD_DEVICE_ID_LOW_REG, &byte);
	if (ret < 0) {
		dev_err(chip->dev, "Cannot read chip ID.\n");
		return -EIO;
	}
	device_id |= byte;
	if (device_id != CRONOS_CPLD_DEVICE_ID) {
		dev_err(chip->dev, "Invalid device ID: 0x%04x\n", device_id);
		return -ENODEV;
	}

	dev_info(chip->dev,
		 "Device detected (device-ID: 0x%04X)\n",
		 device_id);

	return ret;
}

static bool cronos_cpld_is_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CRONOS_CPLD_BRIGHTNESS_RED_REG:
	case CRONOS_CPLD_BRIGHTNESS_GREEN_REG:
	case CRONOS_CPLD_BRIGHTNESS_BLUE_REG:
	case CRONOS_LEDS_SMC_STATUS_REG:
	case CRONOS_LEDS_SWITCH_STATUS_REG:
	case CRONOS_LEDS_CCM1_STATUS_REG:
	case CRONOS_LEDS_CCM2_STATUS_REG:
	case CRONOS_LEDS_CCM3_STATUS_REG:
	case CRONOS_LEDS_CCM4_STATUS_REG:
	case CRONOS_LEDS_CCM_POWER_REG:

	case CRONOS_WDT_CTL_REG:
	case CRONOS_WDT_CLR_REG:

	case CRONOS_CPLD_UART_MUX_REG:
	case CRONOS_CPLD_SWITCH_BOOT_FLASH_SELECT_REG:
	case CRONOS_CPLD_SWITCH_RESET_CMD_REG:
	case CRONOS_CPLD_BMC_BOOT_FLASH_SELECT_REG:
	case CRONOS_CPLD_PAYLOAD_POWER_CTL_REG:
		return true;
	default:
		return false;
	}
}

static bool cronos_cpld_is_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CRONOS_CPLD_REVISION_HIGH_REG:
	case CRONOS_CPLD_REVISION_LOW_REG:
	case CRONOS_CPLD_DEVICE_ID_HIGH_REG:
	case CRONOS_CPLD_DEVICE_ID_LOW_REG:

	case CRONOS_CPLD_BRIGHTNESS_RED_REG:
	case CRONOS_CPLD_BRIGHTNESS_GREEN_REG:
	case CRONOS_CPLD_BRIGHTNESS_BLUE_REG:
	case CRONOS_LEDS_SMC_STATUS_REG:
	case CRONOS_LEDS_SWITCH_STATUS_REG:
	case CRONOS_LEDS_CCM1_STATUS_REG:
	case CRONOS_LEDS_CCM2_STATUS_REG:
	case CRONOS_LEDS_CCM3_STATUS_REG:
	case CRONOS_LEDS_CCM4_STATUS_REG:
	case CRONOS_LEDS_CCM_POWER_REG:

	case CRONOS_WDT_CTL_REG:
	case CRONOS_WDT_CLR_REG:

	case CRONOS_CPLD_STATUS_2_REG:
	case CRONOS_CPLD_UART_MUX_REG:
	case CRONOS_CPLD_SWITCH_BOOT_FLASH_SELECT_REG:
	case CRONOS_CPLD_SWITCH_RESET_CMD_REG:
	case CRONOS_CPLD_BMC_BOOT_FLASH_SELECT_REG:
	case CRONOS_CPLD_PAYLOAD_POWER_CTL_REG:

	case CRONOS_CPLD_BMC_MAC_LOW_REG ... CRONOS_CPLD_BMC_MAC_HIGH_REG:
		return true;
	default:
		return false;
	}
}

static bool cronos_cpld_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CRONOS_CPLD_REVISION_HIGH_REG:
	case CRONOS_CPLD_REVISION_LOW_REG:

	case CRONOS_CPLD_SWITCH_BOOT_FLASH_SELECT_REG:
	case CRONOS_CPLD_SWITCH_RESET_CMD_REG:
	case CRONOS_CPLD_BMC_BOOT_FLASH_SELECT_REG:
	case CRONOS_CPLD_PAYLOAD_POWER_CTL_REG:

	case CRONOS_WDT_CTL_REG:
	case CRONOS_WDT_CLR_REG:
		return true;
	default:
		return false;
	}
}

static struct regmap_config cronos_cpld_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = CRONOS_CPLD_REVISION_HIGH_REG,
	.writeable_reg = cronos_cpld_is_writeable_reg,
	.readable_reg = cronos_cpld_is_readable_reg,
	.volatile_reg = cronos_cpld_is_volatile_reg,
	.use_single_read = true,
	.use_single_write = true,
	.cache_type = REGCACHE_RBTREE,
};

#ifdef CONFIG_OF
static const struct of_device_id cronos_cpld_dt_ids[] = {
	{ .compatible = "sony,cronos-cpld", },
	{ }
};
MODULE_DEVICE_TABLE(of, cronos_cpld_dt_ids);
#endif

static int sony_cronos_i2c_probe(struct i2c_client *i2c)
{
	struct sony_cronos_cpld *chip;
	const struct of_device_id *match;
	const struct mfd_cell *cell;
	const struct regmap_config *config;
	int cell_num;
	int ret;

	chip = devm_kzalloc(&i2c->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	if (i2c->dev.of_node) {
		match = of_match_node(cronos_cpld_dt_ids, i2c->dev.of_node);
		if (!match)
			return -EINVAL;
	}

	i2c_set_clientdata(i2c, chip);
	chip->dev = &i2c->dev;

	cell = cronos_cpld_devs;
	cell_num = ARRAY_SIZE(cronos_cpld_devs);
	config = &cronos_cpld_regmap_config;

	chip->regmap = devm_regmap_init_i2c(i2c, config);
	if (IS_ERR(chip->regmap)) {
		ret = PTR_ERR(chip->regmap);
		dev_err(chip->dev, "Failed to allocate register map: %d\n",
			ret);
		return ret;
	}

	ret = sony_cronos_get_device_type(chip);
	if (ret)
		return ret;

	ret = mfd_add_devices(chip->dev, PLATFORM_DEVID_NONE, cell,
			      cell_num, NULL, 0, NULL);
	if (ret) {
		dev_err(chip->dev, "Cannot register child devices\n");
		return ret;
	}

	/* Add sysfs */
	ret = sysfs_create_group(&chip->dev->kobj, &cronos_cpld_attr_group);
	if (ret)
		dev_err(chip->dev, "Failed to create sysfs entries\n");

	return ret;
}

static void sony_cronos_i2c_remove(struct i2c_client *i2c)
{
	struct sony_cronos_cpld *chip = i2c_get_clientdata(i2c);

	sysfs_remove_group(&chip->dev->kobj, &cronos_cpld_attr_group);
	mfd_remove_devices(chip->dev);
}

static struct i2c_driver sony_cronos_i2c_driver = {
	.driver = {
		.name = "sony-cronos",
		.of_match_table = of_match_ptr(cronos_cpld_dt_ids),
	},
	.probe    = sony_cronos_i2c_probe,
	.remove   = sony_cronos_i2c_remove,
};

module_i2c_driver(sony_cronos_i2c_driver);

MODULE_DESCRIPTION("Core device driver for sony Cronos CPLDs");
MODULE_AUTHOR("Raptor Engineering, LLC <support@raptorengineering.com>");
MODULE_LICENSE("GPL");
