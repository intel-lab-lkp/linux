// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for AMS AS6200 Temperature sensor
 *
 * Auther: Abdel Alkuor <alkuor@gmail.com>
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/regmap.h>
#include <linux/interrupt.h>
#include <linux/iio/iio.h>
#include <linux/iio/events.h>
#include <linux/iio/trigger.h>
#include <linux/iio/buffer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/sysfs.h>
#include <linux/device.h>
#include <linux/bitfield.h>
#include <linux/kstrtox.h>

#define AS_TVAL_REG	0x0
#define AS_CONFIG_REG	0x1
#define AS_TLOW_REG	0x2
#define AS_THIGH_REG	0x3

/* AS_CONFIG_REG */
#define AS_CONFIG_AL	BIT(5)
#define AS_CONFIG_CR	GENMASK(7, 6)
#define AS_CONFIG_SM	BIT(8)
#define AS_CONFIG_IM	BIT(9)
#define AS_CONFIG_POL	BIT(10)
#define AS_CONFIG_CF	GENMASK(12, 11)

#define AS_TEMP_SCALE		62500

struct as6200_freq {
	int val;
	int val2;
};

struct as6200 {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock;
	struct iio_dev *indio_dev;

	unsigned int data[3];
};

static const struct as6200_freq as6200_samp_freq[4] = {
	{0, 250000},
	{1, 0},
	{4, 0},
	{8, 0},
};

static const struct iio_event_spec as6200_temp_event[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE)
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE)
	},
};

static const struct iio_chan_spec as6200_temp_channels[] = {
	{
		.type = IIO_TEMP,
		.indexed = 0,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.scan_index = 0,
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

static int
as6200_modify_config_reg(struct as6200 *as, unsigned int mask, unsigned int val)
{
	int ret;
	unsigned int reg;

	ret = regmap_read(as->regmap, AS_CONFIG_REG, &reg);
	if (ret)
		return ret;

	reg &= ~mask;
	reg |= val << (ffs(mask) - 1);

	return regmap_write(as->regmap, AS_CONFIG_REG, reg);
}

static int
as6200_read_config_reg(struct as6200 *as, unsigned int mask, unsigned int *val)
{
	int ret;
	unsigned int reg;

	ret = regmap_read(as->regmap, AS_CONFIG_REG, &reg);
	if (ret)
		return ret;

	*val = (reg & mask) >> (ffs(mask) - 1);

	return 0;
}

static int as6200_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val,
			   int *val2,
			   long mask)
{
	struct as6200 *as = iio_device_get_drvdata(indio_dev);
	unsigned int reg;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = regmap_read(as->regmap, AS_TVAL_REG, &reg);
		if (ret)
			return ret;

		*val = sign_extend32(reg >> 4, 11);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = AS_TEMP_SCALE;
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = as6200_read_config_reg(as, AS_CONFIG_CR, &reg);
		if (ret)
			return ret;

		*val = as6200_samp_freq[reg].val;
		*val2 = as6200_samp_freq[reg].val2;
		return IIO_VAL_INT_PLUS_MICRO;
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
	struct as6200 *as = iio_device_get_drvdata(indio_dev);
	int i;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		for (i = 0; i < ARRAY_SIZE(as6200_samp_freq); i++) {
			if (val == as6200_samp_freq[i].val &&
			    val2 == as6200_samp_freq[i].val2)
				break;
		}

		if (i == ARRAY_SIZE(as6200_samp_freq))
			return -EINVAL;

		return as6200_modify_config_reg(as, AS_CONFIG_CR, i);
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
	struct as6200 *as = iio_device_get_drvdata(indio_dev);
	unsigned int reg;
	int ret;
	unsigned int temp;

	switch (dir) {
	case IIO_EV_DIR_FALLING:
		reg = AS_TLOW_REG;
		break;
	case IIO_EV_DIR_RISING:
		reg = AS_THIGH_REG;
		break;
	default:
		return -EINVAL;
	}

	ret = regmap_read(as->regmap, reg, &temp);
	if (ret)
		return ret;

	*val = sign_extend32(temp >> 4, 11);

	return IIO_VAL_INT;
}

static int as6200_write_event_value(struct iio_dev *indio_dev,
			  const struct iio_chan_spec *chan,
			  enum iio_event_type type,
			  enum iio_event_direction dir,
			  enum iio_event_info info,
			  int val, int val2)
{
	struct as6200 *as = iio_device_get_drvdata(indio_dev);
	unsigned int temp;
	unsigned int reg;

	/*
	 * range without applying the scaling
	 * factor of 0.06250
	 */
	if (val > 2000 || val < -640)
		return -EINVAL;

	temp = (val & 0xfff) << 4;

	switch (dir) {
	case IIO_EV_DIR_FALLING:
		reg = AS_TLOW_REG;
		break;
	case IIO_EV_DIR_RISING:
		reg = AS_THIGH_REG;
		break;
	default:
		return -EINVAL;
	}

	return regmap_write(as->regmap, reg, temp);
}

static irqreturn_t as6200_event_handler(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct as6200 *as = iio_device_get_drvdata(indio_dev);
	unsigned int alert;
	enum iio_event_direction dir;
	int ret;

	mutex_lock(&as->lock);

	ret = as6200_read_config_reg(as, AS_CONFIG_AL, &alert);
	if (ret) {
		mutex_unlock(&as->lock);
		return IRQ_NONE;
	}

	dir = alert ? IIO_EV_DIR_FALLING : IIO_EV_DIR_RISING;

	iio_push_event(indio_dev,
		       IIO_EVENT_CODE(IIO_TEMP, 0, 0,
				      dir,
				      IIO_EV_TYPE_THRESH,
				      0, 0, 0),
		       iio_get_time_ns(indio_dev));

	mutex_unlock(&as->lock);

	return IRQ_HANDLED;
}

static irqreturn_t as6200_trigger_handler(int irq, void *private)
{
	struct iio_poll_func *pf = private;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct as6200 *as = iio_device_get_drvdata(indio_dev);
	int ret;

	mutex_lock(&as->lock);

	ret = regmap_read(as->regmap, AS_TVAL_REG, &as->data[0]);
	if (!ret)
		iio_push_to_buffers_with_timestamp(indio_dev, as->data,
						   iio_get_time_ns(indio_dev));

	mutex_unlock(&as->lock);

	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static ssize_t
consecutive_faults_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct as6200 *as = iio_device_get_drvdata(indio_dev);
	unsigned int cf;
	int ret;

	ret = as6200_read_config_reg(as, AS_CONFIG_CF, &cf);
	if (ret)
		return ret;

	return sprintf(buf, "%u\n", cf ? cf * 2 : 1);
}

static ssize_t
consecutive_faults_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct as6200 *as = iio_device_get_drvdata(indio_dev);
	unsigned int cf;
	int ret;

	ret = kstrtouint(buf, 0, &cf);
	if (ret)
		return ret;

	switch (cf) {
	case 1:
		cf = 0;
		break;
	case 2:
	case 4:
	case 6:
		cf /= 2;
		break;
	default:
		return -EINVAL;
	}

	ret = as6200_modify_config_reg(as, AS_CONFIG_CF, cf);
	if (ret)
		return ret;

	return count;
}

static IIO_CONST_ATTR_SAMP_FREQ_AVAIL("0.25 1 4 8");
static IIO_CONST_ATTR(avail_consecutive_faults, "1 2 4 6");
static IIO_DEVICE_ATTR_RW(consecutive_faults, 0);

static struct attribute *as6200_event_attributes[] = {
	&iio_const_attr_avail_consecutive_faults.dev_attr.attr,
	&iio_dev_attr_consecutive_faults.dev_attr.attr,
	NULL,
};

static struct attribute *as6200_attributes[] = {
	&iio_const_attr_sampling_frequency_available.dev_attr.attr,
	NULL,
};

static const struct attribute_group as6200_attribute_group = {
	.attrs = as6200_attributes,
};

static const struct attribute_group as6200_event_attribute_group = {
	.attrs = as6200_event_attributes,
};

static const struct iio_info as6200_temp_info = {
	.event_attrs = &as6200_event_attribute_group,
	.attrs = &as6200_attribute_group,
	.read_raw = &as6200_read_raw,
	.write_raw = &as6200_write_raw,
	.read_event_value = &as6200_read_event_value,
	.write_event_value = &as6200_write_event_value,
};

static int as6200_probe(struct i2c_client *client)
{
	struct as6200 *as;
	struct iio_dev *indio_dev;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EINVAL;

	indio_dev = devm_iio_device_alloc(&client->dev, 0);
	if (!indio_dev)
		return -ENOMEM;

	as = devm_kzalloc(&client->dev, sizeof(*as), GFP_KERNEL);
	if (IS_ERR(as))
		return PTR_ERR(as);

	as->regmap = devm_regmap_init_i2c(client, &as6200_regmap_config);
	if (IS_ERR(as->regmap))
		return PTR_ERR(as->regmap);

	mutex_init(&as->lock);

	as->dev = &client->dev;
	as->indio_dev = indio_dev;

	iio_device_set_drvdata(indio_dev, as);

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = as6200_temp_channels;
	indio_dev->num_channels = ARRAY_SIZE(as6200_temp_channels);
	indio_dev->name = client->name;
	indio_dev->info = &as6200_temp_info;

	ret = devm_iio_triggered_buffer_setup(as->dev, indio_dev,
					      NULL,
					      as6200_trigger_handler,
					      NULL);
	if (ret)
		return ret;

	if (client->irq) {
		ret = devm_request_threaded_irq(as->dev,
						client->irq,
						NULL,
						&as6200_event_handler,
						IRQF_ONESHOT,
						client->name,
						indio_dev);
		if (ret)
			return ret;
	}

	i2c_set_clientdata(client, as);

	return iio_device_register(indio_dev);
}

static void as6200_remove(struct i2c_client *client)
{
	struct as6200 *as = i2c_get_clientdata(client);

	iio_device_unregister(as->indio_dev);
}

static int __maybe_unused as6200_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct as6200 *as = i2c_get_clientdata(client);

	if (client->irq)
		disable_irq(client->irq);

	return as6200_modify_config_reg(as, AS_CONFIG_SM, 1);
}

static int __maybe_unused as6200_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct as6200 *as = i2c_get_clientdata(client);

	if (client->irq)
		enable_irq(client->irq);

	return as6200_modify_config_reg(as, AS_CONFIG_SM, 0);
}

static const struct dev_pm_ops as6200_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(as6200_suspend, as6200_resume)
};

static const struct i2c_device_id as6200_id_table[] = {
	{ "as6200" },
	{ },
};
MODULE_DEVICE_TABLE(i2c, as6200_id_table);

static const struct of_device_id as6200_of_match[] = {
	{ .compatible = "ams,as6200" },
	{ },
};
MODULE_DEVICE_TABLE(of, as6200_of_match);

static struct i2c_driver as6200_driver = {
	.driver = {
		.name = "as6200",
		.pm = &as6200_pm_ops,
		.of_match_table = as6200_of_match,
	},
	.probe = as6200_probe,
	.remove = as6200_remove,
	.id_table = as6200_id_table,
};
module_i2c_driver(as6200_driver);

MODULE_AUTHOR("Abdel Alkuor <alkuor@gmail.com");
MODULE_DESCRIPTION("AMS AS6200 Temperature Sensor");
MODULE_LICENSE("GPL");
