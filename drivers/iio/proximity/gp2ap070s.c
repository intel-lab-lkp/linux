// SPDX-License-Identifier: GPL-2.0-only
/*
 * IIO driver for Sharp GP2AP070S proximity sensor.
 *
 * Copyright (C) 2026 Kaustabh Chakraborty <kauschluss@disroot.org>
 */

#include <linux/i2c.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/types.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#define GP2AP070S_REG_COM1		0x80
#define GP2AP070S_REG_COM2		0x81
#define GP2AP070S_REG_COM3		0x82
#define GP2AP070S_REG_COM4		0x83
#define GP2AP070S_REG_PS1		0x85
#define GP2AP070S_REG_PS2		0x86
#define GP2AP070S_REG_PS3		0x87
#define GP2AP070S_REG_PS_THD_LO_LE16	0x88
#define GP2AP070S_REG_PS_THD_HI_LE16	0x8a
#define GP2AP070S_REG_OS_D0_LE16	0x8c
#define GP2AP070S_REG_D0_LE16		0x90

/* GP2AP070S_REG_COM1 */
#define GP2AP070S_VAL_COM1_WKUP		BIT(7)
#define GP2AP070S_VAL_COM1_EN		BIT(5)

/* GP2AP070S_REG_COM3 */
#define GP2AP070S_VAL_COM3_INT_PULSE	BIT(1)

/* GP2AP070S_REG_COM4 */
#define GP2AP070S_VAL_COM1_BLINK	GENMASK(2, 0)	/* LED Blink Interval */

#define GP2AP070S_VAL_COM1_BLINK_0ms	0
#define GP2AP070S_VAL_COM1_BLINK_2ms	1
#define GP2AP070S_VAL_COM1_BLINK_8ms	2
#define GP2AP070S_VAL_COM1_BLINK_33ms	3
#define GP2AP070S_VAL_COM1_BLINK_66ms	4
#define GP2AP070S_VAL_COM1_BLINK_131ms	5
#define GP2AP070S_VAL_COM1_BLINK_262ms	6
#define GP2AP070S_VAL_COM1_BLINK_524ms	7

/* GP2AP070S_REG_PS1 */
#define GP2AP070S_VAL_PS1_RESOL		GENMASK(5, 4)	/* Resolution */

#define GP2AP070S_VAL_PS1_RESOL_14ms	0
#define GP2AP070S_VAL_PS1_RESOL_12ms	1
#define GP2AP070S_VAL_PS1_RESOL_10ms	2
#define GP2AP070S_VAL_PS1_RESOL_8ms	3

/* GP2AP070S_REG_PS2 */
#define GP2AP070S_VAL_PS2_IOUT		GENMASK(6, 4)	/* Current Output */
#define GP2AP070S_VAL_PS2_SUM32		BIT(2)

#define GP2AP070S_VAL_PS2_IOUT_0mA	0
#define GP2AP070S_VAL_PS2_IOUT_24mA	1
#define GP2AP070S_VAL_PS2_IOUT_89mA	2
#define GP2AP070S_VAL_PS2_IOUT_130mA	3
#define GP2AP070S_VAL_PS2_IOUT_190mA	4

/* GP2AP070S_REG_PS3 */
#define GP2AP070S_VAL_PS3_PRST		GENMASK(6, 4)	/* Repeating Measurements */

struct gp2ap070s_drvdata {
	struct device *dev;
	struct regmap *regmap;
	struct regulator_bulk_data *regulators;
	u32 near_level;
};

static const struct regmap_config gp2ap070s_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static const char *const gp2ap070s_regulator_names[] = {
	"vdd",
	"vled",
};

static ssize_t gp2ap070s_iio_read_near_level(struct iio_dev *indio_dev,
					     uintptr_t priv,
					     const struct iio_chan_spec *chan,
					     char *buf)
{
	struct gp2ap070s_drvdata *drvdata = iio_priv(indio_dev);

	return sprintf(buf, "%u\n", drvdata->near_level);
}

static const struct iio_chan_spec_ext_info gp2ap070s_iio_chan_spec_ext_info[] = {
	{
		.name = "nearlevel",
		.shared = IIO_SEPARATE,
		.read = gp2ap070s_iio_read_near_level,
	},
	{ /* sentinel */ }
};

static const struct iio_event_spec gp2ap070s_iio_event_spec[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
};

static const struct iio_chan_spec gp2ap070s_iio_chan_spec[] = {
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.ext_info = gp2ap070s_iio_chan_spec_ext_info,
		.event_spec = gp2ap070s_iio_event_spec,
		.num_event_specs = ARRAY_SIZE(gp2ap070s_iio_event_spec),
	},
};

static int gp2ap070s_iio_read_raw(struct iio_dev *indio_dev,
				  struct iio_chan_spec const *chan, int *val,
				  int *val2, long mask)
{
	struct gp2ap070s_drvdata *drvdata = iio_priv(indio_dev);
	__le16 value;
	int ret;

	ret = regmap_raw_read(drvdata->regmap, GP2AP070S_REG_D0_LE16, &value,
			      sizeof(value));
	if (ret)
		return ret;

	*val = le16_to_cpu(value);
	return IIO_VAL_INT;
}

static int gp2ap070s_iio_read_event_value(struct iio_dev *indio_dev,
					  const struct iio_chan_spec *chan,
					  enum iio_event_type type,
					  enum iio_event_direction dir,
					  enum iio_event_info info, int *val,
					  int *val2)
{
	struct gp2ap070s_drvdata *drvdata = iio_priv(indio_dev);
	__le16 value;
	int ret;

	if (type != IIO_EV_TYPE_THRESH || info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_RISING:
		ret = regmap_raw_read(drvdata->regmap,
				      GP2AP070S_REG_PS_THD_HI_LE16, &value,
				      sizeof(value));
		if (ret)
			return ret;

		*val = le16_to_cpu(value);
		return IIO_VAL_INT;
	case IIO_EV_DIR_FALLING:
		ret = regmap_raw_read(drvdata->regmap,
				      GP2AP070S_REG_PS_THD_LO_LE16, &value,
				      sizeof(value));
		if (ret)
			return ret;

		*val = le16_to_cpu(value);
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int gp2ap070s_iio_write_event_value(struct iio_dev *indio_dev,
					   const struct iio_chan_spec *chan,
					   enum iio_event_type type,
					   enum iio_event_direction dir,
					   enum iio_event_info info, int val,
					   int val2)
{
	struct gp2ap070s_drvdata *drvdata = iio_priv(indio_dev);
	__le16 value;
	u16 threshold_other;
	int ret;

	if (type != IIO_EV_TYPE_THRESH || info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_RISING:
		ret = regmap_raw_read(drvdata->regmap,
				      GP2AP070S_REG_PS_THD_LO_LE16, &value,
				      sizeof(value));
		if (ret)
			return ret;

		/* Ensure lo_threshold < hi_threshold */
		threshold_other = le16_to_cpu(value);
		if (threshold_other >= val)
			return -EINVAL;

		value = cpu_to_le16((u16)val);
		ret = regmap_raw_write(drvdata->regmap,
				       GP2AP070S_REG_PS_THD_HI_LE16, &value,
				       sizeof(value));
		if (ret)
			return ret;

		return IIO_VAL_INT;
	case IIO_EV_DIR_FALLING:
		ret = regmap_raw_read(drvdata->regmap,
				      GP2AP070S_REG_PS_THD_HI_LE16, &value,
				      sizeof(value));
		if (ret)
			return ret;

		/* Ensure hi_threshold > lo_threshold */
		threshold_other = le16_to_cpu(value);
		if (threshold_other <= val)
			return -EINVAL;

		value = cpu_to_le16((u16)val);
		ret = regmap_raw_write(drvdata->regmap,
				       GP2AP070S_REG_PS_THD_LO_LE16, &value,
				       sizeof(value));
		if (ret)
			return ret;

		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int gp2ap070s_read_event_config(struct iio_dev *indio_dev,
				       const struct iio_chan_spec *chan,
				       enum iio_event_type type,
				       enum iio_event_direction dir)
{
	struct gp2ap070s_drvdata *drvdata = iio_priv(indio_dev);
	unsigned int value;
	int ret;

	if (type != IIO_EV_TYPE_THRESH)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_EITHER:
		ret = regmap_read(drvdata->regmap, GP2AP070S_REG_COM3, &value);
		if (ret)
			return ret;

		return !!(value & GP2AP070S_VAL_COM3_INT_PULSE);
	default:
		return -EINVAL;
	}

	return 0;
}

static int gp2ap070s_write_event_config(struct iio_dev *indio_dev,
					const struct iio_chan_spec *chan,
					enum iio_event_type type,
					enum iio_event_direction dir,
					bool state)
{
	struct gp2ap070s_drvdata *drvdata = iio_priv(indio_dev);
	int ret;

	if (type != IIO_EV_TYPE_THRESH)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_EITHER:
		if (state)
			ret = regmap_set_bits(drvdata->regmap,
					      GP2AP070S_REG_COM3,
					      GP2AP070S_VAL_COM3_INT_PULSE);
		else
			ret = regmap_clear_bits(drvdata->regmap,
						GP2AP070S_REG_COM3,
						GP2AP070S_VAL_COM3_INT_PULSE);
		if (ret)
			return ret;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct iio_info gp2ap070s_iio_info = {
	.read_raw = gp2ap070s_iio_read_raw,
	.read_event_value = gp2ap070s_iio_read_event_value,
	.write_event_value = gp2ap070s_iio_write_event_value,
	.read_event_config = gp2ap070s_read_event_config,
	.write_event_config = gp2ap070s_write_event_config,
};

static irqreturn_t gp2ap070s_irq_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct gp2ap070s_drvdata *drvdata = iio_priv(indio_dev);
	struct device *dev = drvdata->dev;
	u16 threshold_lo, threshold_hi, distance;
	__le16 value;
	int ret;

	ret = regmap_raw_read(drvdata->regmap, GP2AP070S_REG_PS_THD_LO_LE16,
			      &value, sizeof(value));
	if (ret) {
		dev_err(dev, "Failed to read low threshold value (%d)\n", ret);
		return IRQ_HANDLED;
	}

	threshold_lo = __le16_to_cpu(value);

	ret = regmap_raw_read(drvdata->regmap, GP2AP070S_REG_PS_THD_HI_LE16,
			      &value, sizeof(value));
	if (ret) {
		dev_err(dev, "Failed to read high threshold value (%d)\n", ret);
		return IRQ_HANDLED;
	}

	threshold_hi = __le16_to_cpu(value);

	ret = regmap_raw_read(drvdata->regmap, GP2AP070S_REG_D0_LE16, &value,
			      sizeof(value));
	if (ret) {
		dev_err(dev, "Failed to read proximity distance (%d)\n", ret);
		return IRQ_HANDLED;
	}

	distance = __le16_to_cpu(value);

	if (distance >= threshold_hi) {
		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, 0,
						    IIO_EV_TYPE_THRESH,
						    IIO_EV_DIR_RISING),
			       iio_get_time_ns(indio_dev));
	} else if (distance <= threshold_lo) {
		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(IIO_PROXIMITY, 0,
						    IIO_EV_TYPE_THRESH,
						    IIO_EV_DIR_FALLING),
			       iio_get_time_ns(indio_dev));
	} else {
		/*
		 * If none match, then it's stuck in the threshold band gap.
		 * The hardware generally does not issue interrupts here.
		 */
		WARN_ONCE(true, "spurious interrupt issued in proximity threshold gap\n");
	}

	return IRQ_HANDLED;
}

static int gp2ap070s_probe_hw_register(struct gp2ap070s_drvdata *drvdata)
{
	struct device *dev = drvdata->dev;
	int ret;

	ret = regmap_clear_bits(drvdata->regmap, GP2AP070S_REG_COM1, ~0);
	if (ret)
		return dev_err_probe(drvdata->dev, ret, "Failed to reset sensor\n");

	ret = regmap_clear_bits(drvdata->regmap, GP2AP070S_REG_COM2, ~0);
	if (ret)
		return dev_err_probe(drvdata->dev, ret, "Failed to unmask interrupts\n");

	ret = regmap_set_bits(drvdata->regmap, GP2AP070S_REG_COM3,
			      GP2AP070S_VAL_COM3_INT_PULSE);
	if (ret)
		return dev_err_probe(drvdata->dev, ret, "Failed to enable interrupt\n");

	ret = regmap_write_bits(drvdata->regmap, GP2AP070S_REG_COM4,
				GP2AP070S_VAL_COM1_BLINK,
				FIELD_PREP_CONST(GP2AP070S_VAL_COM1_BLINK,
						 GP2AP070S_VAL_COM1_BLINK_33ms));
	if (ret)
		return dev_err_probe(drvdata->dev, ret, "Failed to set LED blink timing\n");

	ret = regmap_write_bits(drvdata->regmap, GP2AP070S_REG_PS1,
				GP2AP070S_VAL_PS1_RESOL,
				FIELD_PREP_CONST(GP2AP070S_VAL_PS1_RESOL,
						 GP2AP070S_VAL_PS1_RESOL_10ms));
	if (ret)
		return dev_err_probe(drvdata->dev, ret, "Failed to set resolution\n");

	ret = regmap_write_bits(drvdata->regmap, GP2AP070S_REG_PS2,
				GP2AP070S_VAL_PS2_IOUT | GP2AP070S_VAL_PS2_SUM32,
				FIELD_PREP_CONST(GP2AP070S_VAL_PS2_IOUT,
						 GP2AP070S_VAL_PS2_IOUT_89mA) |
				GP2AP070S_VAL_PS2_SUM32);
	if (ret)
		return dev_err_probe(drvdata->dev, ret, "Failed to set current output\n");

	ret = regmap_write_bits(drvdata->regmap, GP2AP070S_REG_PS3,
				GP2AP070S_VAL_PS3_PRST,
				FIELD_PREP_CONST(GP2AP070S_VAL_PS3_PRST, 3));
	if (ret)
		return dev_err_probe(drvdata->dev, ret, "Failed to set measurement repetitions\n");

	ret = regmap_set_bits(drvdata->regmap, GP2AP070S_REG_COM1,
			      GP2AP070S_VAL_COM1_WKUP | GP2AP070S_VAL_COM1_EN);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable sensor\n");

	return 0;
}

static int gp2ap070s_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct gp2ap070s_drvdata *drvdata;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*drvdata));
	if (!indio_dev)
		return -ENOMEM;

	drvdata = iio_priv(indio_dev);
	i2c_set_clientdata(client, drvdata);

	drvdata->dev = dev;
	drvdata->regmap = devm_regmap_init_i2c(client, &gp2ap070s_regmap_config);
	if (IS_ERR(drvdata->regmap))
		return dev_err_probe(dev, PTR_ERR(drvdata->regmap), "Failed to create regmap\n");

	ret = devm_regulator_bulk_get_enable(dev,
					     ARRAY_SIZE(gp2ap070s_regulator_names),
					     gp2ap070s_regulator_names);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get and enable regulators");

	usleep_range(10000, 11000);

	indio_dev->name = "gp2ap070s";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = gp2ap070s_iio_chan_spec;
	indio_dev->num_channels = ARRAY_SIZE(gp2ap070s_iio_chan_spec);
	indio_dev->info = &gp2ap070s_iio_info;

	device_property_read_u32(&client->dev, "proximity-near-level",
				 &drvdata->near_level);

	ret = gp2ap070s_probe_hw_register(drvdata);
	if (ret)
		return ret;

	ret = devm_request_threaded_irq(dev, client->irq, NULL,
					gp2ap070s_irq_handler, IRQF_ONESHOT,
					"gp2ap070s-irq", indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to request IRQ");

	return devm_iio_device_register(&client->dev, indio_dev);
}

static const struct of_device_id gp2ap070s_of_device_id[] = {
	{ .compatible = "sharp,gp2ap070s" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gp2ap070s_of_device_id);

static const struct i2c_device_id gp2ap070s_i2c_device_id[] = {
	{ .name = "gp2ap070s" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, gp2ap070s_i2c_device_id);

static struct i2c_driver gp2ap070s_i2c_driver = {
	.driver = {
		.name = "gp2ap070s",
		.of_match_table = gp2ap070s_of_device_id,
	},
	.probe = gp2ap070s_probe,
	.id_table = gp2ap070s_i2c_device_id,
};
module_i2c_driver(gp2ap070s_i2c_driver);

MODULE_AUTHOR("Kaustabh Chakraborty <kauschluss@disroot.org>");
MODULE_DESCRIPTION("Sharp GP2AP070S Proximity Sensor");
MODULE_LICENSE("GPL");
