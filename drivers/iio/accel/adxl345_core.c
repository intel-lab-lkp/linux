// SPDX-License-Identifier: GPL-2.0-only
/*
 * ADXL345 3-Axis Digital Accelerometer IIO core driver
 *
 * Copyright (c) 2017 Eva Rachel Retuya <eraretuya@gmail.com>
 *
 * Datasheet: https://www.analog.com/media/en/technical-documentation/data-sheets/ADXL345.pdf
 */

#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/units.h>
#include <linux/interrupt.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#include <linux/input/adxl34x.h>

#include "adxl345.h"

/* ADXL345 register map */
#define ADXL345_REG_DEVID		0x00 /* r    Device ID */
#define ADXL345_REG_THRESH_TAP	0x1D /* r/w  Tap Threshold */
#define ADXL345_REG_OFSX		0x1E /* r/w  X-axis offset */
#define ADXL345_REG_OFSY		0x1F /* r/w  Y-axis offset */
#define ADXL345_REG_OFSZ		0x20 /* r/w  Z-axis offset */
#define ADXL345_REG_DUR		0x21 /* r/w  Tap duration */
#define ADXL345_REG_LATENT		0x22 /* r/w  Tap latency */
#define ADXL345_REG_WINDOW		0x23 /* r/w  Tap window */
#define ADXL345_REG_THRESH_ACT		0x24 /* r/w  Activity threshold */
#define ADXL345_REG_THRESH_INACT	0x25 /* r/w  Inactivity threshold */
#define ADXL345_REG_TIME_INACT	0x26 /* r/w  Inactivity time */
#define ADXL345_REG_ACT_INACT_CTRL	0x27 /* r/w  Axis enable control for */
					     /*      activity and inactivity */
					     /*      detection */
#define ADXL345_REG_THRESH_FF		0x28 /* r/w  Free-fall threshold */
#define ADXL345_REG_TIME_FF		0x29 /* r/w  Free-fall time */
#define ADXL345_REG_TAP_AXIS		0x2A /* r/w  Axis control for */
					     /*      single tap or double tap */
#define ADXL345_REG_ACT_TAP_STATUS	0x2B /* r    Source of single tap or */
					     /*      double tap */
#define ADXL345_REG_BW_RATE		0x2C /* r/w  Data rate and power */
					     /*        mode control */
#define ADXL345_REG_POWER_CTL		0x2D /* r/w  Power-saving features */
#define ADXL345_REG_INT_ENABLE		0x2E /* r/w  Interrupt enable control */
#define ADXL345_REG_INT_MAP		0x2F /* r/w  Interrupt mapping */
					     /*      control */
#define ADXL345_REG_INT_SOURCE		0x30 /* r    Source of interrupts */
/* NB: ADXL345_REG_DATA_FORMAT		0x31  r/w  Data format control,
 *   (defined in header)
 */

#define ADXL345_REG_XYZ_BASE		0x32 /* r    Base address to read out */
					     /*      X-, Y- and Z-axis data 0 */
					     /*      and 1 */
#define ADXL345_REG_DATA_AXIS(index)				\
	(ADXL345_REG_XYZ_BASE + (index) * sizeof(__le16))
/* NB: having DATAX0 and DATAX1 makes 2x sizeof(__le16) */

#define ADXL345_REG_FIFO_CTL		0x38 /* r/w  FIFO control */
#define ADXL345_REG_FIFO_STATUS		0x39 /* r    FIFO status */

/* DEVID(s) */
#define ADXL345_DEVID			0xE5

/* FIFO */
#define ADXL345_FIFO_CTL_SAMLPES(x)	(0x1f & (x))
#define ADXL345_FIFO_CTL_TRIGGER(x)	(0x20 & ((x) << 5)) /* set 1: INT2, 0: INT1 */
#define ADXL345_FIFO_CTL_MODE(x)	(0xc0 & ((x) << 6))

/* INT_ENABLE, INT_MAP, INT_SOURCE bits */
#define ADXL345_INT_DATA_READY		BIT(7)
#define ADXL345_INT_SINGLE_TAP		BIT(6)
#define ADXL345_INT_DOUBLE_TAP		BIT(5)
#define ADXL345_INT_ACTIVITY		BIT(4)
#define ADXL345_INT_INACTIVITY		BIT(3)
#define ADXL345_INT_FREE_FALL		BIT(2)
#define ADXL345_INT_WATERMARK		BIT(1)
#define ADXL345_INT_OVERRUN		BIT(0)

#define ADXL34X_S_TAP_MSK	ADXL345_INT_SINGLE_TAP
#define ADXL34X_D_TAP_MSK	ADXL345_INT_DOUBLE_TAP

/* INT1 or INT2 */
#define ADXL345_INT1			0
#define ADXL345_INT2			1

/* BW_RATE bits - Bandwidth and output data rate. The default value is
 * 0x0A, which translates to a 100 Hz output data rate
 */
#define ADXL345_BW_RATE			GENMASK(3, 0)
#define ADXL345_BW_LOW_POWER	BIT(4)
#define ADXL345_BASE_RATE_NANO_HZ	97656250LL

/* POWER_CTL bits */
#define ADXL345_POWER_CTL_STANDBY	0x00

/* NB:
 * BIT(0), BIT(1) for explicit wakeup (not implemented)
 * BIT(2) for explicit sleep (not implemented)
 */
#define ADXL345_POWER_CTL_MEASURE	BIT(3)
#define ADXL345_POWER_CTL_AUTO_SLEEP	BIT(4)
#define ADXL345_POWER_CTL_LINK	BIT(5)

/* DATA_FORMAT bits */
#define ADXL345_DATA_FORMAT_RANGE	GENMASK(1, 0)	/* Set the g range */
#define ADXL345_DATA_FORMAT_JUSTIFY	BIT(2)	/* 1: left-justified (MSB) mode, 0: right-just'd */
#define ADXL345_DATA_FORMAT_FULL_RES	BIT(3)	/* Up to 13-bits resolution */
/* NB: BIT(6): 3-wire SPI mode (defined in header) */

#define ADXL345_DATA_FORMAT_SELF_TEST	BIT(7)	/* Enable a self test */
#define ADXL345_DATA_FORMAT_2G		0
#define ADXL345_DATA_FORMAT_4G		1
#define ADXL345_DATA_FORMAT_8G		2
#define ADXL345_DATA_FORMAT_16G		3

#define ADXL345_REG_OFS_AXIS(index)	(ADXL345_REG_OFSX + (index))

/* The ADXL345 include a 32 sample FIFO
 *
 * FIFO stores a maximum of 32 entries, which equates to a maximum of 33
 * entries available at any given time because an additional entry is available
 * at the output filter of the device.
 * (see datasheet FIFO_STATUS description on "Entries Bits")
 */
#define ADXL34x_FIFO_SIZE  33

static const struct adxl34x_platform_data adxl345_default_init = {
	.tap_axis_control = ADXL_TAP_X_EN | ADXL_TAP_Y_EN | ADXL_TAP_Z_EN,
};

struct adxl34x_state {
	int irq;
	const struct adxl345_chip_info *info;
	struct regmap *regmap;
	struct adxl34x_platform_data data;  /* watermark, fifo_mode, etc */

	__le16 fifo_buf[3 * ADXL34x_FIFO_SIZE] __aligned(IIO_DMA_MINALIGN);
	u8 int_map;
	bool fifo_delay; /* delay: delay is needed for SPI */
	u8 intio;
};

#define ADXL34x_CHANNEL(index, reg, axis) {				\
		.type = IIO_ACCEL,					\
			.address = (reg),				\
			.modified = 1,					\
			.channel2 = IIO_MOD_##axis,			\
			.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |	\
			BIT(IIO_CHAN_INFO_CALIBBIAS),			\
			.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE) | \
			BIT(IIO_CHAN_INFO_SAMP_FREQ),			\
			.scan_index = (index),				\
			.scan_type = {					\
				.sign = 's',				\
				.realbits = 13,				\
				.storagebits = 16,			\
				.shift = 0,				\
				.endianness = IIO_LE,			\
			},						\
}

enum adxl34x_chans {
	chan_x, chan_y, chan_z,
};

static const struct iio_chan_spec adxl34x_channels[] = {
	ADXL34x_CHANNEL(0, chan_x, X),
	ADXL34x_CHANNEL(1, chan_y, Y),
	ADXL34x_CHANNEL(2, chan_z, Z),
};

static int adxl345_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct adxl34x_state *st = iio_priv(indio_dev);
	__le16 accel;
	long long samp_freq_nhz;
	unsigned int regval;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/*
		 * Data is stored in adjacent registers:
		 * ADXL345_REG_DATA(X0/Y0/Z0) contain the least significant byte
		 * and ADXL345_REG_DATA(X0/Y0/Z0) + 1 the most significant byte
		 */
		ret = regmap_bulk_read(st->regmap,
				       ADXL345_REG_DATA_AXIS(chan->address),
				       &accel, sizeof(accel));
		if (ret < 0)
			return ret;

		*val = sign_extend32(le16_to_cpu(accel), 12);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = st->info->uscale;
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_CALIBBIAS:
		ret = regmap_read(st->regmap,
				  ADXL345_REG_OFS_AXIS(chan->address), &regval);
		if (ret < 0)
			return ret;
		/*
		 * 8-bit resolution at +/- 2g, that is 4x accel data scale
		 * factor
		 */
		*val = sign_extend32(regval, 7) * 4;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = regmap_read(st->regmap, ADXL345_REG_BW_RATE, &regval);
		if (ret < 0)
			return ret;

		samp_freq_nhz = ADXL345_BASE_RATE_NANO_HZ <<
				(regval & ADXL345_BW_RATE);
		*val = div_s64_rem(samp_freq_nhz, NANOHZ_PER_HZ, val2);

		return IIO_VAL_INT_PLUS_NANO;
	}

	return -EINVAL;
}

static int adxl345_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     int val, int val2, long mask)
{
	struct adxl34x_state *st = iio_priv(indio_dev);
	s64 n;

	switch (mask) {
	case IIO_CHAN_INFO_CALIBBIAS:
		/*
		 * 8-bit resolution at +/- 2g, that is 4x accel data scale
		 * factor
		 */
		return regmap_write(st->regmap,
				    ADXL345_REG_OFS_AXIS(chan->address),
				    val / 4);
	case IIO_CHAN_INFO_SAMP_FREQ:
		n = div_s64(val * NANOHZ_PER_HZ + val2,
			    ADXL345_BASE_RATE_NANO_HZ);

		return regmap_update_bits(st->regmap, ADXL345_REG_BW_RATE,
					  ADXL345_BW_RATE,
					  clamp_val(ilog2(n), 0,
						    ADXL345_BW_RATE));
	}

	return -EINVAL;
}

static int adxl345_write_raw_get_fmt(struct iio_dev *indio_dev,
				     struct iio_chan_spec const *chan,
				     long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_CALIBBIAS:
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SAMP_FREQ:
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return -EINVAL;
	}
}

/**
 * For lowest power operation, standby mode can be used. In standby mode,
 * current consumption is supposed to be reduced to 0.1uA (typical). In this
 * mode no measurements are made. Placing the device into standby mode
 * preserves the contents of FIFO.
 *
 * Unloading the driver puts the device in standby mode (measuring off).
 *
 * @st: The device data.
 * @en: Enable measurements, else standby mode.
 */
static int adxl345_set_measure_en(struct adxl34x_state *st, bool en)
{
	unsigned int val = 0;
	int ret;

	val = (en) ? ADXL345_POWER_CTL_MEASURE : ADXL345_POWER_CTL_STANDBY;
	ret = regmap_write(st->regmap, ADXL345_REG_POWER_CTL, val);
	if (ret)
		return -EINVAL;

	return 0;
}

static void adxl345_powerdown(void *ptr)
{
	struct adxl34x_state *st = ptr;

	adxl345_set_measure_en(st, false);
}

static const struct iio_dev_attr *adxl345_fifo_attributes[] = {
	NULL,
};

static IIO_CONST_ATTR_SAMP_FREQ_AVAIL(
"0.09765625 0.1953125 0.390625 0.78125 1.5625 3.125 6.25 12.5 25 50 100 200 400 800 1600 3200"
);

static struct attribute *adxl345_attrs[] = {
	&iio_const_attr_sampling_frequency_available.dev_attr.attr,
	NULL
};

static const struct attribute_group adxl345_attrs_group = {
	.attrs = adxl345_attrs,
};

/**
 * Read number of FIFO entries into *fifo_entries
 */
static int adxl345_get_fifo_entries(struct adxl34x_state *st, int *fifo_entries)
{
	unsigned int regval = 0;
	int ret;

	ret = regmap_read(st->regmap, ADXL345_REG_FIFO_STATUS, &regval);
	if (ret) {
		pr_warn("%s(): Failed to read FIFO_STATUS register\n", __func__);
		*fifo_entries = 0;
		return ret;
	}

	*fifo_entries = 0x3f & regval;
	pr_debug("%s(): fifo_entries %d\n", __func__, *fifo_entries);

	return 0;
}

static const struct iio_buffer_setup_ops adxl345_buffer_ops = {
};

static int adxl345_get_status(struct adxl34x_state *st, u8 *int_stat)
{
	int ret;
	unsigned int regval;

	*int_stat = 0;
	ret = regmap_read(st->regmap, ADXL345_REG_INT_SOURCE, &regval);
	if (ret) {
		pr_warn("%s(): Failed to read INT_SOURCE register\n", __func__);
		return -EINVAL;
	}

	*int_stat = 0xff & regval;
	pr_debug("%s(): int_stat 0x%02X (INT_SOURCE)\n", __func__, *int_stat);

	return 0;
}

/**
 * Interrupt handler used for several features of the ADXL345.
 * - DATA_READY / FIFO watermark
 * - single tap / double tap
 * - activity / inactivity
 * - freefall detection
 * - overrun
 *
 * @irq: The interrupt number. Having an interrupt imples FIFO_STREAM mode was
 * enabled. Since this is given there will no be further test for being in
 * FIFO_BYPASS mode. FIFO_TRIGGER and FIFO_FIFO mode (being similar to
 * FIFO_STREAM mode) are not separately implemented so far. Both should be
 * work smoothly with the same way of interrupt handling.
 * @p: The iio poll function instance, used to derive the device and data.
 */
static irqreturn_t adxl345_trigger_handler(int irq, void *p)
{
	struct iio_dev *indio_dev = ((struct iio_poll_func *) p)->indio_dev;
	struct adxl34x_state *st = iio_priv(indio_dev);
	u8 int_stat;
	int fifo_entries;
	int ret;

	ret = adxl345_get_status(st, &int_stat);
	if (ret < 0)
		goto done;

	/* Ignore already read event by reissued too fast */
	if (int_stat == 0x0)
		goto done;

	/* evaluation */

	if (int_stat & ADXL345_INT_OVERRUN) {
		pr_debug("%s(): OVERRUN event detected\n", __func__);
		goto done;
	}

	if (int_stat & (ADXL345_INT_DATA_READY | ADXL345_INT_WATERMARK)) {
		pr_debug("%s(): WATERMARK or DATA_READY event detected\n", __func__);
		if (adxl345_get_fifo_entries(st, &fifo_entries) < 0)
			goto done;
	}
	goto done;
done:

	if (indio_dev)
		iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static const struct iio_info adxl345_info = {
	.attrs		= &adxl345_attrs_group,
	.read_raw	= adxl345_read_raw,
	.write_raw	= adxl345_write_raw,
	.write_raw_get_fmt	= adxl345_write_raw_get_fmt,
};

/**
 * adxl345_core_probe() - probe and setup for the adxl345 accelerometer,
 *                        also covers the adxl375 and adxl346 accelerometer
 * @dev:	Driver model representation of the device
 * @regmap:	Regmap instance for the device
 * @irq:	Interrupt handling for async usage
 * @fifo_delay_default: Using FIFO with SPI needs delay
 * @setup:	Setup routine to be executed right before the standard device
 *		setup
 *
 * Return: 0 on success, negative errno on error
 */
int adxl345_core_probe(struct device *dev, struct regmap *regmap,
		       int irq, bool fifo_delay_default,
		       int (*setup)(struct device*, struct regmap*))
{
	struct adxl34x_state *st;
	struct iio_dev *indio_dev;
	u32 regval;

	/* NB: ADXL345_DATA_FORMAT_JUSTIFY or 0:
	 * do right-justified: 0, then adjust resolution according to 10-bit
	 * through 13-bit in channel - this is the default behavior, and can
	 * be modified here by oring ADXL345_DATA_FORMAT_JUSTIFY
	 */
	unsigned int data_format_mask = (ADXL345_DATA_FORMAT_RANGE |
					 ADXL345_DATA_FORMAT_FULL_RES |
					 ADXL345_DATA_FORMAT_SELF_TEST);
	const struct adxl34x_platform_data *data;
	u8 fifo_ctl;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->regmap = regmap;

	st->irq = irq;
	st->info = device_get_match_data(dev);
	if (!st->info)
		return -ENODEV;

	st->fifo_delay = fifo_delay_default;
	data = dev_get_platdata(dev);
	if (!data) {
		dev_dbg(dev, "No platform data: Using default initialization\n");
		data = &adxl345_default_init;
	}
	st->data = *data;

	/* some reasonable pre-initialization */
	st->data.act_axis_control = 0xFF;

	st->intio = ADXL345_INT1;

	indio_dev->name = st->info->name;
	indio_dev->info = &adxl345_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = adxl34x_channels;
	indio_dev->num_channels = ARRAY_SIZE(adxl34x_channels);

	if (setup) {
		/* Perform optional initial bus specific configuration */
		ret = setup(dev, st->regmap);
		if (ret)
			return ret;

		/* Enable full-resolution mode */
		ret = regmap_update_bits(st->regmap, ADXL345_REG_DATA_FORMAT,
					 data_format_mask,
					 ADXL345_DATA_FORMAT_FULL_RES);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to set data range\n");

	} else {
		/* Enable full-resolution mode (init all data_format bits) */
		ret = regmap_write(st->regmap, ADXL345_REG_DATA_FORMAT,
				   ADXL345_DATA_FORMAT_FULL_RES);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to set data range\n");
	}

	ret = regmap_read(st->regmap, ADXL345_REG_DEVID, &regval);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Error reading device ID\n");

	if (regval != ADXL345_DEVID)
		return dev_err_probe(dev, -ENODEV, "Invalid device ID: %x, expected %x\n",
				     regval, ADXL345_DEVID);

	ret = devm_add_action_or_reset(dev, adxl345_powerdown, st);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to add action or reset\n");

	/* Basic common initialization of the driver is done now */

	if (st->irq) { /* Initialization to prepare for FIFO_STREAM mode */
		ret = devm_iio_triggered_buffer_setup_ext(dev, indio_dev,
							  NULL,
							  adxl345_trigger_handler,
							  IIO_BUFFER_DIRECTION_IN,
							  &adxl345_buffer_ops,
							  adxl345_fifo_attributes);
		if (ret)
			return dev_err_probe(dev, ret, "Failed to setup triggered buffer\n");

	} else { /* Initialization to prepare for FIFO_BYPASS mode (fallback) */

		/* The following defaults to 0x00, anyway */
		fifo_ctl = 0x00 | ADXL345_FIFO_CTL_MODE(ADXL_FIFO_BYPASS);

		dev_dbg(dev, "fifo_ctl 0x%02X [0x00]\n", fifo_ctl);
		ret = regmap_write(st->regmap, ADXL345_REG_FIFO_CTL, fifo_ctl);
		if (ret < 0)
			return ret;

		/* Enable measurement mode */
		adxl345_set_measure_en(st, true);
	}
	dev_dbg(dev, "Driver operational\n");
	return devm_iio_device_register(dev, indio_dev);
}
EXPORT_SYMBOL_NS_GPL(adxl345_core_probe, IIO_ADXL345);

MODULE_AUTHOR("Eva Rachel Retuya <eraretuya@gmail.com>");
MODULE_DESCRIPTION("ADXL345 3-Axis Digital Accelerometer core driver");
MODULE_LICENSE("GPL v2");
