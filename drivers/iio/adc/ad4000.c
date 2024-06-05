// SPDX-License-Identifier: GPL-2.0+
/*
 * AD4000 SPI ADC driver
 *
 * Copyright 2024 Analog Devices Inc.
 */
#include <asm/unaligned.h>
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/sysfs.h>
#include <linux/units.h>
#include <linux/util_macros.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/buffer.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

#define AD4000_READ_COMMAND	0x54
#define AD4000_WRITE_COMMAND	0x14

#define AD4000_CONFIG_REG_MSK	0xFF

/* AD4000 Configuration Register programmable bits */
#define AD4000_CFG_STATUS		BIT(4) /* Status bits output */
#define AD4000_CFG_SPAN_COMP		BIT(3) /* Input span compression  */
#define AD4000_CFG_HIGHZ		BIT(2) /* High impedance mode  */
#define AD4000_CFG_TURBO		BIT(1) /* Turbo mode */

#define AD4000_TQUIET1_NS		190
#define AD4000_TQUIET2_NS		60
#define AD4000_TCONV_NS			320

#define AD4000_18BIT_MSK	GENMASK(31, 14)
#define AD4000_20BIT_MSK	GENMASK(31, 12)

#define AD4000_DIFF_CHANNEL(_sign, _real_bits)				\
	{								\
		.type = IIO_VOLTAGE,					\
		.indexed = 1,						\
		.differential = 1,					\
		.channel = 0,						\
		.channel2 = 1,						\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
				      BIT(IIO_CHAN_INFO_SCALE),		\
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE),\
		.scan_type = {						\
			.sign = _sign,					\
			.realbits = _real_bits,				\
			.storagebits = _real_bits > 16 ? 32 : 16,	\
			.shift = _real_bits > 16 ? 32 - _real_bits : 0,	\
			.endianness = IIO_BE,				\
		},							\
	}								\

#define AD4000_PSEUDO_DIFF_CHANNEL(_sign, _real_bits)			\
	{								\
		.type = IIO_VOLTAGE,					\
		.indexed = 1,						\
		.channel = 0,						\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |		\
				      BIT(IIO_CHAN_INFO_SCALE) |	\
				      BIT(IIO_CHAN_INFO_OFFSET),	\
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE),\
		.scan_type = {						\
			.sign = _sign,					\
			.realbits = _real_bits,				\
			.storagebits = _real_bits > 16 ? 32 : 16,	\
			.shift = _real_bits > 16 ? 32 - _real_bits : 0,	\
			.endianness = IIO_BE,				\
		},							\
	}								\

enum ad4000_ids {
	ID_AD4000,
	ID_AD4001,
	ID_AD4002,
	ID_AD4003,
	ID_AD4004,
	ID_AD4005,
	ID_AD4006,
	ID_AD4007,
	ID_AD4008,
	ID_AD4010,
	ID_AD4011,
	ID_AD4020,
	ID_AD4021,
	ID_AD4022,
	ID_ADAQ4001,
	ID_ADAQ4003,
};

enum ad4000_spi_mode {
	/* datasheet calls this "4-wire mode" (controller CS goes to ADC SDI!) */
	AD4000_SPI_MODE_DEFAULT,
	/* datasheet calls this "3-wire mode" (not related to SPI_3WIRE!) */
	AD4000_SPI_MODE_SINGLE,
};

/* maps adi,spi-mode property value to enum */
static const char * const ad4000_spi_modes[] = {
	[AD4000_SPI_MODE_DEFAULT] = "",
	[AD4000_SPI_MODE_SINGLE] = "single",
};

struct ad4000_chip_info {
	const char *dev_name;
	struct iio_chan_spec chan_spec;
};

static const struct ad4000_chip_info ad4000_chips[] = {
	[ID_AD4000] = {
		.dev_name = "ad4000",
		.chan_spec = AD4000_PSEUDO_DIFF_CHANNEL('u', 16),
	},
	[ID_AD4001] = {
		.dev_name = "ad4001",
		.chan_spec = AD4000_DIFF_CHANNEL('s', 16),
	},
	[ID_AD4002] = {
		.dev_name = "ad4002",
		.chan_spec = AD4000_PSEUDO_DIFF_CHANNEL('u', 18),
	},
	[ID_AD4003] = {
		.dev_name = "ad4003",
		.chan_spec = AD4000_DIFF_CHANNEL('s', 18),
	},
	[ID_AD4004] = {
		.dev_name = "ad4004",
		.chan_spec = AD4000_PSEUDO_DIFF_CHANNEL('u', 16),
	},
	[ID_AD4005] = {
		.dev_name = "ad4005",
		.chan_spec = AD4000_DIFF_CHANNEL('s', 16),
	},
	[ID_AD4006] = {
		.dev_name = "ad4006",
		.chan_spec = AD4000_PSEUDO_DIFF_CHANNEL('u', 18),
	},
	[ID_AD4007] = {
		.dev_name = "ad4007",
		.chan_spec = AD4000_DIFF_CHANNEL('s', 18),
	},
	[ID_AD4008] = {
		.dev_name = "ad4008",
		.chan_spec = AD4000_PSEUDO_DIFF_CHANNEL('u', 16),
	},
	[ID_AD4010] = {
		.dev_name = "ad4010",
		.chan_spec = AD4000_PSEUDO_DIFF_CHANNEL('u', 18),
	},
	[ID_AD4011] = {
		.dev_name = "ad4011",
		.chan_spec = AD4000_DIFF_CHANNEL('s', 18),
	},
	[ID_AD4020] = {
		.dev_name = "ad4020",
		.chan_spec = AD4000_DIFF_CHANNEL('s', 20),
	},
	[ID_AD4021] = {
		.dev_name = "ad4021",
		.chan_spec = AD4000_DIFF_CHANNEL('s', 20),
	},
	[ID_AD4022] = {
		.dev_name = "ad4022",
		.chan_spec = AD4000_DIFF_CHANNEL('s', 20),
	},
	[ID_ADAQ4001] = {
		.dev_name = "adaq4001",
		.chan_spec = AD4000_DIFF_CHANNEL('s', 16),
	},
	[ID_ADAQ4003] = {
		.dev_name = "adaq4003",
		.chan_spec = AD4000_DIFF_CHANNEL('s', 18),
	},
};

struct ad4000_state {
	struct spi_device *spi;
	struct gpio_desc *cnv_gpio;
	struct spi_transfer xfers[2];
	struct spi_message msg;
	int vref;
	enum ad4000_spi_mode spi_mode;
	bool span_comp;
	bool turbo_mode;
	int gain_milli;
	int scale_tbl[2][2];

	/*
	 * DMA (thus cache coherency maintenance) requires the
	 * transfer buffers to live in their own cache lines.
	 */
	struct {
		union {
			__be16 sample_buf16;
			__be32 sample_buf32;
		} data;
		s64 timestamp __aligned(8);
	} scan __aligned(IIO_DMA_MINALIGN);
	__be16 tx_buf;
	__be16 rx_buf;
};

static void ad4000_fill_scale_tbl(struct ad4000_state *st, int scale_bits,
				  const struct ad4000_chip_info *chip)
{
	int diff = chip->chan_spec.differential;
	int val, val2, tmp0, tmp1;
	u64 tmp2;

	val2 = scale_bits;
	val = st->vref / 1000;
	/*
	 * The gain is stored as a fraction of 1000 and, as we need to
	 * divide vref by gain, we invert the gain/1000 fraction.
	 * Also multiply by an extra MILLI to avoid losing precision.
	 */
	val = mult_frac(val, MILLI * MILLI, st->gain_milli);
	/* Would multiply by NANO here but we multiplied by extra MILLI */
	tmp2 = shift_right((u64)val * MICRO, val2);
	tmp0 = (int)div_s64_rem(tmp2, NANO, &tmp1);
	/* Store scale for when span compression is disabled */
	st->scale_tbl[0][0] = tmp0; /* Integer part */
	st->scale_tbl[0][1] = abs(tmp1); /* Fractional part */
	/* Store scale for when span compression is enabled */
	st->scale_tbl[1][0] = tmp0;
	/* The integer part is always zero so don't bother to divide it. */
	if (diff)
		st->scale_tbl[1][1] = DIV_ROUND_CLOSEST(abs(tmp1) * 4, 5);
	else
		st->scale_tbl[1][1] = DIV_ROUND_CLOSEST(abs(tmp1) * 9, 10);
}

static int ad4000_write_reg(struct ad4000_state *st, uint8_t val)
{
	st->tx_buf = cpu_to_be16(AD4000_WRITE_COMMAND << BITS_PER_BYTE | val);
	return spi_write(st->spi, &st->tx_buf, 2);
}

static int ad4000_read_reg(struct ad4000_state *st, unsigned int *val)
{
	struct spi_transfer t[] = {
		{
			.tx_buf = &st->tx_buf,
			.rx_buf = &st->rx_buf,
			.len = 2,
		},
	};
	int ret;

	st->tx_buf = cpu_to_be16(AD4000_READ_COMMAND << BITS_PER_BYTE);
	ret = spi_sync_transfer(st->spi, t, ARRAY_SIZE(t));
	if (ret < 0)
		return ret;

	*val = be16_to_cpu(st->rx_buf);

	return ret;
}

static void ad4000_unoptimize_msg(void *msg)
{
	spi_unoptimize_message(msg);
}

/*
 * This executes a data sample transfer for when the device connections are
 * in "3-wire" mode, selected by setting the adi,spi-mode device tree property
 * to "single". In this connection mode, the ADC SDI pin is connected to MOSI or
 * to VIO and ADC CNV pin is connected either to a SPI controller CS or to a GPIO.
 * AD4000 series of devices initiate conversions on the rising edge of CNV pin.
 *
 * If the CNV pin is connected to an SPI controller CS line (which is by default
 * active low), the ADC readings would have a latency (delay) of one read.
 * Moreover, since we also do ADC sampling for filling the buffer on triggered
 * buffer mode, the timestamps of buffer readings would be disarranged.
 * To prevent the read latency and reduce the time discrepancy between the
 * sample read request and the time of actual sampling by the ADC, do a
 * preparatory transfer to pulse the CS/CNV line.
 */
static int ad4000_prepare_3wire_mode_message(struct ad4000_state *st,
					     const struct iio_chan_spec *chan)
{
	unsigned int cnv_pulse_time = st->turbo_mode ? AD4000_TQUIET1_NS
						     : AD4000_TCONV_NS;
	struct spi_transfer *xfers = st->xfers;
	int ret;

	xfers[0].cs_change = 1;
	xfers[0].cs_change_delay.value = cnv_pulse_time;
	xfers[0].cs_change_delay.unit = SPI_DELAY_UNIT_NSECS;

	xfers[1].rx_buf = &st->scan.data;
	xfers[1].len = BITS_TO_BYTES(chan->scan_type.storagebits);
	xfers[1].delay.value = AD4000_TQUIET2_NS;
	xfers[1].delay.unit = SPI_DELAY_UNIT_NSECS;

	spi_message_init_with_transfers(&st->msg, st->xfers, 2);

	ret = spi_optimize_message(st->spi, &st->msg);
	if (ret)
		return ret;

	return devm_add_action_or_reset(&st->spi->dev, ad4000_unoptimize_msg,
					&st->msg);
}

/*
 * This executes a data sample transfer for when the device connections are
 * in "4-wire" mode, selected when the adi,spi-mode device tree
 * property is absent or empty. In this connection mode, the controller CS pin
 * is connected to ADC SDI pin and a GPIO is connected to ADC CNV pin.
 * The GPIO connected to ADC CNV pin is set outside of the SPI transfer.
 */
static int ad4000_prepare_4wire_mode_message(struct ad4000_state *st,
					     const struct iio_chan_spec *chan)
{
	unsigned int cnv_to_sdi_time = st->turbo_mode ? AD4000_TQUIET1_NS
						      : AD4000_TCONV_NS;
	struct spi_transfer *xfers = st->xfers;
	int ret;

	/*
	 * Dummy transfer to cause enough delay between CNV going high and SDI
	 * going low.
	 */
	xfers[0].cs_off = 1;
	xfers[0].delay.value = cnv_to_sdi_time;
	xfers[0].delay.unit = SPI_DELAY_UNIT_NSECS;

	xfers[1].rx_buf = &st->scan.data;
	xfers[1].len = BITS_TO_BYTES(chan->scan_type.storagebits);

	spi_message_init_with_transfers(&st->msg, st->xfers, 2);

	ret = spi_optimize_message(st->spi, &st->msg);
	if (ret)
		return ret;

	return devm_add_action_or_reset(&st->spi->dev, ad4000_unoptimize_msg,
					&st->msg);
}

static int ad4000_convert_and_acquire(struct ad4000_state *st)
{
	int ret;

	/*
	 * In 4-wire mode, the CNV line is held high for the entire conversion
	 * and acquisition process. In other modes st->cnv_gpio is NULL and is
	 * ignored (CS is wired to CNV in those cases).
	 */
	gpiod_set_value_cansleep(st->cnv_gpio, 1);
	ret = spi_sync(st->spi, &st->msg);
	gpiod_set_value_cansleep(st->cnv_gpio, 0);

	return ret;
}

static int ad4000_single_conversion(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan, int *val)
{
	struct ad4000_state *st = iio_priv(indio_dev);
	u32 sample;
	int ret;

	ret = ad4000_convert_and_acquire(st);

	if (chan->scan_type.storagebits > 16)
		sample = be32_to_cpu(st->scan.data.sample_buf32);
	else
		sample = be16_to_cpu(st->scan.data.sample_buf16);

	switch (chan->scan_type.realbits) {
	case 16:
		break;
	case 18:
		sample = FIELD_GET(AD4000_18BIT_MSK, sample);
		break;
	case 20:
		sample = FIELD_GET(AD4000_20BIT_MSK, sample);
		break;
	default:
		return -EINVAL;
	}

	if (chan->scan_type.sign == 's')
		*val = sign_extend32(sample, chan->scan_type.realbits - 1);

	return IIO_VAL_INT;
}

static int ad4000_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan, int *val,
			   int *val2, long info)
{
	struct ad4000_state *st = iio_priv(indio_dev);

	switch (info) {
	case IIO_CHAN_INFO_RAW:
		iio_device_claim_direct_scoped(return -EBUSY, indio_dev)
			return ad4000_single_conversion(indio_dev, chan, val);
		unreachable();
	case IIO_CHAN_INFO_SCALE:
		*val = st->scale_tbl[st->span_comp][0];
		*val2 = st->scale_tbl[st->span_comp][1];
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_OFFSET:
		*val = 0;
		if (st->span_comp)
			*val = mult_frac(st->vref / 1000, 1, 10);

		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int ad4000_read_avail(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     const int **vals, int *type, int *length,
			     long info)
{
	struct ad4000_state *st = iio_priv(indio_dev);

	switch (info) {
	case IIO_CHAN_INFO_SCALE:
		*vals = (int *)st->scale_tbl;
		*length = 2 * 2;
		*type = IIO_VAL_INT_PLUS_NANO;
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static int ad4000_write_raw_get_fmt(struct iio_dev *indio_dev,
				    struct iio_chan_spec const *chan, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return IIO_VAL_INT_PLUS_MICRO;
	}
}

static int ad4000_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int val, int val2,
			    long mask)
{
	struct ad4000_state *st = iio_priv(indio_dev);
	unsigned int reg_val;
	bool span_comp_en;
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		iio_device_claim_direct_scoped(return -EBUSY, indio_dev) {
			ret = ad4000_read_reg(st, &reg_val);
			if (ret < 0)
				return ret;

			span_comp_en = (val2 == st->scale_tbl[1][1]);
			reg_val &= ~AD4000_CFG_SPAN_COMP;
			reg_val |= FIELD_PREP(AD4000_CFG_SPAN_COMP, span_comp_en);

			ret = ad4000_write_reg(st, reg_val);
			if (ret < 0)
				return ret;

			st->span_comp = span_comp_en;
			return 0;
		}
		unreachable();
	default:
		return -EINVAL;
	}
}

static irqreturn_t ad4000_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ad4000_state *st = iio_priv(indio_dev);
	int ret;

	ret = ad4000_convert_and_acquire(st);
	if (ret < 0)
		goto err_out;

	iio_push_to_buffers_with_timestamp(indio_dev, &st->scan,
					   iio_get_time_ns(indio_dev));

err_out:
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

static int ad4000_reg_access(struct iio_dev *indio_dev, unsigned int reg,
			     unsigned int writeval, unsigned int *readval)
{
	struct ad4000_state *st = iio_priv(indio_dev);
	int ret;

	if (readval)
		ret = ad4000_read_reg(st, readval);
	else
		ret = ad4000_write_reg(st, writeval);

	return ret;
}

static const struct iio_info ad4000_info = {
	.read_raw = &ad4000_read_raw,
	.read_avail = &ad4000_read_avail,
	.write_raw = &ad4000_write_raw,
	.write_raw_get_fmt = &ad4000_write_raw_get_fmt,
	.debugfs_reg_access = &ad4000_reg_access,

};

static int ad4000_config(struct ad4000_state *st)
{
	unsigned int reg_val;

	if (device_property_present(&st->spi->dev, "adi,high-z-input"))
		reg_val |= FIELD_PREP(AD4000_CFG_HIGHZ, 1);

	/*
	 * The ADC SDI pin might be connected to controller CS line in which
	 * case the write might fail. This, however, does not prevent the device
	 * from functioning even though in a configuration other than the
	 * requested one.
	 */
	return ad4000_write_reg(st, reg_val);
}

static void ad4000_regulator_disable(void *reg)
{
	regulator_disable(reg);
}

static int ad4000_probe(struct spi_device *spi)
{
	const struct ad4000_chip_info *chip;
	struct regulator *vref_reg;
	struct iio_dev *indio_dev;
	struct ad4000_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	chip = spi_get_device_match_data(spi);
	if (!chip)
		return -EINVAL;

	st = iio_priv(indio_dev);
	st->spi = spi;

	ret = devm_regulator_get_enable(&spi->dev, "vdd");
	if (ret)
		return dev_err_probe(&spi->dev, ret, "Failed to enable VDD supply\n");

	ret = devm_regulator_get_enable(&spi->dev, "vio");
	if (ret)
		return dev_err_probe(&spi->dev, ret, "Failed to enable VIO supply\n");

	vref_reg = devm_regulator_get(&spi->dev, "ref");
	if (IS_ERR(vref_reg))
		return dev_err_probe(&spi->dev, PTR_ERR(vref_reg),
				     "Failed to get vref regulator\n");

	ret = regulator_enable(vref_reg);
	if (ret < 0)
		return dev_err_probe(&spi->dev, ret,
				     "Failed to enable voltage regulator\n");

	ret = devm_add_action_or_reset(&spi->dev, ad4000_regulator_disable, vref_reg);
	if (ret)
		return dev_err_probe(&spi->dev, ret,
				     "Failed to add regulator disable action\n");

	st->vref = regulator_get_voltage(vref_reg);
	if (st->vref < 0)
		return dev_err_probe(&spi->dev, st->vref, "Failed to get vref\n");

	st->cnv_gpio = devm_gpiod_get_optional(&spi->dev, "cnv", GPIOD_OUT_HIGH);
	if (IS_ERR(st->cnv_gpio))
		return dev_err_probe(&spi->dev, PTR_ERR(st->cnv_gpio),
				     "Failed to get CNV GPIO");

	ret = device_property_match_property_string(&spi->dev, "adi,spi-mode",
						    ad4000_spi_modes,
						    ARRAY_SIZE(ad4000_spi_modes));
	/* Default to 4-wire mode if adi,spi-mode property is not present */
	if (ret == -EINVAL)
		st->spi_mode = AD4000_SPI_MODE_DEFAULT;
	else if (ret < 0)
		return dev_err_probe(&spi->dev, ret,
				     "getting adi,spi-mode property failed\n");
	else
		st->spi_mode = ret;

	switch (st->spi_mode) {
	case AD4000_SPI_MODE_DEFAULT:
		ret = ad4000_prepare_4wire_mode_message(st, &chip->chan_spec);
		if (ret)
			return ret;

		break;
	case AD4000_SPI_MODE_SINGLE:
		ret = ad4000_prepare_3wire_mode_message(st, &chip->chan_spec);
		if (ret)
			return ret;

		/*
		 * In "3-wire mode", the ADC SDI line must be kept high when
		 * data is not being clocked out of the controller.
		 * Request the SPI controller to make MOSI idle high.
		 */
		spi->mode = SPI_MODE_0 | SPI_MOSI_IDLE_HIGH;
		if (spi_setup(spi))
			dev_warn(&st->spi->dev, "SPI controller setup failed\n");

		break;
	}

	ret = ad4000_config(st);
	if (ret < 0)
		dev_dbg(&st->spi->dev, "Failed to config device\n");

	indio_dev->name = chip->dev_name;
	indio_dev->info = &ad4000_info;
	indio_dev->channels = &chip->chan_spec;
	indio_dev->num_channels = 1;

	/* Hardware gain only applies to ADAQ devices */
	st->gain_milli = 1000;
	if (device_property_present(&spi->dev, "adi,gain-milli")) {

		ret = device_property_read_u32(&spi->dev, "adi,gain-milli",
					       &st->gain_milli);
		if (ret)
			return dev_err_probe(&spi->dev, ret,
					     "Failed to read gain property\n");
	}

	/*
	 * ADCs that output two's complement code have one less bit to express
	 * voltage magnitude.
	 */
	if (chip->chan_spec.scan_type.sign == 's')
		ad4000_fill_scale_tbl(st, chip->chan_spec.scan_type.realbits - 1,
				      chip);
	else
		ad4000_fill_scale_tbl(st, chip->chan_spec.scan_type.realbits,
				      chip);

	ret = devm_iio_triggered_buffer_setup(&spi->dev, indio_dev,
					      &iio_pollfunc_store_time,
					      &ad4000_trigger_handler, NULL);
	if (ret)
		return ret;

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static const struct spi_device_id ad4000_id[] = {
	{ "ad4000", (kernel_ulong_t)&ad4000_chips[ID_AD4000] },
	{ "ad4001", (kernel_ulong_t)&ad4000_chips[ID_AD4001] },
	{ "ad4002", (kernel_ulong_t)&ad4000_chips[ID_AD4002] },
	{ "ad4003", (kernel_ulong_t)&ad4000_chips[ID_AD4003] },
	{ "ad4004", (kernel_ulong_t)&ad4000_chips[ID_AD4004] },
	{ "ad4005", (kernel_ulong_t)&ad4000_chips[ID_AD4005] },
	{ "ad4006", (kernel_ulong_t)&ad4000_chips[ID_AD4006] },
	{ "ad4007", (kernel_ulong_t)&ad4000_chips[ID_AD4007] },
	{ "ad4008", (kernel_ulong_t)&ad4000_chips[ID_AD4008] },
	{ "ad4010", (kernel_ulong_t)&ad4000_chips[ID_AD4010] },
	{ "ad4011", (kernel_ulong_t)&ad4000_chips[ID_AD4011] },
	{ "ad4020", (kernel_ulong_t)&ad4000_chips[ID_AD4020] },
	{ "ad4021", (kernel_ulong_t)&ad4000_chips[ID_AD4021] },
	{ "ad4022", (kernel_ulong_t)&ad4000_chips[ID_AD4022] },
	{ "adaq4001", (kernel_ulong_t)&ad4000_chips[ID_ADAQ4001] },
	{ "adaq4003", (kernel_ulong_t)&ad4000_chips[ID_ADAQ4003] },
	{ }
};
MODULE_DEVICE_TABLE(spi, ad4000_id);

static const struct of_device_id ad4000_of_match[] = {
	{ .compatible = "adi,ad4000", .data = &ad4000_chips[ID_AD4000] },
	{ .compatible = "adi,ad4001", .data = &ad4000_chips[ID_AD4001] },
	{ .compatible = "adi,ad4002", .data = &ad4000_chips[ID_AD4002] },
	{ .compatible = "adi,ad4003", .data = &ad4000_chips[ID_AD4003] },
	{ .compatible = "adi,ad4004", .data = &ad4000_chips[ID_AD4004] },
	{ .compatible = "adi,ad4005", .data = &ad4000_chips[ID_AD4005] },
	{ .compatible = "adi,ad4006", .data = &ad4000_chips[ID_AD4006] },
	{ .compatible = "adi,ad4007", .data = &ad4000_chips[ID_AD4007] },
	{ .compatible = "adi,ad4008", .data = &ad4000_chips[ID_AD4008] },
	{ .compatible = "adi,ad4010", .data = &ad4000_chips[ID_AD4010] },
	{ .compatible = "adi,ad4011", .data = &ad4000_chips[ID_AD4011] },
	{ .compatible = "adi,ad4020", .data = &ad4000_chips[ID_AD4020] },
	{ .compatible = "adi,ad4021", .data = &ad4000_chips[ID_AD4021] },
	{ .compatible = "adi,ad4022", .data = &ad4000_chips[ID_AD4022] },
	{ .compatible = "adi,adaq4001", .data = &ad4000_chips[ID_ADAQ4001] },
	{ .compatible = "adi,adaq4003", .data = &ad4000_chips[ID_ADAQ4003] },
	{ }
};
MODULE_DEVICE_TABLE(of, ad4000_of_match);

static struct spi_driver ad4000_driver = {
	.driver = {
		.name   = "ad4000",
		.of_match_table = ad4000_of_match,
	},
	.probe          = ad4000_probe,
	.id_table       = ad4000_id,
};
module_spi_driver(ad4000_driver);

MODULE_AUTHOR("Marcelo Schmitt <marcelo.schmitt@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD4000 ADC driver");
MODULE_LICENSE("GPL");
