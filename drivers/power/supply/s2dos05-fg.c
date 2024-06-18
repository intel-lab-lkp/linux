// SPDX-License-Identifier: GPL-2.0+
/*
 * s2dos05-fg.c
 *
 * Copyright (c) 2015 Samsung Electronics Co., Ltd
 *              http://www.samsung.com
 * Copyright (c) 2024 Dzmitry Sankouski <dsankouski@gmail.com>
 *
 */
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/mfd/samsung/s2dos-core.h>
#include <linux/mfd/samsung/s2dos05.h>

#define SYNC_MODE	1
#define ASYNC_MODE	2

struct s2dos05_fg {
	struct regmap *regmap;
	struct device *dev;
	u8 adc_sync_mode;
	struct power_supply	*psy_elvdd;
	struct power_supply	*psy_elvss;
	struct power_supply	*psy_avdd;
	struct power_supply	*psy_buck;
	struct power_supply	*psy_ldo1;
	struct power_supply	*psy_ldo2;
	struct power_supply	*psy_ldo3;
	struct power_supply	*psy_ldo4;
};

static const unsigned int power_coeffs[8] = {POWER_ELVDD, POWER_ELVSS, POWER_AVDD,
	POWER_BUCK, POWER_L1, POWER_L2, POWER_L3, POWER_L4};

static void s2dos05_is_online(struct s2dos05_fg *drv_data, int *val)
{
	unsigned int adc_ctrl2;

	regmap_read(drv_data->regmap, S2DOS05_REG_PWRMT_CTRL2, &adc_ctrl2);
	if ((adc_ctrl2 & ADC_EN_MASK) > 0)
		*val = 1;
	else
		*val = 0;
}

static void s2dos05_start_measurement_if_async(struct s2dos05_fg *drv_data, unsigned int channel)
{
	unsigned int temp;

	if (drv_data->adc_sync_mode == ASYNC_MODE) {
		regmap_read(drv_data->regmap, S2DOS05_REG_PWRMT_CTRL1, &temp);
		if (!(temp & PWRMT_EN_CHK))
			return;

		regmap_update_bits(drv_data->regmap, S2DOS05_REG_PWRMT_CTRL1,
							ADC_ASYNCRD_MASK, ADC_ASYNCRD_MASK);
		usleep_range(2000, 2100);
	}
}

static void s2dos05_adc_read_power(struct s2dos05_fg *drv_data, unsigned int channel, int *val)
{
	unsigned int data_l, data_h, adc_val;

	regmap_update_bits(drv_data->regmap, S2DOS05_REG_PWRMT_CTRL2, ADC_PTR_MASK,
				2*channel);
	regmap_read(drv_data->regmap, S2DOS05_REG_PWRMT_DATA,
				&data_l);

	regmap_update_bits(drv_data->regmap, S2DOS05_REG_PWRMT_CTRL2, ADC_PTR_MASK,
				2*channel+1);
	regmap_read(drv_data->regmap, S2DOS05_REG_PWRMT_DATA,
				&data_h);

	adc_val = ((data_h & 0xff) << 8) | (data_l & 0xff);
	*val = adc_val * power_coeffs[channel] / 100;
}

static int s2dos05_get_adc_validity(struct s2dos05_fg *drv_data)
{
	unsigned int adc_validity;

	regmap_read(drv_data->regmap, S2DOS05_REG_OCL, &adc_validity);
	return !!(adc_validity | ADC_VALID_MASK);
}

static ssize_t adc_en_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s2dos05_fg *drv_data = dev_get_drvdata(dev);
	unsigned int adc_ctrl3;

	regmap_read(drv_data->regmap, S2DOS05_REG_PWRMT_CTRL2, &adc_ctrl3);
	if ((adc_ctrl3 & ADC_EN_MASK) > 0)
		return snprintf(buf, PAGE_SIZE, "ADC enable (%x)\n", adc_ctrl3);
	else
		return snprintf(buf, PAGE_SIZE, "ADC disable (%x)\n", adc_ctrl3);
}

static ssize_t adc_en_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct s2dos05_fg *drv_data = dev_get_drvdata(dev);
	int ret;
	unsigned int temp, val;

	ret = kstrtouint(buf, 16, &temp);
	if (ret)
		return -EINVAL;

	switch (temp) {
	case 0:
		val = 0x00;
		break;
	case 1:
		val = 0x80;
		break;
	default:
		val = 0x00;
		break;
	}

	regmap_update_bits(drv_data->regmap, S2DOS05_REG_PWRMT_CTRL2, ADC_EN_MASK,
				val);
	return count;
}

static ssize_t adc_sync_mode_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct s2dos05_fg *drv_data = dev_get_drvdata(dev);

	switch (drv_data->adc_sync_mode) {
	case SYNC_MODE:
		return snprintf(buf, PAGE_SIZE, "SYNC_MODE (%d)\n", drv_data->adc_sync_mode);
	case ASYNC_MODE:
		return snprintf(buf, PAGE_SIZE, "ASYNC_MODE (%d)\n", drv_data->adc_sync_mode);
	default:
		return snprintf(buf, PAGE_SIZE, "error (%d)\n", drv_data->adc_sync_mode);
	}
}

static ssize_t adc_sync_mode_store(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct s2dos05_fg *drv_data = dev_get_drvdata(dev);
	int ret;
	u8 temp;

	ret = kstrtou8(buf, 16, &temp);
	if (ret)
		return -EINVAL;

	switch (temp) {
	case SYNC_MODE:
		drv_data->adc_sync_mode = 1;
		break;
	case ASYNC_MODE:
		drv_data->adc_sync_mode = 2;
		break;
	default:
		drv_data->adc_sync_mode = 1;
		break;
	}

	return count;
}

static DEVICE_ATTR_RW(adc_en);
static DEVICE_ATTR_RW(adc_sync_mode);

static int s2dos05_fg_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val,
					 unsigned int channel
					 )
{
	struct s2dos05_fg *drv_data = power_supply_get_drvdata(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		s2dos05_is_online(drv_data, &val->intval);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (s2dos05_get_adc_validity(drv_data))
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		else
			val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		s2dos05_start_measurement_if_async(drv_data, channel);
		s2dos05_adc_read_power(drv_data, channel, &val->intval);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int s2dos05_fg_get_property_elvdd(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val
					 )
{
	return s2dos05_fg_get_property(psy, psp, val, CHANNEL_ELVDD);
}

static int s2dos05_fg_get_property_elvss(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val
					 )
{
	return s2dos05_fg_get_property(psy, psp, val, CHANNEL_ELVSS);
}

static int s2dos05_fg_get_property_avdd(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val
					 )
{
	return s2dos05_fg_get_property(psy, psp, val, CHANNEL_AVDD);
}

static int s2dos05_fg_get_property_buck(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val
					 )
{
	return s2dos05_fg_get_property(psy, psp, val, CHANNEL_BUCK);
}

static int s2dos05_fg_get_property_ldo1(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val
					 )
{
	return s2dos05_fg_get_property(psy, psp, val, CHANNEL_L1);
}

static int s2dos05_fg_get_property_ldo2(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val
					 )
{
	return s2dos05_fg_get_property(psy, psp, val, CHANNEL_L2);
}

static int s2dos05_fg_get_property_ldo3(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val
					 )
{
	return s2dos05_fg_get_property(psy, psp, val, CHANNEL_L3);
}

static int s2dos05_fg_get_property_ldo4(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val
					 )
{
	return s2dos05_fg_get_property(psy, psp, val, CHANNEL_L4);
}

static void s2dos05_powermeter_init(struct s2dos05_fg *drv_data)
{
	/*  SMP_NUM = 1100(16384) ~16s in case of aync mode */
	regmap_write(drv_data->regmap, S2DOS05_REG_PWRMT_CTRL1, 0x0C);
	regmap_update_bits(drv_data->regmap, S2DOS05_REG_PWRMT_CTRL2, POWER_MODE,
			POWER_MODE);
	/* ADC EN */
	regmap_update_bits(drv_data->regmap, S2DOS05_REG_PWRMT_CTRL2,
			ADC_EN_MASK, ADC_EN_MASK);

}

static void s2dos05_powermeter_deinit(struct s2dos05_fg *s2dos05)
{
	/* ADC turned off */
	regmap_write(s2dos05->regmap, S2DOS05_REG_PWRMT_CTRL2, 0);
}

static enum power_supply_property s2dos05_fg_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_POWER_NOW,
};

static const struct power_supply_desc s2dos05_elvdd_fg_desc = {
	.name		= "s2dos05_elvdd",
	.type		= POWER_SUPPLY_TYPE_UNKNOWN,
	.get_property	= s2dos05_fg_get_property_elvdd,
	.properties	= s2dos05_fg_properties,
	.num_properties	= ARRAY_SIZE(s2dos05_fg_properties),
};

static const struct power_supply_desc s2dos05_elvss_fg_desc = {
	.name		= "s2dos05_elvss",
	.type		= POWER_SUPPLY_TYPE_UNKNOWN,
	.get_property	= s2dos05_fg_get_property_elvss,
	.properties	= s2dos05_fg_properties,
	.num_properties	= ARRAY_SIZE(s2dos05_fg_properties),
};

static const struct power_supply_desc s2dos05_avdd_fg_desc = {
	.name		= "s2dos05_avdd",
	.type		= POWER_SUPPLY_TYPE_UNKNOWN,
	.get_property	= s2dos05_fg_get_property_avdd,
	.properties	= s2dos05_fg_properties,
	.num_properties	= ARRAY_SIZE(s2dos05_fg_properties),
};

static const struct power_supply_desc s2dos05_buck_fg_desc = {
	.name		= "s2dos05_buck",
	.type		= POWER_SUPPLY_TYPE_UNKNOWN,
	.get_property	= s2dos05_fg_get_property_buck,
	.properties	= s2dos05_fg_properties,
	.num_properties	= ARRAY_SIZE(s2dos05_fg_properties),
};

static const struct power_supply_desc s2dos05_ldo1_fg_desc = {
	.name		= "s2dos05_ldo1",
	.type		= POWER_SUPPLY_TYPE_UNKNOWN,
	.get_property	= s2dos05_fg_get_property_ldo1,
	.properties	= s2dos05_fg_properties,
	.num_properties	= ARRAY_SIZE(s2dos05_fg_properties),
};

static const struct power_supply_desc s2dos05_ldo2_fg_desc = {
	.name		= "s2dos05_ldo2",
	.type		= POWER_SUPPLY_TYPE_UNKNOWN,
	.get_property	= s2dos05_fg_get_property_ldo2,
	.properties	= s2dos05_fg_properties,
	.num_properties	= ARRAY_SIZE(s2dos05_fg_properties),
};

static const struct power_supply_desc s2dos05_ldo3_fg_desc = {
	.name		= "s2dos05_ldo3",
	.type		= POWER_SUPPLY_TYPE_UNKNOWN,
	.get_property	= s2dos05_fg_get_property_ldo3,
	.properties	= s2dos05_fg_properties,
	.num_properties	= ARRAY_SIZE(s2dos05_fg_properties),
};

static const struct power_supply_desc s2dos05_ldo4_fg_desc = {
	.name		= "s2dos05_ldo4",
	.type		= POWER_SUPPLY_TYPE_UNKNOWN,
	.get_property	= s2dos05_fg_get_property_ldo4,
	.properties	= s2dos05_fg_properties,
	.num_properties	= ARRAY_SIZE(s2dos05_fg_properties),
};

static int s2dos05_fuelgauge_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct s2dos_core *iodev = dev_get_drvdata(dev->parent);
	struct s2dos05_fg	*drv_data;
	struct power_supply_config pscfg = {};
	int ret;

	drv_data = devm_kzalloc(dev, sizeof(struct s2dos05_fg),
							GFP_KERNEL);
	if (!drv_data)
		ret = -ENOMEM;

	drv_data->regmap = iodev->regmap;
	s2dos05_powermeter_init(drv_data);
	pscfg.drv_data = drv_data;

	drv_data->psy_elvdd = devm_power_supply_register(dev, &s2dos05_elvdd_fg_desc, &pscfg);
	drv_data->psy_elvss = devm_power_supply_register(dev, &s2dos05_elvss_fg_desc, &pscfg);
	drv_data->psy_avdd = devm_power_supply_register(dev, &s2dos05_avdd_fg_desc, &pscfg);
	drv_data->psy_buck = devm_power_supply_register(dev, &s2dos05_buck_fg_desc, &pscfg);
	drv_data->psy_ldo1 = devm_power_supply_register(dev, &s2dos05_ldo1_fg_desc, &pscfg);
	drv_data->psy_ldo2 = devm_power_supply_register(dev, &s2dos05_ldo2_fg_desc, &pscfg);
	drv_data->psy_ldo3 = devm_power_supply_register(dev, &s2dos05_ldo3_fg_desc, &pscfg);
	drv_data->psy_ldo4 = devm_power_supply_register(dev, &s2dos05_ldo4_fg_desc, &pscfg);

	platform_set_drvdata(pdev, drv_data);

	ret = device_create_file(&pdev->dev, &dev_attr_adc_en);
	if (ret) {
		dev_err(dev, "failed: create adc enable sysfs entry\n");
		goto err;
	}
	ret = device_create_file(&pdev->dev, &dev_attr_adc_sync_mode);
	if (ret) {
		dev_err(dev, "failed: create adc sync mode sysfs entry\n");
		goto err;
	}

err:
	device_remove_file(dev, &dev_attr_adc_en);
	device_remove_file(dev, &dev_attr_adc_sync_mode);

	return 0;
}

static void s2dos05_fuelgauge_remove(struct platform_device *pdev)
{
	struct s2dos05_fg *info = platform_get_drvdata(pdev);

	s2dos05_powermeter_deinit(info);

	device_remove_file(&pdev->dev, &dev_attr_adc_en);
	device_remove_file(&pdev->dev, &dev_attr_adc_sync_mode);
}

static const struct platform_device_id s2dos05_platform_ids[] = {
	{"s2dos05-fg", 0},
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(platform, s2dos05_platform_ids);

static struct platform_driver s2dos05_platform_driver = {
	.driver = {
		.name = "s2dos05-fg",
	},
	.probe = s2dos05_fuelgauge_probe,
	.id_table = s2dos05_platform_ids,
	.remove_new = s2dos05_fuelgauge_remove,
};
module_platform_driver(s2dos05_platform_driver);

MODULE_DESCRIPTION("s2dos05 power meter");
MODULE_AUTHOR("Dzmitry Sankouski <dsankouski@gmail.com>");
MODULE_LICENSE("GPL");
