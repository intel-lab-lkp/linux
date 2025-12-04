// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Device driver for Sony Cronos SMCs
 * Copyright (C) 2015-2017  Dialog Semiconductor
 * Copyright (C) 2022-2025  Raptor Engineering, LLC
 */

#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/mfd/core.h>
#include <linux/mfd/sony-cronos.h>
#include <linux/module.h>
#include <linux/regmap.h>

static const struct mfd_cell cronos_smc_devs[] = {
	{
		.name = "cronos-watchdog",
		.of_compatible = "sony,cronos-watchdog",
	},
	{
		.name = "cronos-led",
		.of_compatible = "sony,cronos-led",
	},
};

static int sony_cronos_get_device_type(struct sony_cronos_smc *ddata)
{
	int device_id;
	int byte_high;
	int byte_low;
	int ret;

	ret = regmap_read(ddata->regmap, CRONOS_SMC_DEVICE_ID_HIGH_REG, &byte_high);
	if (ret) {
		dev_err(ddata->dev, "Cannot read ddata ID high byte.\n");
		return -EIO;
	}
	ret = regmap_read(ddata->regmap, CRONOS_SMC_DEVICE_ID_LOW_REG, &byte_low);
	if (ret) {
		dev_err(ddata->dev, "Cannot read ddata ID low byte.\n");
		return -EIO;
	}

	device_id = byte_high << 8;
	device_id |= byte_low;

	if (device_id != CRONOS_SMC_DEVICE_ID) {
		dev_err(ddata->dev, "Unsupported device ID 0x%04x\n", device_id);
		return -ENODEV;
	}

	return ret;
}

static bool cronos_smc_is_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CRONOS_SMC_BRIGHTNESS_RED_REG:
	case CRONOS_SMC_BRIGHTNESS_GREEN_REG:
	case CRONOS_SMC_BRIGHTNESS_BLUE_REG:
	case CRONOS_LEDS_SMC_STATUS_REG:
	case CRONOS_LEDS_SWITCH_STATUS_REG:
	case CRONOS_LEDS_CCM1_STATUS_REG:
	case CRONOS_LEDS_CCM2_STATUS_REG:
	case CRONOS_LEDS_CCM3_STATUS_REG:
	case CRONOS_LEDS_CCM4_STATUS_REG:
	case CRONOS_LEDS_CCM_POWER_REG:

	case CRONOS_WDT_CTL_REG:
	case CRONOS_WDT_CLR_REG:

	case CRONOS_SMC_UART_MUX_REG:
	case CRONOS_SMC_SWITCH_BOOT_FLASH_SELECT_REG:
	case CRONOS_SMC_SWITCH_RESET_CMD_REG:
	case CRONOS_SMC_BMC_BOOT_FLASH_SELECT_REG:
	case CRONOS_SMC_PAYLOAD_POWER_CTL_REG:
		return true;
	default:
		return false;
	}
}

static bool cronos_smc_is_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CRONOS_SMC_REVISION_HIGH_REG:
	case CRONOS_SMC_REVISION_LOW_REG:
	case CRONOS_SMC_DEVICE_ID_HIGH_REG:
	case CRONOS_SMC_DEVICE_ID_LOW_REG:

	case CRONOS_SMC_BRIGHTNESS_RED_REG:
	case CRONOS_SMC_BRIGHTNESS_GREEN_REG:
	case CRONOS_SMC_BRIGHTNESS_BLUE_REG:
	case CRONOS_LEDS_SMC_STATUS_REG:
	case CRONOS_LEDS_SWITCH_STATUS_REG:
	case CRONOS_LEDS_CCM1_STATUS_REG:
	case CRONOS_LEDS_CCM2_STATUS_REG:
	case CRONOS_LEDS_CCM3_STATUS_REG:
	case CRONOS_LEDS_CCM4_STATUS_REG:
	case CRONOS_LEDS_CCM_POWER_REG:

	case CRONOS_WDT_CTL_REG:
	case CRONOS_WDT_CLR_REG:

	case CRONOS_SMC_STATUS_2_REG:
	case CRONOS_SMC_UART_MUX_REG:
	case CRONOS_SMC_SWITCH_BOOT_FLASH_SELECT_REG:
	case CRONOS_SMC_SWITCH_RESET_CMD_REG:
	case CRONOS_SMC_BMC_BOOT_FLASH_SELECT_REG:
	case CRONOS_SMC_PAYLOAD_POWER_CTL_REG:

	case CRONOS_SMC_BMC_MAC_LOW_REG ... CRONOS_SMC_BMC_MAC_HIGH_REG:
		return true;
	default:
		return false;
	}
}

static bool cronos_smc_is_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case CRONOS_SMC_REVISION_HIGH_REG:
	case CRONOS_SMC_REVISION_LOW_REG:

	case CRONOS_SMC_SWITCH_BOOT_FLASH_SELECT_REG:
	case CRONOS_SMC_SWITCH_RESET_CMD_REG:
	case CRONOS_SMC_BMC_BOOT_FLASH_SELECT_REG:
	case CRONOS_SMC_PAYLOAD_POWER_CTL_REG:

	case CRONOS_WDT_CTL_REG:
	case CRONOS_WDT_CLR_REG:
		return true;
	default:
		return false;
	}
}

static struct regmap_config cronos_smc_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = CRONOS_SMC_REVISION_HIGH_REG,
	.writeable_reg = cronos_smc_is_writeable_reg,
	.readable_reg = cronos_smc_is_readable_reg,
	.volatile_reg = cronos_smc_is_volatile_reg,
	.use_single_read = true,
	.use_single_write = true,
	.cache_type = REGCACHE_MAPLE,
};

static const struct of_device_id cronos_smc_dt_ids[] = {
	{
		.compatible = "sony,cronos-smc",
	},
	{},
};
MODULE_DEVICE_TABLE(of, cronos_smc_dt_ids);

static int sony_cronos_i2c_probe(struct i2c_client *i2c)
{
	struct sony_cronos_smc *ddata;
	int ret;

	ddata = devm_kzalloc(&i2c->dev, sizeof(*ddata), GFP_KERNEL);
	if (!ddata)
		return -ENOMEM;

	i2c_set_clientdata(i2c, ddata);
	ddata->dev = &i2c->dev;

	ddata->regmap = devm_regmap_init_i2c(i2c, &cronos_smc_regmap_config);
	if (IS_ERR(ddata->regmap)) {
		return dev_err_probe(ddata->dev, PTR_ERR(ddata->regmap),
				     "Failed to allocate register map\n");
	}

	ret = sony_cronos_get_device_type(ddata);
	if (ret)
		return ret;

	ret = mfd_add_devices(ddata->dev, PLATFORM_DEVID_AUTO, cronos_smc_devs,
			      ARRAY_SIZE(cronos_smc_devs), NULL, 0, NULL);
	if (ret) {
		dev_err(ddata->dev, "Failed to register child devices\n");
		return ret;
	}

	return ret;
}

static void sony_cronos_i2c_remove(struct i2c_client *i2c)
{
	struct sony_cronos_smc *ddata = i2c_get_clientdata(i2c);

	mfd_remove_devices(ddata->dev);
}

static struct i2c_driver sony_cronos_i2c_driver = {
	.driver = {
		.name = "sony-cronos-smc",
		.of_match_table = of_match_ptr(cronos_smc_dt_ids),
	},
	.probe = sony_cronos_i2c_probe,
	.remove = sony_cronos_i2c_remove,
};
module_i2c_driver(sony_cronos_i2c_driver);

MODULE_DESCRIPTION("Device driver for the Sony Cronos system management controller");
MODULE_AUTHOR("Raptor Engineering, LLC <tpearson@raptorengineering.com>");
MODULE_LICENSE("GPL");