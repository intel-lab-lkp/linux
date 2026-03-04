// SPDX-License-Identifier: GPL-2.0-only
/*
 * Analog Devices MAX30210 I2C Temperature Sensor driver
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/iio/buffer.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/iio/sysfs.h>
#include <linux/regmap.h>
#include <linux/units.h>

#define MAX30210_STATUS_REG		0x00
#define MAX30210_INT_EN_REG		0x02
#define MAX30210_FIFO_DATA_REG		0x08
#define MAX30210_FIFO_CONF_1_REG	0x09
#define MAX30210_FIFO_CONF_2_REG	0x0A
#define MAX30210_SYS_CONF_REG		0x11
#define MAX30210_PIN_CONF_REG		0x12
#define MAX30210_TEMP_ALM_HI_REG	0x22
#define MAX30210_TEMP_ALM_LO_REG	0x24
#define MAX30210_TEMP_INC_THRESH_REG	0x26
#define MAX30210_TEMP_DEC_THRESH_REG	0x27
#define MAX30210_TEMP_CONF_1_REG	0x28
#define MAX30210_TEMP_CONF_2_REG	0x29
#define MAX30210_TEMP_CONV_REG		0x2A
#define MAX30210_TEMP_DATA_REG		0x2B
#define MAX30210_TEMP_SLOPE_REG		0x2D
#define MAX30210_UNIQUE_ID_REG		0x30
#define MAX30210_PART_ID_REG		0xFF

#define MAX30210_STATUS_A_FULL_MASK   BIT(7)
#define MAX30210_STATUS_TEMP_RDY_MASK BIT(6)
#define MAX30210_STATUS_TEMP_DEC_MASK BIT(5)
#define MAX30210_STATUS_TEMP_INC_MASK BIT(4)
#define MAX30210_STATUS_TEMP_LO_MASK  BIT(3)
#define MAX30210_STATUS_TEMP_HI_MASK  BIT(2)
#define MAX30210_STATUS_PWR_RDY_MASK  BIT(0)

#define MAX30210_FIFOCONF1_FLUSH_FIFO_MASK BIT(4)

#define MAX30210_SYSCONF_RESET_MASK		BIT(0)

#define MAX30210_PINCONF_EXT_CNV_EN_MASK	BIT(7)
#define MAX30210_PINCONF_EXT_CVT_ICFG_MASK	BIT(6)
#define MAX30210_PINCONF_INT_FCFG_MASK		GENMASK(3, 2)
#define MAX30210_PINCONF_INT_OCFG_MASK		GENMASK(1, 0)

#define MAX30210_TEMPCONF1_CHG_DET_EN_MASK	BIT(3)
#define MAX30210_TEMPCONF1_RATE_CHG_FILTER_MASK	GENMASK(2, 0)

#define MAX30210_TEMPCONF2_TEMP_PERIOD_MASK	GENMASK(3, 0)
#define MAX30210_TEMPCONF2_ALERT_MODE_MASK	BIT(7)

#define MAX30210_TEMPCONV_AUTO_MASK	BIT(1)
#define MAX30210_TEMPCONV_CONV_T_MASK	BIT(0)

#define MAX30210_PART_ID		0x45
#define MAX30210_FIFO_SIZE		64
#define MAX30210_FIFO_INVAL_DATA	GENMASK(23, 0)
#define MAX30210_WATERMARK_DEFAULT	(0x40 - 0x1F)

#define MAX30210_UNIQUE_ID_LEN		6
#define MAX30210_EXT_CVT_FREQ_MIN	1
#define MAX30210_EXT_CVT_FREQ_MAX	20

struct max30210_state {
	struct regmap *regmap;
	u8 watermark;
	/* Raw FIFO byte buffer (3 bytes per sample) */
	u8 fifo_buf[3 * MAX30210_FIFO_SIZE];
};

static const int max30210_samp_freq_avail[][2] = {
	{ 0, 15625 },
	{ 0, 31250 },
	{ 0, 62500 },
	{ 0, 125000 },
	{ 0, 250000 },
	{ 0, 500000 },
	{ 1, 0 },
	{ 2, 0 },
	{ 4, 0 },
	{ 8, 0 },
};

static const struct regmap_config max30210_regmap = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = MAX30210_PART_ID_REG,
};

static int max30210_read_temp(struct regmap *regmap, unsigned int reg,
			      int *temp)
{
	__be16 val;
	int ret;

	ret = regmap_bulk_read(regmap, reg, &val, sizeof(val));
	if (ret)
		return ret;

	*temp = sign_extend32(be16_to_cpu(val), 15);

	return IIO_VAL_INT;
}

static int max30210_write_temp(struct regmap *regmap, unsigned int reg,
			       int temp)
{
	__be16 val;

	val = cpu_to_be16(temp);

	return regmap_bulk_write(regmap, reg, &val, sizeof(val));
}

static void max30210_fifo_read(struct iio_dev *indio_dev)
{
	struct max30210_state *st = iio_priv(indio_dev);
	int ret;

	ret = regmap_bulk_read(st->regmap, MAX30210_FIFO_DATA_REG,
			       st->fifo_buf, 3 * st->watermark);
	if (ret) {
		dev_err(&indio_dev->dev, "Failed to read from fifo.\n");
		return;
	}

	for (unsigned int i = 0; i < st->watermark; i++) {
		u32 raw = get_unaligned_be24(&st->fifo_buf[3 * i]);

		if (raw == MAX30210_FIFO_INVAL_DATA) {
			dev_err_ratelimited(&indio_dev->dev, "Invalid data\n");
			continue;
		}

		s16 temp = (s16)(raw & 0xFFFF);

		iio_push_to_buffers(indio_dev, &temp);
	}
}

static irqreturn_t max30210_irq_handler(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = dev_id;
	struct max30210_state *st = iio_priv(indio_dev);
	unsigned int status;
	int ret;

	ret = regmap_read(st->regmap, MAX30210_STATUS_REG, &status);
	if (ret) {
		dev_err(&indio_dev->dev, "Status read failed\n");
		return IRQ_NONE;
	}

	if (status & MAX30210_STATUS_A_FULL_MASK)
		max30210_fifo_read(indio_dev);

	if (status & MAX30210_STATUS_TEMP_HI_MASK)
		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(IIO_TEMP, 0,
						    IIO_EV_TYPE_THRESH,
						    IIO_EV_DIR_RISING),
			       iio_get_time_ns(indio_dev));

	if (status & MAX30210_STATUS_TEMP_LO_MASK)
		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(IIO_TEMP, 0,
						    IIO_EV_TYPE_THRESH,
						    IIO_EV_DIR_FALLING),
			       iio_get_time_ns(indio_dev));

	return IRQ_HANDLED;
}

static int max30210_reg_access(struct iio_dev *indio_dev, unsigned int reg,
			       unsigned int writeval, unsigned int *readval)
{
	struct max30210_state *st = iio_priv(indio_dev);

	if (!readval)
		return regmap_write(st->regmap, reg, writeval);

	return regmap_read(st->regmap, reg, readval);
}

static int max30210_read_event(struct iio_dev *indio_dev,
			       const struct iio_chan_spec *chan,
			       enum iio_event_type type,
			       enum iio_event_direction dir,
			       enum iio_event_info info, int *val, int *val2)
{
	struct max30210_state *st = iio_priv(indio_dev);

	if (info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_RISING:
		switch (type) {
		case IIO_EV_TYPE_THRESH:
			return max30210_read_temp(st->regmap, MAX30210_TEMP_ALM_HI_REG, val);
		default:
			return -EINVAL;
		}
	case IIO_EV_DIR_FALLING:
		switch (type) {
		case IIO_EV_TYPE_THRESH:
			return max30210_read_temp(st->regmap, MAX30210_TEMP_ALM_LO_REG, val);
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static int max30210_write_event(struct iio_dev *indio_dev,
				const struct iio_chan_spec *chan,
				enum iio_event_type type,
				enum iio_event_direction dir,
				enum iio_event_info info, int val, int val2)
{
	struct max30210_state *st = iio_priv(indio_dev);

	if (info != IIO_EV_INFO_VALUE)
		return -EINVAL;

	switch (dir) {
	case IIO_EV_DIR_RISING:
		switch (type) {
		case IIO_EV_TYPE_THRESH:
			return max30210_write_temp(st->regmap, MAX30210_TEMP_ALM_HI_REG, val);
		default:
			return -EINVAL;
		}
	case IIO_EV_DIR_FALLING:
		switch (type) {
		case IIO_EV_TYPE_THRESH:
			return max30210_write_temp(st->regmap, MAX30210_TEMP_ALM_LO_REG, val);
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static int max30210_read_event_config(struct iio_dev *indio_dev,
				      const struct iio_chan_spec *chan,
				      enum iio_event_type type,
				      enum iio_event_direction dir)
{
	struct max30210_state *st = iio_priv(indio_dev);
	unsigned int val;
	int ret;

	ret = regmap_read(st->regmap, MAX30210_INT_EN_REG, &val);
	if (ret)
		return ret;

	switch (dir) {
	case IIO_EV_DIR_RISING:
		switch (type) {
		case IIO_EV_TYPE_THRESH:
			return FIELD_GET(MAX30210_STATUS_TEMP_HI_MASK, val);
		default:
			return -EINVAL;
		}
	case IIO_EV_DIR_FALLING:
		switch (type) {
		case IIO_EV_TYPE_THRESH:
			return FIELD_GET(MAX30210_STATUS_TEMP_LO_MASK, val);
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static int max30210_write_event_config(struct iio_dev *indio_dev,
				       const struct iio_chan_spec *chan,
				       enum iio_event_type type,
				       enum iio_event_direction dir, bool state)
{
	struct max30210_state *st = iio_priv(indio_dev);

	switch (dir) {
	case IIO_EV_DIR_RISING:
		switch (type) {
		case IIO_EV_TYPE_THRESH:
			return regmap_assign_bits(st->regmap, MAX30210_INT_EN_REG,
						  MAX30210_STATUS_TEMP_HI_MASK, state);
		default:
			return -EINVAL;
		}
	case IIO_EV_DIR_FALLING:
		switch (type) {
		case IIO_EV_TYPE_THRESH:
			return regmap_assign_bits(st->regmap, MAX30210_INT_EN_REG,
						  MAX30210_STATUS_TEMP_LO_MASK, state);
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static int max30210_read_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int *val,
			     int *val2, long mask)
{
	struct max30210_state *st = iio_priv(indio_dev);
	unsigned int uval;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*val = 5;
		*val2 = 1000;

		return IIO_VAL_FRACTIONAL;
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = regmap_read(st->regmap, MAX30210_TEMP_CONF_2_REG, &uval);
		if (ret)
			return ret;

		uval = FIELD_GET(MAX30210_TEMPCONF2_TEMP_PERIOD_MASK, uval);

		if (uval >= ARRAY_SIZE(max30210_samp_freq_avail))
			uval = ARRAY_SIZE(max30210_samp_freq_avail) - 1;

		*val = max30210_samp_freq_avail[uval][0];
		*val2 = max30210_samp_freq_avail[uval][1];

		return IIO_VAL_FRACTIONAL;
	case IIO_CHAN_INFO_RAW: {
		if (iio_buffer_enabled(indio_dev))
			return max30210_read_temp(st->regmap, MAX30210_TEMP_DATA_REG, val);

		IIO_DEV_ACQUIRE_DIRECT_MODE(indio_dev, claim);
		if (IIO_DEV_ACQUIRE_FAILED(claim))
			return -EBUSY;

		ret = regmap_write(st->regmap, MAX30210_TEMP_CONV_REG,
				   MAX30210_TEMPCONV_CONV_T_MASK);
		if (ret)
			return ret;

		/*
		 * Wait until CONVERT_T auto-clears.
		 * Datasheet:
		 *   tBIAS_WU = 260 µs
		 *   tINT     = 8 ms
		 *
		 * Worst-case conversion ≈ 8.26 ms.
		 * Use 10 ms timeout for margin.
		 */
		ret = regmap_read_poll_timeout(st->regmap, MAX30210_TEMP_CONV_REG, uval,
					       !(uval & MAX30210_TEMPCONV_CONV_T_MASK),
					       500,          /* poll every 500 µs */
					       10000);       /* 10 ms timeout */
		if (ret)
			return ret;

		return max30210_read_temp(st->regmap, MAX30210_TEMP_DATA_REG, val);
	}
	default:
		return -EINVAL;
	}
}

static int max30210_read_avail(struct iio_dev *indio_dev,
			       struct iio_chan_spec const *chan,
			       const int **vals, int *type, int *length,
			       long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*vals = &max30210_samp_freq_avail[0][0];
		*type = IIO_VAL_INT_PLUS_MICRO;
		*length = ARRAY_SIZE(max30210_samp_freq_avail) * 2;

		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static int max30210_write_raw(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan, int val,
			      int val2, long mask)
{
	struct max30210_state *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ: {
		IIO_DEV_ACQUIRE_DIRECT_MODE(indio_dev, claim);
		if (IIO_DEV_ACQUIRE_FAILED(claim))
			return -EBUSY;

		if (val < 0 || val2 < 0)
			return -EINVAL;

		for (unsigned int i = 0; i < ARRAY_SIZE(max30210_samp_freq_avail); i++) {
			if (val == max30210_samp_freq_avail[i][0] &&
			    val2 == max30210_samp_freq_avail[i][1]) {
				return regmap_update_bits(st->regmap, MAX30210_TEMP_CONF_2_REG,
						MAX30210_TEMPCONF2_TEMP_PERIOD_MASK,
						FIELD_PREP(MAX30210_TEMPCONF2_TEMP_PERIOD_MASK, i));
			}
		}

		return -EINVAL;
	}
	default:
		return -EINVAL;
	}
}

static int max30210_set_watermark(struct iio_dev *indio_dev, unsigned int val)
{
	struct max30210_state *st = iio_priv(indio_dev);
	unsigned int reg;
	int ret;

	if (val < 1 || val > MAX30210_FIFO_SIZE)
		return -EINVAL;

	reg = MAX30210_FIFO_SIZE - val;

	ret = regmap_write(st->regmap, MAX30210_FIFO_CONF_1_REG, reg);
	if (ret)
		return ret;

	st->watermark = val;

	return 0;
}

static ssize_t hwfifo_watermark_show(struct device *dev,
				     struct device_attribute *devattr,
				     char *buf)
{
	struct max30210_state *st = iio_priv(dev_to_iio_dev(dev));

	return sysfs_emit(buf, "%d\n", st->watermark);
}

IIO_STATIC_CONST_DEVICE_ATTR(hwfifo_watermark_min, "1");
IIO_STATIC_CONST_DEVICE_ATTR(hwfifo_watermark_max,
			     __stringify(MAX30210_FIFO_SIZE));
static IIO_DEVICE_ATTR_RO(hwfifo_watermark, 0);

static const struct iio_dev_attr *max30210_fifo_attributes[] = {
	&iio_dev_attr_hwfifo_watermark_min,
	&iio_dev_attr_hwfifo_watermark_max,
	&iio_dev_attr_hwfifo_watermark,
	NULL
};

static int max30210_buffer_preenable(struct iio_dev *indio_dev)
{
	struct max30210_state *st = iio_priv(indio_dev);
	int ret;

	ret = regmap_set_bits(st->regmap, MAX30210_INT_EN_REG, MAX30210_STATUS_A_FULL_MASK);
	if (ret)
		return ret;

	ret = regmap_set_bits(st->regmap, MAX30210_FIFO_CONF_2_REG,
			      MAX30210_FIFOCONF1_FLUSH_FIFO_MASK);
	if (ret)
		return ret;

	return regmap_write(st->regmap, MAX30210_TEMP_CONV_REG,
			    MAX30210_TEMPCONV_AUTO_MASK | MAX30210_TEMPCONV_CONV_T_MASK);
}

static int max30210_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct max30210_state *st = iio_priv(indio_dev);
	int ret;

	ret = regmap_write(st->regmap, MAX30210_TEMP_CONV_REG, 0x0);
	if (ret)
		return ret;

	ret = regmap_set_bits(st->regmap, MAX30210_FIFO_CONF_2_REG,
			      MAX30210_FIFOCONF1_FLUSH_FIFO_MASK);
	if (ret)
		return ret;

	return regmap_clear_bits(st->regmap, MAX30210_INT_EN_REG,
				 MAX30210_STATUS_A_FULL_MASK);
}

static const struct iio_buffer_setup_ops max30210_buffer_ops = {
	.preenable = max30210_buffer_preenable,
	.postdisable = max30210_buffer_postdisable,
};

static const struct iio_info max30210_info = {
	.read_raw = max30210_read_raw,
	.read_avail = max30210_read_avail,
	.write_raw = max30210_write_raw,
	.hwfifo_set_watermark = max30210_set_watermark,
	.debugfs_reg_access = max30210_reg_access,
	.read_event_value = max30210_read_event,
	.write_event_value = max30210_write_event,
	.write_event_config = max30210_write_event_config,
	.read_event_config = max30210_read_event_config,
};

static const struct iio_event_spec max30210_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) | BIT(IIO_EV_INFO_ENABLE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) | BIT(IIO_EV_INFO_ENABLE),
	},
};

static const struct iio_chan_spec max30210_channels = {
	.type = IIO_TEMP,
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			      BIT(IIO_CHAN_INFO_SCALE) |
			      BIT(IIO_CHAN_INFO_SAMP_FREQ),
	.info_mask_separate_available = BIT(IIO_CHAN_INFO_SAMP_FREQ),
	.scan_index = 0,
	.event_spec = max30210_events,
	.num_event_specs = ARRAY_SIZE(max30210_events),
	.scan_type = {
		.sign = 's',
		.realbits = 16,
		.storagebits = 16,
		.shift = 0,
		.endianness = IIO_CPU,
	},
};

static int max30210_setup(struct max30210_state *st, struct device *dev)
{
	struct gpio_desc *powerdown_gpio;
	unsigned int val;
	int ret;

	/* Optional hardware reset via powerdown GPIO */
	powerdown_gpio = devm_gpiod_get_optional(dev, "powerdown",
						 GPIOD_OUT_HIGH);
	if (IS_ERR(powerdown_gpio))
		return dev_err_probe(dev, PTR_ERR(powerdown_gpio),
				     "failed to request powerdown GPIO\n");

	if (powerdown_gpio) {
		/* Deassert powerdown to power up device */
		gpiod_set_value(powerdown_gpio, 0);
	} else {
		/* Software reset fallback */
		ret = regmap_update_bits(st->regmap, MAX30210_SYS_CONF_REG,
					 MAX30210_SYSCONF_RESET_MASK,
					 MAX30210_SYSCONF_RESET_MASK);
		if (ret)
			return ret;
	}

	/* Datasheet Figure 6:
	 * tPU max = 700 µs after power-up or reset before device is ready.
	 */
	fsleep(700);

	/* Clear status byte */
	return regmap_read(st->regmap, MAX30210_STATUS_REG, &val);
}

static int max30210_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct max30210_state *st;
	int ret;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EOPNOTSUPP;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);

	ret = devm_regulator_get_enable(dev, "vdd");
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to enable vdd regulator.\n");

	st->regmap = devm_regmap_init_i2c(client, &max30210_regmap);
	if (IS_ERR(st->regmap))
		return dev_err_probe(dev, PTR_ERR(st->regmap),
				     "Failed to allocate regmap.\n");

	st->watermark = MAX30210_WATERMARK_DEFAULT;

	ret = max30210_setup(st, dev);
	if (ret)
		return ret;

	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = &max30210_channels;
	indio_dev->num_channels = 1;
	indio_dev->name = "max30210";
	indio_dev->info = &max30210_info;

	ret = devm_iio_kfifo_buffer_setup_ext(dev, indio_dev, &max30210_buffer_ops,
					      max30210_fifo_attributes);
	if (ret)
		return ret;

	if (client->irq) {
		ret = devm_request_irq(dev, client->irq, max30210_irq_handler, IRQF_NO_THREAD,
				       indio_dev->name, indio_dev);
		if (ret)
			return ret;
	}

	return devm_iio_device_register(dev, indio_dev);
}

static const struct i2c_device_id max30210_id[] = {
	{ "max30210" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, max30210_id);

static const struct of_device_id max30210_of_match[] = {
	{ .compatible = "adi,max30210" },
	{ }
};
MODULE_DEVICE_TABLE(of, max30210_of_match);

static struct i2c_driver max30210_driver = {
	.driver = {
		.name = "max30210",
		.of_match_table = max30210_of_match,
	},
	.probe = max30210_probe,
	.id_table = max30210_id,
};
module_i2c_driver(max30210_driver);

MODULE_AUTHOR("John Erasmus Mari Geronimo <johnerasmusmari.geronimo@analog.com");
MODULE_DESCRIPTION("MAX30210 low-power digital temperature sensor");
MODULE_LICENSE("GPL");
