// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2025 Andreas Kemnade

/* Datasheet: https://www.ti.com/lit/gpn/tps65185 */

#include <linux/cleanup.h>
#include <linux/completion.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/hwmon.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/regmap.h>

#define TPS65185_REG_TMST_VALUE 0
#define TPS65185_REG_ENABLE 1
#define TPS65185_REG_VADJ 2
#define TPS65185_REG_VCOM1 3
#define TPS65185_REG_VCOM2 4
#define TPS65185_REG_INT_EN1 5
#define TPS65185_REG_INT_EN2 6
#define TPS65185_REG_INT1 7
#define TPS65185_REG_INT2 8
#define TPS65185_REG_TMST1 0xd
#define TPS65185_REG_TMST2 0xe
#define TPS65185_REG_PG 0xf
#define TPS65185_REG_REVID 0x10

#define TPS65185_READ_THERM BIT(7)
#define TPS65185_CONV_END BIT(5)

#define TPS65185_ENABLE_ACTIVE BIT(7)
#define TPS65185_ENABLE_STANDBY BIT(6)

#define PGOOD_TIMEOUT_MSECS 200

struct tps65185_data {
	struct device *dev;
	struct regmap *regmap;
	struct regulator *vin_reg;
	struct gpio_desc *pgood_gpio;
	struct gpio_desc *pwrup_gpio;
	struct gpio_desc *vcom_ctrl_gpio;
	struct gpio_desc *wakeup_gpio;
	struct completion pgood_completion;
	int pgood_irq;
	struct completion tmst_completion;
	unsigned int vcom_msb;
};

static const struct hwmon_channel_info *tps65185_info[] = {
	HWMON_CHANNEL_INFO(chip, HWMON_C_REGISTER_TZ),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL
};

static int tps65185_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			       u32 attr, int channel, long *temp)
{
	struct tps65185_data *data = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = pm_runtime_resume_and_get(data->dev);
	if (ret < 0)
		return ret;

	reinit_completion(&data->tmst_completion);
	/* start acquisition */
	regmap_update_bits(data->regmap, TPS65185_REG_TMST1,
			   TPS65185_READ_THERM, TPS65185_READ_THERM);
	wait_for_completion_timeout(&data->tmst_completion,
				    msecs_to_jiffies(PGOOD_TIMEOUT_MSECS));
	ret = regmap_read(data->regmap, TPS65185_REG_TMST1, &val);
	if (!(val & TPS65185_CONV_END)) {
		ret = -ETIMEDOUT;
		goto out;
	}
	ret = regmap_read(data->regmap, TPS65185_REG_TMST_VALUE, &val);
	if (ret)
		goto out;

	*temp = (s8)val * 1000;

out:
	pm_runtime_put_autosuspend(data->dev);

	return ret;
}

static umode_t tps65185_hwmon_is_visible(const void *data,
					 enum hwmon_sensor_types type,
					 u32 attr, int channel)
{
	return 0444;
}

static const struct hwmon_ops tps65185_hwmon_ops = {
	.is_visible = tps65185_hwmon_is_visible,
	.read = tps65185_hwmon_read,
};

static const struct hwmon_chip_info tps65185_chip_info = {
	.ops = &tps65185_hwmon_ops,
	.info = tps65185_info,
};

static int tps65185_runtime_suspend(struct device *dev)
{
	int ret = 0;
	struct tps65185_data *data = dev_get_drvdata(dev);

	ret = regmap_read(data->regmap, TPS65185_REG_VCOM2, &data->vcom_msb);
	if (ret)
		return ret;

	if (data->wakeup_gpio) {
		ret = gpiod_set_value_cansleep(data->wakeup_gpio, 0);
		if (ret)
			return ret;
	}

	if (data->vin_reg) {
		ret = regulator_disable(data->vin_reg);
		if (ret)
			goto reenable_wkup;
	}

	regcache_mark_dirty(data->regmap);

	return 0;
reenable_wkup:
	if (data->wakeup_gpio)
		gpiod_set_value_cansleep(data->wakeup_gpio, 1);

	return ret;
}

static int tps65185_runtime_resume(struct device *dev)
{
	int ret = 0;
	struct tps65185_data *data = dev_get_drvdata(dev);

	if (data->vin_reg)
		ret = regulator_enable(data->vin_reg);

	if (ret)
		return ret;

	if (data->wakeup_gpio) {
		ret = gpiod_set_value_cansleep(data->wakeup_gpio, 1);
		usleep_range(2000, 4000);
	}

	if (ret)
		goto resume_wkup_err;

	ret = regcache_sync(data->regmap);
	if (ret)
		goto resume_sync_err;

	ret = regmap_update_bits(data->regmap, TPS65185_REG_VCOM2,
				 BIT(0), data->vcom_msb);
	if (ret)
		goto resume_sync_err;

	return 0;

resume_sync_err:
	gpiod_set_value_cansleep(data->wakeup_gpio, 0);

resume_wkup_err:
	regulator_disable(data->vin_reg);
	return ret;
}

static bool tps65185_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case TPS65185_REG_TMST_VALUE:
	case TPS65185_REG_ENABLE:
	case TPS65185_REG_VCOM2:
	case TPS65185_REG_INT1:
	case TPS65185_REG_INT2:
	case TPS65185_REG_TMST1:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = 0x10,
	.cache_type = REGCACHE_MAPLE,
	.volatile_reg = tps65185_volatile_reg,
};

static void disable_nopm(void *d)
{
	struct tps65185_data *data = d;

	tps65185_runtime_suspend(data->dev);
}

static int tps65185_v3p3_enable(struct regulator_dev *rdev)
{
	struct tps65185_data *data = rdev_get_drvdata(rdev);
	int ret;

	ret = pm_runtime_resume_and_get(data->dev);
	if (ret < 0)
		return ret;

	ret = regulator_enable_regmap(rdev);
	if (ret < 0)
		pm_runtime_put_autosuspend(data->dev);

	return ret;
}

static int tps65185_v3p3_disable(struct regulator_dev *rdev)
{
	struct tps65185_data *data = rdev_get_drvdata(rdev);
	int ret;

	ret = regulator_disable_regmap(rdev);
	pm_runtime_put_autosuspend(data->dev);

	return ret;
}

static int tps65185_v3p3_is_enabled(struct regulator_dev *rdev)
{
	struct tps65185_data *data = rdev_get_drvdata(rdev);
	int ret;

	if (pm_runtime_status_suspended(data->dev))
		return 0;

	ret = pm_runtime_resume_and_get(data->dev);
	if (ret < 0)
		return 0;

	ret = regulator_is_enabled_regmap(rdev);

	pm_runtime_put_autosuspend(data->dev);
	return ret;
}

static const struct regulator_ops tps65185_v3p3ops = {
	.list_voltage = regulator_list_voltage_linear,
	.enable = tps65185_v3p3_enable,
	.disable = tps65185_v3p3_disable,
	.is_enabled = tps65185_v3p3_is_enabled,
};

static int tps65185_check_powergood(struct regulator_dev *rdev)
{
	struct tps65185_data *data = rdev_get_drvdata(rdev);

	if (pm_runtime_status_suspended(data->dev))
		return 0;

	return gpiod_get_value_cansleep(data->pgood_gpio);
}

static int tps65185_vposneg_get_voltage_sel(struct regulator_dev *rdev)
{
	struct tps65185_data *data = rdev_get_drvdata(rdev);
	int ret;

	ret = pm_runtime_resume_and_get(data->dev);
	if (ret < 0)
		return ret;

	ret = regulator_get_voltage_sel_regmap(rdev);
	if (ret < 0)
		return ret;

	pm_runtime_put_autosuspend(data->dev);

	/* highest value is lowest voltage */
	return 6 - ret;
}

static int tps65185_vposneg_set_voltage_sel(struct regulator_dev *rdev, unsigned int selector)
{
	struct tps65185_data *data = rdev_get_drvdata(rdev);
	int ret;

	ret = pm_runtime_resume_and_get(data->dev);
	if (ret < 0)
		return ret;

	/* highest value is lowest voltage */
	ret = regulator_set_voltage_sel_regmap(rdev, 6 - selector);
	pm_runtime_put_autosuspend(data->dev);

	return ret;
}

static irqreturn_t pgood_handler(int irq, void *dev_id)
{
	struct tps65185_data *data = dev_id;

	complete(&data->pgood_completion);

	return IRQ_HANDLED;
}

static int tps65185_vposneg_enable(struct regulator_dev *rdev)
{
	struct tps65185_data *data = rdev_get_drvdata(rdev);
	int ret;

	ret = pm_runtime_resume_and_get(data->dev);
	if (ret < 0)
		return ret;

	reinit_completion(&data->pgood_completion);
	regmap_update_bits(data->regmap, TPS65185_REG_ENABLE,
			   TPS65185_ENABLE_ACTIVE,
			   TPS65185_ENABLE_ACTIVE);
	dev_dbg(data->dev, "turning on...");
	wait_for_completion_timeout(&data->pgood_completion,
				    msecs_to_jiffies(PGOOD_TIMEOUT_MSECS));
	dev_dbg(data->dev, "turned on");
	if (gpiod_get_value_cansleep(data->pgood_gpio) != 1) {
		pm_runtime_put_autosuspend(data->dev);
		return -ETIMEDOUT;
	}

	return 0;
}

static int tps65185_vposneg_disable(struct regulator_dev *rdev)
{
	struct tps65185_data *data = rdev_get_drvdata(rdev);

	regmap_update_bits(data->regmap, TPS65185_REG_ENABLE,
			   TPS65185_ENABLE_STANDBY,
			   TPS65185_ENABLE_STANDBY);
	pm_runtime_put_autosuspend(data->dev);
	return 0;
}

static int tps65185_vcom_set_voltage_sel(struct regulator_dev *rdev, unsigned int selector)
{
	struct tps65185_data *data = rdev_get_drvdata(rdev);
	int ret;

	ret = pm_runtime_resume_and_get(data->dev);
	if (ret < 0)
		return ret;

	ret = regmap_update_bits(data->regmap, TPS65185_REG_VCOM2, BIT(0), selector >> 8);
	if (ret < 0)
		return ret;

	ret = regmap_write(data->regmap, TPS65185_REG_VCOM1, selector & 0xFF);
	pm_runtime_put_autosuspend(data->dev);
	return ret;
}

static int tps65185_vcom_get_voltage_sel(struct regulator_dev *rdev)
{
	struct tps65185_data *data = rdev_get_drvdata(rdev);
	int ret;
	unsigned int sel, sel2;

	ret = pm_runtime_resume_and_get(data->dev);
	if (ret < 0)
		return ret;

	ret = regmap_read(data->regmap, TPS65185_REG_VCOM1, &sel);
	if (ret < 0)
		goto out;

	ret = regmap_read(data->regmap, TPS65185_REG_VCOM2, &sel2);
	if (ret < 0)
		goto out;

	if (sel2 & BIT(0))
		sel |= 0x100;

	ret = sel;
out:
	pm_runtime_put_autosuspend(data->dev);
	return ret;
}

static int tps65185_vcom_enable(struct regulator_dev *rdev)
{
	struct tps65185_data *data = rdev_get_drvdata(rdev);

	if (data->vcom_ctrl_gpio)
		return gpiod_set_value_cansleep(data->vcom_ctrl_gpio, 1);

	return 0;
}

static int tps65185_vcom_disable(struct regulator_dev *rdev)
{
	struct tps65185_data *data = rdev_get_drvdata(rdev);

	if (data->vcom_ctrl_gpio)
		return gpiod_set_value_cansleep(data->vcom_ctrl_gpio, 0);

	return 0;
}

static int tps65185_vcom_is_enabled(struct regulator_dev *rdev)
{
	struct tps65185_data *data = rdev_get_drvdata(rdev);
	int ret;

	ret = tps65185_check_powergood(rdev);
	if (ret <= 0)
		return ret;

	if (data->vcom_ctrl_gpio)
		ret = gpiod_get_value_cansleep(data->vcom_ctrl_gpio);

	return ret;
}

static const struct regulator_ops tps65185_vcom_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.enable = tps65185_vcom_enable,
	.disable = tps65185_vcom_disable,
	.is_enabled = tps65185_vcom_is_enabled,
	.set_voltage_sel = tps65185_vcom_set_voltage_sel,
	.get_voltage_sel = tps65185_vcom_get_voltage_sel,
};

static const struct regulator_ops tps65185_vposneg_ops = {
	.list_voltage = regulator_list_voltage_linear,
	.map_voltage = regulator_map_voltage_linear,
	.enable = tps65185_vposneg_enable,
	.disable = tps65185_vposneg_disable,
	.is_enabled = tps65185_check_powergood,
	.set_voltage_sel = tps65185_vposneg_set_voltage_sel,
	.get_voltage_sel = tps65185_vposneg_get_voltage_sel,
};

static const struct regulator_desc regulators[] = {
	{
		.name = "v3p3",
		.of_match = of_match_ptr("v3p3"),
		.regulators_node = of_match_ptr("regulators"),
		.id = 0,
		.ops = &tps65185_v3p3ops,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.enable_reg = TPS65185_REG_ENABLE,
		.enable_mask = BIT(5),
		.n_voltages = 1,
		.min_uV = 3300000,
	},
	{
		.name = "vposneg",
		.of_match = of_match_ptr("vposneg"),
		.regulators_node = of_match_ptr("regulators"),
		.id = 1,
		.ops = &tps65185_vposneg_ops,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.n_voltages = 4,
		.vsel_reg = TPS65185_REG_VADJ,
		.vsel_mask = 0x7,
		.min_uV = 14250000,
		.uV_step = 250000,
	},
	{
		.name = "vcom",
		.of_match = of_match_ptr("vcom"),
		.regulators_node = of_match_ptr("regulators"),
		.supply_name = "vposneg",
		.id = 2,
		.ops = &tps65185_vcom_ops,
		.type = REGULATOR_VOLTAGE,
		.owner = THIS_MODULE,
		.n_voltages = 511,
		.min_uV = 0,
		.uV_step = 10000,
	},
};

static irqreturn_t tps65185_irq_thread(int irq, void *dev_id)
{
	struct tps65185_data *data = dev_id;
	unsigned int int_status_1, int_status_2;
	int ret;

	/* read both status to have irq cleared */
	regmap_read(data->regmap, TPS65185_REG_INT1, &int_status_1);

	ret = regmap_read(data->regmap, TPS65185_REG_INT2, &int_status_2);
	if (!ret) {
		if (int_status_2 & BIT(0))
			complete(&data->tmst_completion);
	}

	dev_dbg(data->dev, "irq status %02x %02x\n", int_status_1, int_status_2);

	return IRQ_HANDLED;
}

static int tps65185_probe(struct i2c_client *client)
{
	struct tps65185_data *data;
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	int ret = 0;
	int i;

	data = devm_kzalloc(&client->dev, sizeof(*data), GFP_KERNEL);
	data->regmap = devm_regmap_init_i2c(client, &regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(&client->dev, PTR_ERR(data->regmap),
				     "failed to allocate regmap!\n");

	data->pgood_gpio = devm_gpiod_get(&client->dev, "pwr-good", GPIOD_IN);
	if (IS_ERR(data->pgood_gpio))
		return dev_err_probe(&client->dev,
				     PTR_ERR(data->pgood_gpio),
				     "failed to get power good gpio\n");

	data->pgood_irq = gpiod_to_irq(data->pgood_gpio);
	if (data->pgood_irq < 0)
		return data->pgood_irq;

	data->pwrup_gpio = devm_gpiod_get_optional(&client->dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(data->pwrup_gpio))
		return dev_err_probe(&client->dev, PTR_ERR(data->pwrup_gpio),
				     "failed to get pwrup gpio\n");

	data->wakeup_gpio = devm_gpiod_get_optional(&client->dev, "wakeup", GPIOD_OUT_LOW);
	if (IS_ERR(data->wakeup_gpio))
		return dev_err_probe(&client->dev,
				     PTR_ERR(data->wakeup_gpio),
				     "failed to get wakeup gpio\n");

	data->vcom_ctrl_gpio = devm_gpiod_get_optional(&client->dev, "vcom-ctrl", GPIOD_OUT_LOW);
	if (IS_ERR(data->vcom_ctrl_gpio))
		return dev_err_probe(&client->dev,
				     PTR_ERR(data->vcom_ctrl_gpio),
				     "failed to get vcm ctrl gpio\n");

	data->vin_reg = devm_regulator_get_optional(&client->dev, "vin");
	if (IS_ERR(data->vin_reg))
		return dev_err_probe(&client->dev, PTR_ERR(data->vin_reg),
				     "failed to get vin regulator\n");

	data->dev = &client->dev;
	i2c_set_clientdata(client, data);

	init_completion(&data->pgood_completion);
	init_completion(&data->tmst_completion);

	ret = devm_request_threaded_irq(&client->dev, data->pgood_irq, NULL,
					pgood_handler,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"PGOOD", data);
	if (ret)
		return dev_err_probe(&client->dev, ret,
				     "failed to request power good irq\n");

	if (client->irq) {
		ret = devm_request_threaded_irq(&client->dev, client->irq,
						NULL, tps65185_irq_thread,
						IRQF_TRIGGER_LOW | IRQF_ONESHOT,
						"tps65185", data);
		if (ret)
			return dev_err_probe(&client->dev, ret,
					     "failed to request irq\n");
	}

	if (IS_ENABLED(CONFIG_PM)) {
		devm_pm_runtime_enable(&client->dev);
		pm_runtime_set_autosuspend_delay(&client->dev, 4000);
		pm_runtime_use_autosuspend(&client->dev);
	} else {
		ret = tps65185_runtime_resume(&client->dev);
		if (ret < 0)
			return ret;

		devm_add_action_or_reset(&client->dev, disable_nopm, data);
	}

	ret = pm_runtime_resume_and_get(&client->dev);
	if (ret < 0)
		return ret;

	regmap_update_bits(data->regmap, TPS65185_REG_INT_EN2, BIT(0), BIT(0));
	pm_runtime_put_autosuspend(&client->dev);

	config.driver_data = data;
	config.dev = &client->dev;
	config.regmap = data->regmap;

	for (i = 0; i < ARRAY_SIZE(regulators); i++) {
		rdev = devm_regulator_register(&client->dev, &regulators[i],
					       &config);
		if (IS_ERR(rdev))
			return dev_err_probe(&client->dev, PTR_ERR(rdev),
					     "failed to register %s regulator\n",
					     regulators[i].name);
	}

	if (IS_REACHABLE(CONFIG_HWMON)) {
		struct device *hwmon_dev;

		hwmon_dev = devm_hwmon_device_register_with_info(&client->dev, "tps65185", data,
								 &tps65185_chip_info, NULL);
		if (IS_ERR(hwmon_dev))
			dev_notice(&client->dev, "failed to register hwmon\n");
	}

	return 0;
}

static const struct dev_pm_ops tps65185_pm_ops = {
	SET_RUNTIME_PM_OPS(tps65185_runtime_suspend, tps65185_runtime_resume, NULL)
};

static const struct of_device_id tps65185_dt_ids[] = {
	{
		.compatible = "ti,tps65185",
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, tps65185_dt_ids);

static struct i2c_driver tps65185_i2c_driver = {
	.driver = {
		   .name = "tps65185",
		   .of_match_table = tps65185_dt_ids,
		   .pm = &tps65185_pm_ops,
	},
	.probe = tps65185_probe,
};

module_i2c_driver(tps65185_i2c_driver);

/* Module information */
MODULE_DESCRIPTION("TPS65185 regulator driver");
MODULE_LICENSE("GPL");

