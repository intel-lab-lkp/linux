// SPDX-License-Identifier: GPL-2.0-only
/*
 * stc3117_fuel_gauge.c - STMicroelectronics STC3117 Fuel Gauge Driver
 *
 * Copyright (c) 2024 Silicon Signals Pvt Ltd.
 * Author:      Bhavin Sharma <bhavin.sharma@siliconsignals.io>
 *              Hardevsinh Palaniya <hardevsinh.palaniya@siliconsignals.com>
 */


#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/power_supply.h>

#define VOLTAGE_REG_ADDR	0x08
#define VOLTAGE_REG_ADDR_SIZE	1		//in bytes
#define VOLTAGE_DATA_SIZE	2		//in bytes
#define LSB_VALUE		2200		//in micro-volts


static int stc3117_probe(struct i2c_client *client);
static void stc3117_dev_remove(struct i2c_client *client);

static int stc3117_get_property(struct power_supply *psy,
	enum power_supply_property psp, union power_supply_propval *val);
static int stc3117_get_batt_volt(const struct i2c_client *client);

const struct i2c_client *tmp_client;
struct power_supply *stc_sply;

static const struct of_device_id stc3117_of_match[] = {
	{ .compatible = "st,stc3117-fgu" },
	{},
};

MODULE_DEVICE_TABLE(of, stc3117_of_match);

static const struct i2c_device_id stc3117_id[] = {
	{"stc3117", 0},
	{},
};


MODULE_DEVICE_TABLE(i2c, stc3117_id);

struct i2c_driver stc3117_i2c_driver = {
	.driver = {
		.name = "stc3117_i2c_driver",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(stc3117_of_match),
	},
	.probe = stc3117_probe,
	.id_table = stc3117_id,
	.remove = stc3117_dev_remove,
};


static enum power_supply_property stc3117_battery_props[] = {
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
};

static const struct power_supply_desc stc3117_battery_desc = {
	.name = "stc3117-battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.get_property = stc3117_get_property,
	.properties = stc3117_battery_props,
	.num_properties = ARRAY_SIZE(stc3117_battery_props),
};

static int stc3117_get_property(struct power_supply *psy,
	enum power_supply_property psp, union power_supply_propval *val)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = stc3117_get_batt_volt(tmp_client);
	break;
	default:
		return -EINVAL;
	}
	return 0;
}


static int stc3117_get_batt_volt(const struct i2c_client *stc_client)
{
	int ret, volt = 0;
	char i2c_tx = VOLTAGE_REG_ADDR, i2c_rx[2] = {0};

	ret = i2c_master_send(stc_client, &i2c_tx, VOLTAGE_REG_ADDR_SIZE);
	if (ret > 0) {

		ret = i2c_master_recv(stc_client, i2c_rx, VOLTAGE_DATA_SIZE);
		if (ret > 0) {

			volt = (i2c_rx[1] << 8) + i2c_rx[0];
			volt *= LSB_VALUE;

			return volt;
		}
	}

	return ret;
}

static int stc3117_probe(struct i2c_client *client)
{
	struct power_supply_config psy_cfg = {};
	struct device *dev;

	dev = &client->dev;

	psy_cfg.of_node = dev->of_node;

	tmp_client = client;

	stc_sply = power_supply_register(dev, &stc3117_battery_desc, &psy_cfg);
	if (IS_ERR(stc_sply))
		pr_err("failed to register battery\n");

	return 0;
}

static void stc3117_dev_remove(struct i2c_client *client)
{
	power_supply_unregister(stc_sply);
}

module_i2c_driver(stc3117_i2c_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bhavin Sharma <bhavin.sharma@siliconsignals.io>");
MODULE_AUTHOR("Hardevsinh Palaniya <hardevsinh.palaniya@siliconsignals.io>");
MODULE_DESCRIPTION("STC3117 Fuel Gauge Driver");
