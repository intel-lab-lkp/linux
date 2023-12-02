// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for AMS AS6200 Temperature sensor
 *
 * Author: Abdel Alkuor <alkuor@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/kstrtox.h>
#include <linux/module.h>
#include <linux/regmap.h>

#include <linux/iio/buffer.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

#define AS6200_TVAL_REG		0x0
#define AS6200_CONFIG_REG	0x1
#define AS6200_TLOW_REG		0x2
#define AS6200_THIGH_REG	0x3

#define AS6200_CONFIG_AL	BIT(5)
#define AS6200_CONFIG_CR	GENMASK(7, 6)
#define AS6200_CONFIG_SM	BIT(8)
#define AS6200_CONFIG_IM	BIT(9)
#define AS6200_CONFIG_POL	BIT(10)
#define AS6200_CONFIG_CF	GENMASK(12, 11)

#define AS6200_TEMP_MASK	GENMASK(15, 4)

struct as6200 {
	struct regmap *regmap;
	struct mutex lock; /* Prevent concurrent temp fault processing */
};

static const int as6200_samp_freq[4][2] = {
	{ 0, 250000 },
	{ 1, 0 },
	{ 4, 0 },
	{ 8, 0 }
};

/* Consective faults converted to period */
static const int as6200_temp_thresh_periods[4][4][2] = {
	{ { 4, 0 }, { 8, 0 }, { 16, 0 }, { 24, 0 } },
	{ { 1, 0 }, { 2, 0 }, { 4, 0 }, { 6, 0 } },
	{ { 0, 250000 }, { 0, 500000 }, { 1, 0 }, { 2, 0} },
	{ { 0, 125000 }, { 0, 250000 }, { 0, 500000 }, { 0, 750000 } }
};

static const struct iio_event_spec as6200_temp_event[] = {
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
		.mask_separate = BIT(IIO_EV_INFO_PERIOD),
	}
};

static const struct iio_chan_spec as6200_channels[] = {
	{
		.type = IIO_TEMP,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.scan_type = {
			.sign = 's',
			.realbits = 12,
			.storagebits = 16,
			.shift = 4,
		},
		.event_spec = as6200_temp_event,
		.num_event_specs = ARRAY_SIZE(as6200_temp_event),
	},
	IIO_CHAN_SOFT_TIMESTAMP(1),
};

static const struct regmap_config as6200_regmap_config = {
	.reg_bits = 8,
	.val_bits = 16,
	.max_register = 0x7F,
};

static int as6200_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val,
			   int *val2,
			   long mask)
{
	struct as6200 *as = iio_priv(indio_dev);
	unsigned int reg;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_read(as->regmap, AS6200_TVAL_REG, &reg);
		if (ret)
			return ret;

		*val = sign_extend32(FIELD_GET(AS6200_TEMP_MASK, reg), 11);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = 62500;
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = regmap_read(as->regmap, AS6200_CONFIG_REG, &reg);
		if (ret)
			return ret;

		reg = FIELD_GET(AS6200_CONFIG_CR, reg);
		*val = as6200_samp_freq[reg][0];
		*val2 = as6200_samp_freq[reg][1];
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int as6200_read_avail(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     const int **vals, int *type, int *length,
			     long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*type = IIO_VAL_INT_PLUS_MICRO;
		*length = ARRAY_SIZE(as6200_samp_freq) * 2;
		*vals = (int *)as6200_samp_freq;
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static int as6200_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val,
			    int val2,
			    long mask)
{
	struct as6200 *as = iio_priv(indio_dev);
	int cr;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		for (cr = 0; cr < ARRAY_SIZE(as6200_samp_freq); cr++) {
			if (val == as6200_samp_freq[cr][0] &&
			    val2 == as6200_samp_freq[cr][1])
				break;
		}

		if (cr == ARRAY_SIZE(as6200_samp_freq))
			return -EINVAL;

		return regmap_update_bits(as->regmap, AS6200_CONFIG_REG,
					  AS6200_CONFIG_CR,
					  FIELD_PREP(AS6200_CONFIG_CR, cr));
	default:
		return -EINVAL;
	}
}

static int as6200_read_event_value(struct iio_dev *indio_dev,
				   const struct iio_chan_spec *chan,
				   enum iio_event_type type,
				   enum iio_event_direction dir,
				   enum iio_event_info info,
				   int *val, int *val2)
{
	struct as6200 *as = iio_priv(indio_dev);
	unsigned int reg;
	unsigned int tmp;
	int ret;
	u8 cf;
	u8 cr;

	switch (dir) {
	case IIO_EV_DIR_FALLING:
		reg = AS6200_TLOW_REG;
		break;
	case IIO_EV_DIR_RISING:
		reg = AS6200_THIGH_REG;
		break;
	case IIO_EV_DIR_EITHER:
		reg = AS6200_CONFIG_REG;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_read(as->regmap, reg, &tmp);
	if (ret)
		return ret;

	if (info == IIO_EV_INFO_VALUE) {
		*val = sign_extend32(FIELD_GET(AS6200_TEMP_MASK, tmp), 11);
		ret = IIO_VAL_INT;
	} else {
		cf = FIELD_GET(AS6200_CONFIG_CF, tmp);
		cr = FIELD_GET(AS6200_CONFIG_CR, tmp);
		*val = as6200_temp_thresh_periods[cr][cf][0];
		*val2 = as6200_temp_thresh_periods[cr][cf][1];
		ret = IIO_VAL_INT_PLUS_MICRO;
	}

	return ret;
}

static int as6200_write_event_value(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info,
				    int val, int val2)
{
	struct as6200 *as = iio_priv(indio_dev);
	unsigned int tmp;
	unsigned int reg;
	int ret;
	u8 cr;
	u8 cf;

	switch (dir) {
	case IIO_EV_DIR_FALLING:
		reg = AS6200_TLOW_REG;
		break;
	case IIO_EV_DIR_RISING:
		reg = AS6200_THIGH_REG;
		break;
	case IIO_EV_TYPE_THRESH:
		reg = AS6200_CONFIG_REG;
		break;
	default:
		return -EINVAL;
	}

	if (info == IIO_EV_INFO_VALUE) {
		/*
		 * range without applying the scaling
		 * factor of 0.06250
		 */
		if (val > 2000 || val < -640)
			return -EINVAL;

		tmp = FIELD_PREP(AS6200_TEMP_MASK, val);
	} else {
		ret = regmap_read(as->regmap, reg, &tmp);
		if (ret)
			return ret;

		cr = FIELD_GET(AS6200_CONFIG_CR, tmp);

		for (cf = 0; cf < ARRAY_SIZE(as6200_temp_thresh_periods); cf++) {
			if (val == as6200_temp_thresh_periods[cr][cf][0] &&
			    val2 == as6200_temp_thresh_periods[cr][cf][1])
				break;
		}

		if (cf == ARRAY_SIZE(as6200_temp_thresh_periods))
			return -EINVAL;

		tmp &= ~AS6200_CONFIG_CF;
		tmp |= FIELD_PREP(AS6200_CONFIG_CF, cf);
	}

	return regmap_write(as->regmap, reg, tmp);
}

static irqreturn_t as6200_event_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct as6200 *as = iio_priv(indio_dev);
	unsigned int alert;
	enum iio_event_direction dir;
	int ret;

	guard(mutex)(&as->lock);

	ret = regmap_read(as->regmap, AS6200_CONFIG_REG, &alert);
	if (ret)
		return IRQ_NONE;

	alert = FIELD_GET(AS6200_CONFIG_AL, alert);

	dir = alert ? IIO_EV_DIR_FALLING : IIO_EV_DIR_RISING;

	iio_push_event(indio_dev,
		       IIO_EVENT_CODE(IIO_TEMP, 0, 0,
				      dir,
				      IIO_EV_TYPE_THRESH,
				      0, 0, 0),
		       iio_get_time_ns(indio_dev));

	return IRQ_HANDLED;
}

static irqreturn_t as6200_trigger_handler(int irq, void *private)
{
	struct iio_poll_func *pf = private;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct as6200 *as = iio_priv(indio_dev);
	int ret;
	u8 data[16];

	ret = regmap_read(as->regmap, AS6200_TVAL_REG, (unsigned int *)data);
	if (!ret)
		iio_push_to_buffers_with_timestamp(indio_dev, data,
						   iio_get_time_ns(indio_dev));

	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static ssize_t
in_temp_period_available_show(struct device *dev,
			      struct device_attribute *attr,
			      char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct as6200 *as = iio_priv(indio_dev);
	int len = 0;
	unsigned int cr;
	u8 cf;
	int ret;

	ret = regmap_read(as->regmap, AS6200_CONFIG_REG, &cr);
	if (ret)
		return ret;

	cr = FIELD_GET(AS6200_CONFIG_CR, cr);

	for (cf = 0; cf < ARRAY_SIZE(as6200_temp_thresh_periods); cf++)
		len += sprintf(buf + len, "%d.%06d ",
			       as6200_temp_thresh_periods[cr][cf][0],
			       as6200_temp_thresh_periods[cr][cf][1]);

	len += sprintf(buf + len, "\n");

	return len;
}

static IIO_DEVICE_ATTR_RO(in_temp_period_available, 0);

static struct attribute *as6200_event_attributes[] = {
	&iio_dev_attr_in_temp_period_available.dev_attr.attr,
	NULL
};

static const struct attribute_group as6200_event_attribute_group = {
	.attrs = as6200_event_attributes,
};

static const struct iio_info as6200_temp_info = {
	.event_attrs = &as6200_event_attribute_group,
	.read_raw = &as6200_read_raw,
	.read_avail = &as6200_read_avail,
	.write_raw = &as6200_write_raw,
	.read_event_value = &as6200_read_event_value,
	.write_event_value = &as6200_write_event_value,
};

static int as6200_probe(struct i2c_client *client)
{
	struct as6200 *as;
	struct iio_dev *indio_dev;
	int ret;
	struct device *dev = &client->dev;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EINVAL;

	indio_dev = devm_iio_device_alloc(&client->dev, sizeof(*as));
	if (!indio_dev)
		return -ENOMEM;

	as = iio_priv(indio_dev);

	as->regmap = devm_regmap_init_i2c(client, &as6200_regmap_config);
	if (IS_ERR(as->regmap))
		return PTR_ERR(as->regmap);

	mutex_init(&as->lock);

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = as6200_channels;
	indio_dev->num_channels = ARRAY_SIZE(as6200_channels);
	indio_dev->name = "as6200";
	indio_dev->info = &as6200_temp_info;

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      NULL,
					      as6200_trigger_handler,
					      NULL);
	if (ret)
		return ret;

	if (client->irq) {
		ret = devm_request_threaded_irq(dev,
						client->irq,
						NULL,
						&as6200_event_handler,
						IRQF_ONESHOT,
						client->name,
						indio_dev);
		if (ret)
			return ret;
	}

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret,
				     "Could not get and enable regulator %d\n",
				     ret);

	i2c_set_clientdata(client, indio_dev);

	return devm_iio_device_register(dev, indio_dev);
}

static int __maybe_unused as6200_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct as6200 *as = iio_priv(i2c_get_clientdata(client));

	if (client->irq)
		disable_irq(client->irq);

	return regmap_update_bits(as->regmap, AS6200_CONFIG_REG,
				  AS6200_CONFIG_SM, AS6200_CONFIG_SM);
}

static int __maybe_unused as6200_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct as6200 *as = iio_priv(i2c_get_clientdata(client));

	if (client->irq)
		enable_irq(client->irq);

	return regmap_update_bits(as->regmap, AS6200_CONFIG_REG,
				  AS6200_CONFIG_SM, 0);
}

static const struct dev_pm_ops as6200_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(as6200_suspend, as6200_resume)
};

static const struct i2c_device_id as6200_id_table[] = {
	{ "as6200" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, as6200_id_table);

static const struct of_device_id as6200_of_match[] = {
	{ .compatible = "ams,as6200" },
	{ }
};
MODULE_DEVICE_TABLE(of, as6200_of_match);

static struct i2c_driver as6200_driver = {
	.driver = {
		.name = "as6200",
		.pm = pm_sleep_ptr(&as6200_pm_ops),
		.of_match_table = as6200_of_match,
	},
	.probe = as6200_probe,
	.id_table = as6200_id_table,
};
module_i2c_driver(as6200_driver);

MODULE_AUTHOR("Abdel Alkuor <alkuor@gmail.com");
MODULE_DESCRIPTION("AMS AS6200 Temperature Sensor");
MODULE_LICENSE("GPL");
