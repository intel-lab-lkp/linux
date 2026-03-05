// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024-2026 Analog Devices, Inc.
 * Author: Radu Sabau <radu.sabau@analog.com>
 */
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/util_macros.h>
#include <linux/units.h>
#include <linux/unaligned.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>

#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

#include <dt-bindings/iio/adc/adi,ad4691.h>

#define AD4691_NUM_REGULATORS			1
#define AD4691_MAX_ADC_MODE			4

#define AD4691_VREF_MIN				2400000
#define AD4691_VREF_MAX				5250000

/*
 * Default sampling frequency for MANUAL_MODE.
 * Each sample needs (num_channels + 1) SPI transfers of 24 bits.
 * The factor 36 = 24 * 3/2 folds in a 50% scheduling margin:
 *   freq = spi_hz / (24 * 3/2 * (num_channels + 1))
 *        = spi_hz / (36 * (num_channels + 1))
 */
#define AD4691_MANUAL_MODE_STD_FREQ(x, y)	((y) / (36 * ((x) + 1)))
#define AD4691_BITS_PER_XFER			24
#define AD4691_CNV_DUTY_CYCLE_NS		380
#define AD4691_MAX_CONV_PERIOD_US		800

#define AD4691_SEQ_ALL_CHANNELS_OFF		0x00
#define AD4691_STATE_RESET_ALL			0x01

#define AD4691_REF_CTRL_MASK			GENMASK(4, 2)

#define AD4691_DEVICE_MANUAL			0x14

#define AD4691_NOOP				0x00
#define AD4691_ADC_CHAN(ch)			((0x10 + (ch)) << 3)

#define AD4691_PRODUCT_ID_LSB_REG		0x04

#define AD4691_STATUS_REG			0x14
#define AD4691_CLAMP_STATUS1_REG		0x1A
#define AD4691_CLAMP_STATUS2_REG		0x1B
#define AD4691_DEVICE_SETUP			0x20
#define AD4691_REF_CTRL				0x21
#define AD4691_OSC_FREQ_REG			0x23
#define AD4691_STD_SEQ_CONFIG			0x25
#define AD4691_SPARE_CONTROL			0x2A

#define AD4691_OSC_EN_REG			0x180
#define AD4691_STATE_RESET_REG			0x181
#define AD4691_ADC_SETUP			0x182
#define AD4691_ACC_MASK1_REG			0x184
#define AD4691_ACC_MASK2_REG			0x185
#define AD4691_ACC_COUNT_LIMIT(n)		(0x186 + (n))
#define AD4691_ACC_COUNT_VAL			0x3F
#define AD4691_GPIO_MODE1_REG			0x196
#define AD4691_GPIO_MODE2_REG			0x197
#define AD4691_GPIO_READ			0x1A0
#define AD4691_ACC_STATUS_FULL1_REG		0x1B0
#define AD4691_ACC_STATUS_FULL2_REG		0x1B1
#define AD4691_ACC_STATUS_OVERRUN1_REG		0x1B2
#define AD4691_ACC_STATUS_OVERRUN2_REG		0x1B3
#define AD4691_ACC_STATUS_SAT1_REG		0x1B4
#define AD4691_ACC_STATUS_SAT2_REG		0x1BE
#define AD4691_ACC_SAT_OVR_REG(n)		(0x1C0 + (n))
#define AD4691_AVG_IN(n)			(0x201 + (2 * (n)))
#define AD4691_AVG_STS_IN(n)			(0x222 + (3 * (n)))
#define AD4691_ACC_IN(n)			(0x252 + (3 * (n)))
#define AD4691_ACC_STS_DATA(n)			(0x283 + (4 * (n)))

#define AD4691_CHANNEL(chan, index, real_bits, storage_bits, _shift)	\
	{								\
		.type = IIO_VOLTAGE,					\
		.indexed = 1,						\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ)	\
					   | BIT(IIO_CHAN_INFO_SCALE),	\
		.channel = chan,					\
		.scan_index = index,					\
		.scan_type = {						\
			.sign = 'u',					\
			.realbits = real_bits,				\
			.storagebits = storage_bits,			\
			.shift = _shift,				\
		},							\
	}

enum ad4691_ids {
	AD4691_ID_AD4691,
	AD4691_ID_AD4692,
	AD4691_ID_AD4693,
	AD4691_ID_AD4694,
};

enum ad4691_adc_mode {
	AD4691_CNV_CLOCK_MODE,
	AD4691_CNV_BURST_MODE,
	AD4691_AUTONOMOUS_MODE,
	AD4691_SPI_BURST_MODE,
	AD4691_MANUAL_MODE,
};

enum ad4691_gpio_mode {
	AD4691_HIGH_Z,
	AD4691_DIGITAL_OUTPUT_LOW,
	AD4691_DIGITAL_OUTPUT_HIGH,
	AD4691_DIGITAL_INPUT,
	AD4691_ADC_BUSY,
	AD4691_SEQ_DONE,
	AD4691_DATA_READY,
	AD4691_ACC_OVR_ERROR,
	AD4691_ACC_SAT_ERROR,
};

enum ad4691_int_osc_freq {
	AD4691_OSC_1MHZ = 0,
	AD4691_OSC_500KHZ,
	AD4691_OSC_400KHZ,
	AD4691_OSC_250KHZ,
	AD4691_OSC_200KHZ,
	AD4691_OSC_167KHZ,
	AD4691_OSC_133KHZ,
	AD4691_OSC_125KHZ,
	AD4691_OSC_100KHZ,
	AD4691_OSC_50KHZ,
	AD4691_OSC_25KHZ,
	AD4691_OSC_12P5KHZ,
	AD4691_OSC_10KHZ,
	AD4691_OSC_5KHZ,
	AD4691_OSC_2P5KHZ,
	AD4691_OSC_1P25KHZ,
};

enum ad4691_ref_ctrl {
	AD4691_VREF_2P5 = 0,
	AD4691_VREF_3P0,
	AD4691_VREF_3P3,
	AD4691_VREF_4P096,
	AD4691_VREF_5P0,
};

static int ad4691_int_osc_val[] = {
	[AD4691_OSC_1MHZ] = 1000000,
	[AD4691_OSC_500KHZ] = 500000,
	[AD4691_OSC_400KHZ] = 400000,
	[AD4691_OSC_250KHZ] = 250000,
	[AD4691_OSC_200KHZ] = 200000,
	[AD4691_OSC_167KHZ] = 167000,
	[AD4691_OSC_133KHZ] = 133000,
	[AD4691_OSC_125KHZ] = 125000,
	[AD4691_OSC_100KHZ] = 100000,
	[AD4691_OSC_50KHZ] = 50000,
	[AD4691_OSC_25KHZ] = 25000,
	[AD4691_OSC_12P5KHZ] = 12500,
	[AD4691_OSC_10KHZ] = 10000,
	[AD4691_OSC_5KHZ] = 5000,
	[AD4691_OSC_2P5KHZ] = 2500,
	[AD4691_OSC_1P25KHZ] = 1250,
};

struct ad4691_chip_info {
	const struct iio_chan_spec *channels;
	const char *name;
	u8 product_id;
	int num_channels;
	int max_rate;
};

static const struct iio_chan_spec ad4691_channels[] = {
	AD4691_CHANNEL(0, 0, 16, 32, 0),
	AD4691_CHANNEL(1, 1, 16, 32, 0),
	AD4691_CHANNEL(2, 2, 16, 32, 0),
	AD4691_CHANNEL(3, 3, 16, 32, 0),
	AD4691_CHANNEL(4, 4, 16, 32, 0),
	AD4691_CHANNEL(5, 5, 16, 32, 0),
	AD4691_CHANNEL(6, 6, 16, 32, 0),
	AD4691_CHANNEL(7, 7, 16, 32, 0),
	AD4691_CHANNEL(8, 8, 16, 32, 0),
	AD4691_CHANNEL(9, 9, 16, 32, 0),
	AD4691_CHANNEL(10, 10, 16, 32, 0),
	AD4691_CHANNEL(11, 11, 16, 32, 0),
	AD4691_CHANNEL(12, 12, 16, 32, 0),
	AD4691_CHANNEL(13, 13, 16, 32, 0),
	AD4691_CHANNEL(14, 14, 16, 32, 0),
	AD4691_CHANNEL(15, 15, 16, 32, 0)
};

static const struct iio_chan_spec ad4693_channels[] = {
	AD4691_CHANNEL(0, 0, 16, 32, 0),
	AD4691_CHANNEL(1, 1, 16, 32, 0),
	AD4691_CHANNEL(2, 2, 16, 32, 0),
	AD4691_CHANNEL(3, 3, 16, 32, 0),
	AD4691_CHANNEL(4, 4, 16, 32, 0),
	AD4691_CHANNEL(5, 5, 16, 32, 0),
	AD4691_CHANNEL(6, 6, 16, 32, 0),
	AD4691_CHANNEL(7, 7, 16, 32, 0)
};

static const struct iio_chan_spec ad4691_manual_channels[] = {
	AD4691_CHANNEL(0, 0, 16, 24, 8),
	AD4691_CHANNEL(1, 1, 16, 24, 8),
	AD4691_CHANNEL(2, 2, 16, 24, 8),
	AD4691_CHANNEL(3, 3, 16, 24, 8),
	AD4691_CHANNEL(4, 4, 16, 24, 8),
	AD4691_CHANNEL(5, 5, 16, 24, 8),
	AD4691_CHANNEL(6, 6, 16, 24, 8),
	AD4691_CHANNEL(7, 7, 16, 24, 8),
	AD4691_CHANNEL(8, 8, 16, 24, 8),
	AD4691_CHANNEL(9, 9, 16, 24, 8),
	AD4691_CHANNEL(10, 10, 16, 24, 8),
	AD4691_CHANNEL(11, 11, 16, 24, 8),
	AD4691_CHANNEL(12, 12, 16, 24, 8),
	AD4691_CHANNEL(13, 13, 16, 24, 8),
	AD4691_CHANNEL(14, 14, 16, 24, 8),
	AD4691_CHANNEL(15, 15, 16, 24, 8)
};

static const struct iio_chan_spec ad4693_manual_channels[] = {
	AD4691_CHANNEL(0, 0, 16, 24, 8),
	AD4691_CHANNEL(1, 1, 16, 24, 8),
	AD4691_CHANNEL(2, 2, 16, 24, 8),
	AD4691_CHANNEL(3, 3, 16, 24, 8),
	AD4691_CHANNEL(4, 4, 16, 24, 8),
	AD4691_CHANNEL(5, 5, 16, 24, 8),
	AD4691_CHANNEL(6, 6, 16, 24, 8),
	AD4691_CHANNEL(7, 7, 16, 24, 8)
};

static const struct ad4691_chip_info ad4691_chips[] = {
	[AD4691_ID_AD4691] = {
		.channels = ad4691_channels,
		.name = "ad4691",
		.product_id = 0x11,
		.num_channels = ARRAY_SIZE(ad4691_channels),
		.max_rate = 500000,
	},
	[AD4691_ID_AD4692] = {
		.channels = ad4691_channels,
		.name = "ad4692",
		.product_id = 0x12,
		.num_channels = ARRAY_SIZE(ad4691_channels),
		.max_rate = 1000000,
	},
	[AD4691_ID_AD4693] = {
		.channels = ad4693_channels,
		.name = "ad4693",
		.product_id = 0x13,
		.num_channels = ARRAY_SIZE(ad4693_channels),
		.max_rate = 500000,
	},
	[AD4691_ID_AD4694] = {
		.channels = ad4693_channels,
		.name = "ad4694",
		.product_id = 0x14,
		.num_channels = ARRAY_SIZE(ad4693_channels),
		.max_rate = 1000000,
	},
};

struct ad4691_state {
	const struct ad4691_chip_info	*chip;
	struct spi_device		*spi;
	struct regmap			*regmap;

	unsigned long			ref_clk_rate;
	struct pwm_device		*conv_trigger;

	struct regulator_bulk_data	regulators[AD4691_NUM_REGULATORS];

	struct iio_trigger		*trig;

	enum ad4691_adc_mode		adc_mode;

	int				vref;
	u64				cnv_period;
	/*
	 * Synchronize access to members of the driver state, and ensure
	 * atomicity of consecutive SPI operations.
	 */
	struct mutex			lock;

	/* hrtimer for MANUAL_MODE triggered buffer (non-offload) */
	struct hrtimer			sampling_timer;
	ktime_t				sampling_period;

	/* DMA (thus cache coherency maintenance) may require the
	 * transfer buffers to live in their own cache lines.
	 * Make the buffer large enough for one 24 bit sample and one 64 bit
	 * aligned 64 bit timestamp.
	 */
	unsigned char rx_data[ALIGN(3, sizeof(s64)) + sizeof(s64)]	__aligned(IIO_DMA_MINALIGN);
	unsigned char tx_data[ALIGN(3, sizeof(s64)) + sizeof(s64)]	__aligned(IIO_DMA_MINALIGN);
	/* Scan buffer for triggered buffer push (one sample + timestamp) */
	struct {
		u32 val;
		s64 ts __aligned(8);
	} scan __aligned(IIO_DMA_MINALIGN);
};

static void ad4691_disable_regulators(void *data)
{
	struct ad4691_state *st = data;

	regulator_bulk_disable(AD4691_NUM_REGULATORS, st->regulators);
}

static void ad4691_disable_regulator(void *data)
{
	struct regulator *reg = data;

	regulator_disable(reg);
}

static void ad4691_disable_pwm(void *data)
{
	struct pwm_device *pwm = data;
	struct pwm_state state;

	pwm_get_state(pwm, &state);
	state.enabled = false;
	pwm_apply_might_sleep(pwm, &state);
}

static int ad4691_regulators_get(struct ad4691_state *st)
{
	struct device *dev = &st->spi->dev;
	struct regulator *ref;
	int ret;

	st->regulators[0].supply = "vio";

	ret = devm_regulator_bulk_get(dev, AD4691_NUM_REGULATORS,
				      st->regulators);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get VIO regulator\n");

	ret = regulator_bulk_enable(AD4691_NUM_REGULATORS, st->regulators);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable regulators\n");

	ret = devm_add_action_or_reset(dev, ad4691_disable_regulators, st);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register regulator disable action\n");

	ref = devm_regulator_get_optional(dev, "vref");
	if (IS_ERR(ref)) {
		if (PTR_ERR(ref) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(ref),
					     "Failed to get vref regulator");

		/* Internal REFIN must be used if optional REF isn't used. */
		ref = devm_regulator_get(dev, "vrefin");
		if (IS_ERR(ref))
			return dev_err_probe(dev, PTR_ERR(ref),
					     "Failed to get vrefin regulator");
	}

	ret = regulator_enable(ref);
	if (ret) {
		dev_err_probe(dev, ret, "Failed to enable specified ref supply\n");
		return ret;
	}

	ret = devm_add_action_or_reset(dev, ad4691_disable_regulator, ref);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register ref disable action\n");

	st->vref = regulator_get_voltage(ref);
	if (st->vref < AD4691_VREF_MIN || st->vref > AD4691_VREF_MAX)
		return dev_err_probe(dev, -EINVAL, "vref(%d) must be under [%u %u]\n",
				     st->vref, AD4691_VREF_MIN, AD4691_VREF_MAX);

	return 0;
}

static int ad4691_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct ad4691_state *st = context;
	unsigned char buf[6];
	int ret;

	buf[0] = (reg >> 8) | 0x80;
	buf[1] = reg & 0xFF;

	switch (reg) {
	case 0 ... AD4691_OSC_FREQ_REG:
	case AD4691_SPARE_CONTROL ... AD4691_ACC_SAT_OVR_REG(15):
		ret = spi_write_then_read(st->spi, &buf[0], 2, &buf[2], 1);
		if (!ret)
			*val = buf[2];
		break;
	case AD4691_STD_SEQ_CONFIG:
	case AD4691_AVG_IN(0) ... AD4691_AVG_IN(15):
		ret = spi_write_then_read(st->spi, &buf[0], 2, &buf[2], 2);
		if (!ret)
			*val = get_unaligned_be16(&buf[2]);
		break;
	case AD4691_AVG_STS_IN(0) ... AD4691_AVG_STS_IN(15):
		ret = spi_write_then_read(st->spi, &buf[0], 2, &buf[2], 3);
		if (!ret)
			*val = get_unaligned_be24(&buf[2]);
		break;
	case AD4691_ACC_IN(0) ... AD4691_ACC_IN(15):
		ret = spi_write_then_read(st->spi, &buf[0], 2, &buf[2], 3);
		if (!ret)
			*val = get_unaligned_be24(&buf[2]);
		break;
	case AD4691_ACC_STS_DATA(0) ... AD4691_ACC_STS_DATA(15):
		ret = spi_write_then_read(st->spi, &buf[0], 2, &buf[2], 4);
		if (!ret)
			*val = get_unaligned_be32(&buf[2]);
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static int ad4691_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct ad4691_state *st = context;
	unsigned char buf[4];

	buf[0] = (reg >> 8);
	buf[1] = reg & 0xFF;

	switch (reg) {
	case 0 ... AD4691_OSC_FREQ_REG:
	case AD4691_SPARE_CONTROL ... AD4691_GPIO_MODE2_REG:
		if (val > 0xFF)
			return -EINVAL;
		buf[2] = val;

		return spi_write(st->spi, buf, 3);
	case AD4691_STD_SEQ_CONFIG:
		if (val > 0xFFFF)
			return -EINVAL;
		put_unaligned_be16(val, &buf[2]);

		return spi_write(st->spi, buf, 4);
	default:
		return -EINVAL;
	}
}

static bool ad4691_volatile_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case AD4691_STATUS_REG:
	case AD4691_CLAMP_STATUS1_REG:
	case AD4691_CLAMP_STATUS2_REG:
	case AD4691_GPIO_READ:
	case AD4691_ACC_STATUS_FULL1_REG ... AD4691_ACC_STATUS_SAT2_REG:
	case AD4691_ACC_SAT_OVR_REG(0) ... AD4691_ACC_SAT_OVR_REG(15):
	case AD4691_AVG_IN(0) ... AD4691_AVG_IN(15):
	case AD4691_AVG_STS_IN(0) ... AD4691_AVG_STS_IN(15):
	case AD4691_ACC_IN(0) ... AD4691_ACC_IN(15):
	case AD4691_ACC_STS_DATA(0) ... AD4691_ACC_STS_DATA(15):
		return true;
	default:
		return false;
	}
}

static bool ad4691_readable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0 ... AD4691_OSC_FREQ_REG:
	case AD4691_SPARE_CONTROL ... AD4691_ACC_SAT_OVR_REG(15):
	case AD4691_STD_SEQ_CONFIG:
	case AD4691_AVG_IN(0) ... AD4691_AVG_IN(15):
	case AD4691_AVG_STS_IN(0) ... AD4691_AVG_STS_IN(15):
	case AD4691_ACC_IN(0) ... AD4691_ACC_IN(15):
	case AD4691_ACC_STS_DATA(0) ... AD4691_ACC_STS_DATA(15):
		return true;
	default:
		return false;
	}
}

static bool ad4691_writeable_reg(struct device *dev, unsigned int reg)
{
	switch (reg) {
	case 0 ... AD4691_OSC_FREQ_REG:
	case AD4691_STD_SEQ_CONFIG:
	case AD4691_SPARE_CONTROL ... AD4691_GPIO_MODE2_REG:
		return true;
	default:
		return false;
	}
}

static const struct regmap_config ad4691_regmap_config = {
	.reg_bits = 16,
	.val_bits = 32,
	.reg_read = ad4691_reg_read,
	.reg_write = ad4691_reg_write,
	.volatile_reg = ad4691_volatile_reg,
	.readable_reg = ad4691_readable_reg,
	.writeable_reg = ad4691_writeable_reg,
	.max_register = AD4691_ACC_STS_DATA(15),
	.cache_type = REGCACHE_RBTREE,
};

static int ad4691_transfer(struct ad4691_state *st, int command,
			   unsigned int *val)
{
	struct spi_transfer xfer = {
		.tx_buf = st->tx_data,
		.rx_buf = st->rx_data,
		.len = 3,
	};
	int ret;

	memcpy(st->tx_data, &command, 3);

	ret = spi_sync_transfer(st->spi, &xfer, 1);
	if (ret)
		return ret;

	*val = get_unaligned_be24(st->rx_data);

	return 0;
}

static int ad4691_get_sampling_freq(struct ad4691_state *st)
{
	unsigned int val;
	int ret;

	switch (st->adc_mode) {
	case AD4691_MANUAL_MODE:
		return DIV_ROUND_CLOSEST_ULL(NSEC_PER_SEC,
					     ktime_to_ns(st->sampling_period));
	case AD4691_CNV_CLOCK_MODE:
		return DIV_ROUND_CLOSEST_ULL(NSEC_PER_SEC,
					     pwm_get_period(st->conv_trigger));
	case AD4691_CNV_BURST_MODE:
	case AD4691_AUTONOMOUS_MODE:
	case AD4691_SPI_BURST_MODE:
		ret = regmap_read(st->regmap, AD4691_OSC_FREQ_REG, &val);
		if (ret)
			return ret;

		return ad4691_int_osc_val[val];
	default:
		return -EINVAL;
	}
}

static int __ad4691_set_sampling_freq(struct ad4691_state *st, int freq)
{
	unsigned long long target, ref_clk_period_ns;
	struct pwm_state cnv_state;

	pwm_init_state(st->conv_trigger, &cnv_state);

	freq = clamp(freq, 1, st->chip->max_rate);
	target = DIV_ROUND_CLOSEST_ULL(st->ref_clk_rate, freq);
	ref_clk_period_ns = DIV_ROUND_CLOSEST_ULL(NANO, st->ref_clk_rate);
	st->cnv_period = ref_clk_period_ns * target;
	cnv_state.period = ref_clk_period_ns * target;
	cnv_state.duty_cycle = AD4691_CNV_DUTY_CYCLE_NS;
	cnv_state.enabled = false;

	return pwm_apply_might_sleep(st->conv_trigger, &cnv_state);
}

/*
 * ad4691_cnv_burst_period_ns - Compute the CNV_BURST_MODE PWM period.
 * @st: Driver state.
 * @n_active: Number of active channels.
 *
 * The period must cover the full conversion time tOSC*(n_active+1) plus
 * the SPI transfer time for reading the accumulator results and issuing
 * STATE_RESET, with a 50% margin on the SPI portion to absorb jitter.
 *
 * Return: Period in nanoseconds.
 */
static u64 ad4691_cnv_burst_period_ns(struct ad4691_state *st,
				      int n_active)
{
	unsigned int osc_idx = AD4691_OSC_1MHZ;
	u64 osc_freq, conv_time_ns, spi_bits, spi_time_ns;

	regmap_read(st->regmap, AD4691_OSC_FREQ_REG, &osc_idx);
	if (osc_idx >= ARRAY_SIZE(ad4691_int_osc_val))
		osc_idx = AD4691_OSC_1MHZ;

	osc_freq = ad4691_int_osc_val[osc_idx];
	conv_time_ns = div64_u64((u64)(n_active + 1) * NSEC_PER_SEC, osc_freq);

	spi_bits = (u64)n_active * 32 + 24;
	spi_time_ns = div64_u64(spi_bits * NSEC_PER_SEC, st->spi->max_speed_hz);

	/* 50% margin on SPI time absorbs OS scheduling jitter. */
	return conv_time_ns + spi_time_ns * 3 / 2;
}

static int ad4691_pwm_get(struct spi_device *spi, struct ad4691_state *st)
{
	struct clk *ref_clk;
	int ret;

	ref_clk = devm_clk_get_enabled(&spi->dev, "ref_clk");
	if (IS_ERR(ref_clk))
		return dev_err_probe(&spi->dev, PTR_ERR(ref_clk),
				     "Failed to get ref_clk\n");

	st->ref_clk_rate = clk_get_rate(ref_clk);

	st->conv_trigger = devm_pwm_get(&spi->dev, "cnv");
	if (IS_ERR(st->conv_trigger)) {
		return dev_err_probe(&spi->dev, PTR_ERR(st->conv_trigger),
				     "Failed to get cnv pwm\n");
	}

	ret = devm_add_action_or_reset(&spi->dev, ad4691_disable_pwm,
				       st->conv_trigger);
	if (ret)
		return dev_err_probe(&spi->dev, ret,
				     "Failed to register PWM disable action\n");

	switch (st->adc_mode) {
	case AD4691_CNV_CLOCK_MODE:
		return __ad4691_set_sampling_freq(st, st->chip->max_rate);
	case AD4691_CNV_BURST_MODE: {
		/*
		 * In CNV Burst Mode, the internal oscillator drives per-channel
		 * conversions. The PWM triggers each burst cycle; its period
		 * must cover the full conversion time tOSC*(n+1) plus SPI
		 * transfer time. Use worst-case channel count here; the period
		 * is refined at buffer enable time when the active count is known.
		 */
		u64 period_ns = ad4691_cnv_burst_period_ns(st, st->chip->num_channels);
		int pwm_freq = (int)max(1ULL, div64_u64(NSEC_PER_SEC, period_ns));

		return __ad4691_set_sampling_freq(st, pwm_freq);
	}
	default:
		return -EOPNOTSUPP;
	}
}

static int ad4691_set_sampling_freq(struct iio_dev *indio_dev, unsigned int freq)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	int ret, i;

	if (!iio_device_claim_direct(indio_dev))
		return -EBUSY;

	mutex_lock(&st->lock);
	switch (st->adc_mode) {
	case AD4691_MANUAL_MODE:
		if (!freq || freq > st->chip->max_rate) {
			ret = -EINVAL;
			goto exit;
		}

		st->sampling_period = ns_to_ktime(DIV_ROUND_CLOSEST_ULL
			(NSEC_PER_SEC, freq));
		ret = 0;
		goto exit;
	case AD4691_CNV_CLOCK_MODE:
		if (!st->conv_trigger) {
			ret = -ENODEV;
			goto exit;
		}

		if (!freq || freq > st->chip->max_rate) {
			ret = -EINVAL;
			goto exit;
		}

		ret = __ad4691_set_sampling_freq(st, freq);
		break;
	case AD4691_CNV_BURST_MODE: {
		u64 period_ns;
		int pwm_freq;

		i = find_closest_descending(freq, ad4691_int_osc_val, 16);
		ret = regmap_write(st->regmap, AD4691_OSC_FREQ_REG, i);
		if (ret)
			goto exit;

		/*
		 * Compute the worst-case PWM period using the maximum channel
		 * count. The exact period is refined at buffer enable time when
		 * the active channel count is known.
		 */
		period_ns = ad4691_cnv_burst_period_ns(st, st->chip->num_channels);
		pwm_freq = (int)max(1ULL, div64_u64(NSEC_PER_SEC, period_ns));
		ret = __ad4691_set_sampling_freq(st, pwm_freq);

		break;
	}
	case AD4691_AUTONOMOUS_MODE:
	case AD4691_SPI_BURST_MODE:
		i = find_closest_descending(freq, ad4691_int_osc_val, 16);
		ret = regmap_write(st->regmap, AD4691_OSC_FREQ_REG, i);
		break;
	default:
		ret = -EINVAL;
		break;
	}

exit:
	mutex_unlock(&st->lock);
	iio_device_release_direct(indio_dev);
	return ret;
}

static int ad4691_sampling_enable(struct ad4691_state *st, bool enable)
{
	struct pwm_state conv_state = { };

	switch (st->adc_mode) {
	case AD4691_CNV_CLOCK_MODE:
	case AD4691_CNV_BURST_MODE:
		conv_state.period = st->cnv_period;
		conv_state.duty_cycle = AD4691_CNV_DUTY_CYCLE_NS;
		conv_state.polarity = PWM_POLARITY_NORMAL;
		conv_state.enabled = enable;

		return pwm_apply_might_sleep(st->conv_trigger, &conv_state);
	case AD4691_AUTONOMOUS_MODE:
		return regmap_write(st->regmap, AD4691_OSC_EN_REG, enable);
	case AD4691_SPI_BURST_MODE:
		if (enable)
			return regmap_write(st->regmap, AD4691_OSC_EN_REG, enable);

		/*
		 * SPI Burst Mode is self-terminating: the oscillator stops
		 * automatically after the configured number of conversions.
		 * No explicit disable write is needed.
		 */
		return 0;
	default:
		return -EINVAL;
	}
}

/*
 * Return the time in microseconds for a single-channel conversion driven by
 * the internal oscillator. A single read requires (n_active + 1) = 2 oscillator
 * periods (n_active = 1).
 */
static unsigned long ad4691_osc_single_conv_us(struct ad4691_state *st)
{
	unsigned int osc_idx = AD4691_OSC_1MHZ;

	regmap_read(st->regmap, AD4691_OSC_FREQ_REG, &osc_idx);
	if (osc_idx >= ARRAY_SIZE(ad4691_int_osc_val))
		osc_idx = AD4691_OSC_1MHZ;

	return DIV_ROUND_UP(2UL * USEC_PER_SEC, ad4691_int_osc_val[osc_idx]);
}

static int ad4691_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan, int *val,
			   int *val2, long info)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	unsigned int reg_val;
	int ret;

	switch (info) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;

		switch (st->adc_mode) {
		case AD4691_CNV_CLOCK_MODE:
		case AD4691_CNV_BURST_MODE:
		case AD4691_AUTONOMOUS_MODE:
		case AD4691_SPI_BURST_MODE:
			ret = regmap_write(st->regmap, AD4691_STATE_RESET_REG,
					   AD4691_STATE_RESET_ALL);
			if (ret)
				goto done;

			ret = regmap_write(st->regmap, AD4691_STD_SEQ_CONFIG, BIT(chan->channel));
			if (ret)
				goto done;

			if (chan->channel < 8) {
				ret = regmap_write(st->regmap, AD4691_ACC_MASK1_REG,
						   ~BIT(chan->channel) & 0xFF);
				if (ret)
					goto done;
				ret = regmap_write(st->regmap, AD4691_ACC_MASK2_REG,
						   0xFF);
			} else {
				ret = regmap_write(st->regmap, AD4691_ACC_MASK1_REG,
						   0xFF);
				if (ret)
					goto done;
				ret = regmap_write(st->regmap, AD4691_ACC_MASK2_REG,
						   ~BIT(chan->channel - 8) & 0xFF);
			}

			if (ret)
				goto done;

			ret = ad4691_sampling_enable(st, true);
			if (ret)
				goto done;

			/*
			 * Wait for conversion to complete using a timed delay.
			 * CNV_CLOCK_MODE conversion time is bounded by
			 * AD4691_MAX_CONV_PERIOD_US. All other modes are driven by
			 * the internal oscillator; two oscillator periods cover a
			 * single-channel read (n_active + 1 = 2).
			 */
			if (st->adc_mode == AD4691_CNV_CLOCK_MODE) {
				usleep_range(AD4691_MAX_CONV_PERIOD_US,
					     AD4691_MAX_CONV_PERIOD_US + 100);
			} else {
				unsigned long conv_us = ad4691_osc_single_conv_us(st);

				usleep_range(conv_us, conv_us + conv_us / 4 + 1);
			}

			ret = ad4691_sampling_enable(st, false);
			if (ret)
				goto done;

			ret = regmap_read(st->regmap,
					  AD4691_AVG_IN(chan->channel),
					  &reg_val);

			*val = reg_val;
			regmap_write(st->regmap, AD4691_STATE_RESET_REG,
					AD4691_STATE_RESET_ALL);

			break;
		case AD4691_MANUAL_MODE:
			ret = ad4691_transfer(st, AD4691_ADC_CHAN(chan->channel), val);
			if (ret)
				goto done;

			ret = ad4691_transfer(st, AD4691_NOOP, val);
			if (ret)
				goto done;

			/* Extract ADC data from the 24-bit SPI frame */
			*val = *val >> 8;
			break;
		default:
			ret = -EINVAL;
			goto done;
		}

done:
		iio_device_release_direct(indio_dev);

		if (ret)
			return ret;

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = ad4691_get_sampling_freq(st);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = st->vref / 1000;
		*val2 = chan->scan_type.realbits;
		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static int ad4691_read_avail(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     const int **vals, int *type, int *length,
			     long info)
{
	struct ad4691_state *st = iio_priv(indio_dev);

	switch (info) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		switch (st->adc_mode) {
		case AD4691_CNV_CLOCK_MODE:
		case AD4691_MANUAL_MODE:
			return -EOPNOTSUPP;
		case AD4691_CNV_BURST_MODE:
		case AD4691_AUTONOMOUS_MODE:
		case AD4691_SPI_BURST_MODE:
			*vals = ad4691_int_osc_val;
			*length = ARRAY_SIZE(ad4691_int_osc_val);
			*type = IIO_VAL_INT;

			return IIO_AVAIL_LIST;
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static int ad4691_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val, int val2, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		return ad4691_set_sampling_freq(indio_dev, val);
	default:
		return -EINVAL;
	}
}

static int ad4691_reg_access(struct iio_dev *indio_dev, unsigned int reg,
			     unsigned int writeval, unsigned int *readval)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	int ret;

	if (st->adc_mode == AD4691_MANUAL_MODE)
		return -EOPNOTSUPP;

	mutex_lock(&st->lock);
	if (readval) {
		ret = regmap_read(st->regmap, reg, readval);
		goto mutex_unlock;
	}

	ret = regmap_write(st->regmap, reg, writeval);

mutex_unlock:
	mutex_unlock(&st->lock);
	return ret;
}

static int ad4691_buffer_postenable(struct iio_dev *indio_dev)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	int n_active = hweight_long(*indio_dev->active_scan_mask);
	int ret;

	if (st->adc_mode == AD4691_MANUAL_MODE) {
		u64 min_period_ns;

		/* N+1 transfers needed for N channels, with 50% overhead */
		min_period_ns = div64_u64((u64)(n_active + 1) * AD4691_BITS_PER_XFER *
					  NSEC_PER_SEC * 3,
					  st->spi->max_speed_hz * 2);

		if (ktime_to_ns(st->sampling_period) < min_period_ns) {
			dev_err(&st->spi->dev,
				"Sampling period %lld ns too short for %d channels. Min: %llu ns\n",
				ktime_to_ns(st->sampling_period), n_active,
				min_period_ns);
			return -EINVAL;
		}

		hrtimer_start(&st->sampling_timer, st->sampling_period,
			      HRTIMER_MODE_REL);
		return 0;
	}

	ret = regmap_write(st->regmap, AD4691_STATE_RESET_REG,
			   AD4691_STATE_RESET_ALL);
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, AD4691_ACC_MASK1_REG,
				 ~(*indio_dev->active_scan_mask) & 0xFF);
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, AD4691_ACC_MASK2_REG,
				 ~(*indio_dev->active_scan_mask >> 8) & 0xFF);
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, AD4691_STD_SEQ_CONFIG,
				*indio_dev->active_scan_mask);
	if (ret)
		return ret;

	if (st->adc_mode == AD4691_AUTONOMOUS_MODE)
		ret = regmap_write(st->regmap, AD4691_GPIO_MODE1_REG, AD4691_ADC_BUSY);
	else
		ret = regmap_write(st->regmap, AD4691_GPIO_MODE1_REG, AD4691_DATA_READY);
	if (ret)
		return ret;

	switch (st->adc_mode) {
	case AD4691_CNV_BURST_MODE:
		/*
		 * Recompute the PWM period now that the active channel count is
		 * known. The period must cover one full burst cycle: oscillator
		 * conversion time (tOSC * (n+1)) plus all SPI transfer time.
		 */
		st->cnv_period = ad4691_cnv_burst_period_ns(st, n_active, false);
		fallthrough;
	case AD4691_AUTONOMOUS_MODE:
	case AD4691_SPI_BURST_MODE:
	case AD4691_CNV_CLOCK_MODE:
		return ad4691_sampling_enable(st, true);
	default:
		return -EOPNOTSUPP;
	}
}

static int ad4691_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	int ret;

	switch (st->adc_mode) {
	case AD4691_AUTONOMOUS_MODE:
	case AD4691_SPI_BURST_MODE:
	case AD4691_CNV_BURST_MODE:
	case AD4691_CNV_CLOCK_MODE:
		ret = ad4691_sampling_enable(st, false);
		if (ret)
			return ret;
		break;
	case AD4691_MANUAL_MODE:
		hrtimer_cancel_wait_running(&st->sampling_timer);
		return 0;
	default:
		return -EOPNOTSUPP;
	}

	ret = regmap_write(st->regmap, AD4691_STD_SEQ_CONFIG,
			   AD4691_SEQ_ALL_CHANNELS_OFF);
	if (ret)
		return ret;

	return regmap_write(st->regmap, AD4691_STATE_RESET_REG,
			    AD4691_STATE_RESET_ALL);
}

static const struct iio_buffer_setup_ops ad4691_buffer_setup_ops = {
	.postenable = &ad4691_buffer_postenable,
	.postdisable = &ad4691_buffer_postdisable,
};

static irqreturn_t ad4691_irq(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct ad4691_state *st = iio_priv(indio_dev);

	/*
	 * DATA_READY has asserted: stop conversions before reading so the
	 * accumulator does not continue sampling while the trigger handler
	 * processes the data. Then fire the IIO trigger to push the sample
	 * to the buffer.
	 *
	 * In direct (read_raw) mode the buffer is not enabled; read_raw uses
	 * a timed delay and stops conversions itself, so skip the trigger poll.
	 */
	ad4691_sampling_enable(st, false);

	if (iio_buffer_enabled(indio_dev))
		iio_trigger_poll(st->trig);

	return IRQ_HANDLED;
}

static enum hrtimer_restart ad4691_sampling_timer_handler(struct hrtimer *timer)
{
	struct ad4691_state *st = container_of(timer, struct ad4691_state,
					       sampling_timer);

	iio_trigger_poll(st->trig);
	hrtimer_forward_now(timer, st->sampling_period);

	return HRTIMER_RESTART;
}

static const struct iio_trigger_ops ad4691_trigger_ops = {
	.validate_device = iio_trigger_validate_own_device,
};

static irqreturn_t ad4691_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ad4691_state *st = iio_priv(indio_dev);
	unsigned int val;
	int ret, i;

	mutex_lock(&st->lock);

	if (st->adc_mode == AD4691_MANUAL_MODE) {
		unsigned int prev_val;
		int prev_chan = -1;

		/*
		 * MANUAL_MODE with CNV tied to CS: each transfer triggers a
		 * conversion AND returns the previous conversion's result.
		 * First transfer returns garbage, so we do N+1 transfers for
		 * N channels.
		 */
		iio_for_each_active_channel(indio_dev, i) {
			ret = ad4691_transfer(st, AD4691_ADC_CHAN(i), &val);
			if (ret)
				goto done;

			/* Push previous channel's data (skip first - garbage) */
			if (prev_chan >= 0) {
				st->scan.val = prev_val;
				iio_push_to_buffers_with_ts(indio_dev,
					&st->scan, sizeof(st->scan),
					iio_get_time_ns(indio_dev));
			}
			prev_val = val;
			prev_chan = i;
		}

		/* Final NOOP transfer to get last channel's data */
		ret = ad4691_transfer(st, AD4691_NOOP, &val);
		if (ret)
			goto done;

		st->scan.val = val;
		iio_push_to_buffers_with_ts(indio_dev, &st->scan, sizeof(st->scan),
					    iio_get_time_ns(indio_dev));
		goto done;
	}

	for (i = 0; i < st->chip->num_channels; i++) {
		if (BIT(i) & *indio_dev->active_scan_mask) {
			ret = regmap_read(st->regmap, AD4691_AVG_IN(i), &val);
			if (ret)
				goto done;

			st->scan.val = val;
			iio_push_to_buffers_with_ts(indio_dev, &st->scan, sizeof(st->scan),
						    iio_get_time_ns(indio_dev));
		}
	}

	regmap_write(st->regmap, AD4691_STATE_RESET_REG, AD4691_STATE_RESET_ALL);

	/* START next conversion. */
	switch (st->adc_mode) {
	case AD4691_CNV_CLOCK_MODE:
	case AD4691_CNV_BURST_MODE:
	case AD4691_AUTONOMOUS_MODE:
	case AD4691_SPI_BURST_MODE:
		ad4691_sampling_enable(st, true);
		break;
	case AD4691_MANUAL_MODE:
	default:
		break;
	}

	iio_trigger_notify_done(indio_dev->trig);
	mutex_unlock(&st->lock);
	return IRQ_HANDLED;
done:
	mutex_unlock(&st->lock);
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

static const struct iio_info ad4691_info = {
	.read_raw = &ad4691_read_raw,
	.read_avail = &ad4691_read_avail,
	.write_raw = &ad4691_write_raw,
	.debugfs_reg_access = &ad4691_reg_access,
};

static const struct spi_device_id ad4691_id[] = {
	{ "ad4692", (kernel_ulong_t)&ad4691_chips[AD4691_ID_AD4692] },
	{ "ad4691", (kernel_ulong_t)&ad4691_chips[AD4691_ID_AD4691] },
	{ "ad4694", (kernel_ulong_t)&ad4691_chips[AD4691_ID_AD4694] },
	{ "ad4693", (kernel_ulong_t)&ad4691_chips[AD4691_ID_AD4693] },
	{}
};
MODULE_DEVICE_TABLE(spi, ad4691_id);

static int ad4691_gpio_setup(struct ad4691_state *st)
{
	struct device *dev = &st->spi->dev;
	struct gpio_desc *reset;

	reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset),
				     "Failed to get reset GPIO\n");

	/* Reset delay required. See datasheet Table 5. */
	fsleep(300);
	gpiod_set_value(reset, 0);

	return 0;
}

static int ad4691_config(struct ad4691_state *st)
{
	struct device *dev = &st->spi->dev;
	unsigned int reg_val;
	u32 mode;
	int ret;

	ret = regmap_read(st->regmap, AD4691_PRODUCT_ID_LSB_REG, &reg_val);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read product ID\n");

	if (reg_val != st->chip->product_id)
		return dev_err_probe(dev, -ENODEV,
				     "Product ID mismatch: expected 0x%02x, got 0x%02x\n",
				     st->chip->product_id, reg_val);

	ret = device_property_read_u32(dev, "adi,spi-mode", &mode);
	if (ret)
		return dev_err_probe(dev, -EINVAL, "Could not find SPI mode\n");

	if (mode > AD4691_MAX_ADC_MODE)
		return dev_err_probe(dev, -EINVAL, "Invalid SPI mode(%u)\n", mode);

	st->adc_mode = mode;

	/*
	 * CNV_CLOCK_MODE and CNV_BURST_MODE require a PWM for conversion timing.
	 * MANUAL_MODE doesn't need PWM - CS is tied to CNV, so each SPI
	 * transfer automatically triggers a conversion.
	 */
	if (st->adc_mode == AD4691_CNV_CLOCK_MODE ||
	    st->adc_mode == AD4691_CNV_BURST_MODE) {
		if (device_property_present(dev, "pwms")) {
			ret = ad4691_pwm_get(st->spi, st);
			if (ret)
				return ret;
		} else {
			return dev_err_probe(dev, -ENODEV,
					     "CNV modes require 'pwms' property\n");
		}
	}

	/* Perform a state reset on the channels at start-up. */
	ret = regmap_write(st->regmap, AD4691_STATE_RESET_REG,
			   AD4691_STATE_RESET_ALL);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to write state reset\n");

	/* Clear STATUS register by reading from the STATUS register. */
	ret = regmap_read(st->regmap, AD4691_STATUS_REG, &reg_val);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to read status register\n");

	switch (st->vref) {
	case AD4691_VREF_MIN ... 2750000:
		ret = regmap_write(st->regmap, AD4691_REF_CTRL,
				       FIELD_PREP(AD4691_REF_CTRL_MASK,
						  AD4691_VREF_2P5));
		break;
	case 2750001 ... 3250000:
		ret = regmap_write(st->regmap, AD4691_REF_CTRL,
				       FIELD_PREP(AD4691_REF_CTRL_MASK,
						  AD4691_VREF_3P0));
		break;
	case 3250001 ... 3750000:
		ret = regmap_write(st->regmap, AD4691_REF_CTRL,
				       FIELD_PREP(AD4691_REF_CTRL_MASK,
						  AD4691_VREF_3P3));
		break;
	case 3750001 ... 4500000:
		ret = regmap_write(st->regmap, AD4691_REF_CTRL,
				       FIELD_PREP(AD4691_REF_CTRL_MASK,
						  AD4691_VREF_4P096));
		break;
	case 4500001 ... AD4691_VREF_MAX:
		ret = regmap_write(st->regmap, AD4691_REF_CTRL,
				       FIELD_PREP(AD4691_REF_CTRL_MASK,
						  AD4691_VREF_5P0));
		break;
	default:
		return dev_err_probe(dev, -EINVAL,
				     "Unsupported vref voltage: %d uV\n",
				     st->vref);
	}
	if (ret)
		return dev_err_probe(dev, ret, "Failed to write REF_CTRL\n");

	switch (st->adc_mode) {
	case AD4691_CNV_CLOCK_MODE:
	case AD4691_CNV_BURST_MODE:
	case AD4691_AUTONOMOUS_MODE:
	case AD4691_SPI_BURST_MODE:
		/*
		 * The adi,spi-mode DT property values 0-3 map directly to the
		 * ADC_SETUP register encoding for these four modes.
		 */
		ret = regmap_write(st->regmap, AD4691_ADC_SETUP, mode);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to write ADC_SETUP\n");

		if (st->adc_mode == AD4691_AUTONOMOUS_MODE)
			/* Configure GP0 as ADC_BUSY for trigger */
			return regmap_write(st->regmap, AD4691_GPIO_MODE1_REG,
					    AD4691_ADC_BUSY);
		else
			/* Configure GP0 as DATA_READY for trigger */
			return regmap_write(st->regmap, AD4691_GPIO_MODE1_REG,
					    AD4691_DATA_READY);
	case AD4691_MANUAL_MODE:
		/* GP0 as ADC_BUSY; conversion completion is polled via CS in MANUAL_MODE. */
		ret = regmap_write(st->regmap, AD4691_GPIO_MODE1_REG,
				   AD4691_ADC_BUSY);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to write GPIO_MODE1\n");

		return regmap_write(st->regmap, AD4691_DEVICE_SETUP,
				    AD4691_DEVICE_MANUAL);
	default:
		return -EINVAL;
	}
}

static void ad4691_setup_channels(struct iio_dev *indio_dev,
				  struct ad4691_state *st)
{
	if (st->adc_mode == AD4691_MANUAL_MODE) {
		if (st->chip->num_channels == 8)
			indio_dev->channels = ad4693_manual_channels;
		else
			indio_dev->channels = ad4691_manual_channels;
	} else {
		indio_dev->channels = st->chip->channels;
	}

	indio_dev->num_channels = st->chip->num_channels;
}

static int ad4691_setup_triggered_buffer(struct iio_dev *indio_dev,
					 struct ad4691_state *st)
{
	struct device *dev = &st->spi->dev;
	int irq, ret;

	st->trig = devm_iio_trigger_alloc(dev, "%s-dev%d",
					  indio_dev->name,
					  iio_device_id(indio_dev));
	if (!st->trig)
		return dev_err_probe(dev, -ENOMEM,
				     "Failed to allocate IIO trigger\n");

	st->trig->ops = &ad4691_trigger_ops;
	iio_trigger_set_drvdata(st->trig, st);

	ret = devm_iio_trigger_register(dev, st->trig);
	if (ret)
		return dev_err_probe(dev, ret, "IIO trigger register failed\n");

	indio_dev->trig = iio_trigger_get(st->trig);

	switch (st->adc_mode) {
	case AD4691_CNV_CLOCK_MODE:
	case AD4691_CNV_BURST_MODE:
	case AD4691_AUTONOMOUS_MODE:
	case AD4691_SPI_BURST_MODE:
		/*
		 * DATA_READY asserts at end-of-conversion (or when the
		 * accumulator fills in AUTONOMOUS_MODE). The IRQ handler stops
		 * conversions and fires the IIO trigger so the trigger handler
		 * can read and push the sample to the buffer.
		 */
		irq = fwnode_irq_get_byname(dev_fwnode(dev), "DRDY");
		if (irq <= 0)
			return dev_err_probe(dev, irq ? irq : -ENOENT,
					     "failed to get DRDY interrupt\n");

		ret = devm_request_threaded_irq(dev, irq, NULL,
						&ad4691_irq,
						IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
						indio_dev->name, indio_dev);
		if (ret)
			return dev_err_probe(dev, ret,
					     "request irq %d failed\n", irq);
		break;
	case AD4691_MANUAL_MODE:
		/*
		 * No DATA_READY signal in MANUAL_MODE; CNV is tied to CS so
		 * conversions start with each SPI transfer. Use an hrtimer to
		 * schedule periodic reads.
		 */
		hrtimer_setup(&st->sampling_timer, ad4691_sampling_timer_handler,
			      CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		st->sampling_period = ns_to_ktime(DIV_ROUND_CLOSEST_ULL(
			NSEC_PER_SEC,
			AD4691_MANUAL_MODE_STD_FREQ(st->chip->num_channels,
						    st->spi->max_speed_hz)));
		break;
	default:
		return -EINVAL;
	}

	return devm_iio_triggered_buffer_setup(dev, indio_dev,
					       &iio_pollfunc_store_time,
					       &ad4691_trigger_handler,
					       &ad4691_buffer_setup_ops);
}

static int ad4691_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev;
	struct ad4691_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	mutex_init(&st->lock);

	st->spi = spi;
	spi_set_drvdata(spi, indio_dev);

	st->regmap = devm_regmap_init(dev, NULL, st, &ad4691_regmap_config);
	if (IS_ERR(st->regmap))
		return dev_err_probe(dev, PTR_ERR(st->regmap),
				     "Failed to initialize regmap\n");

	st->chip = spi_get_device_match_data(spi);
	if (!st->chip) {
		st->chip = (void *)spi_get_device_id(spi)->driver_data;
		if (!st->chip)
			return dev_err_probe(dev, -ENODEV,
					     "Could not find chip info data\n");
	}

	ret = ad4691_regulators_get(st);
	if (ret)
		return ret;

	ret = ad4691_gpio_setup(st);
	if (ret)
		return ret;

	ret = ad4691_config(st);
	if (ret)
		return ret;

	indio_dev->name = st->chip->name;
	indio_dev->info = &ad4691_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ad4691_setup_channels(indio_dev, st);

	ret = ad4691_setup_triggered_buffer(indio_dev, st);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id ad4691_of_match[] = {
	{ .compatible = "adi,ad4692", .data = &ad4691_chips[AD4691_ID_AD4692] },
	{ .compatible = "adi,ad4691", .data = &ad4691_chips[AD4691_ID_AD4691] },
	{ .compatible = "adi,ad4694", .data = &ad4691_chips[AD4691_ID_AD4694] },
	{ .compatible = "adi,ad4693", .data = &ad4691_chips[AD4691_ID_AD4693] },
	{},
};
MODULE_DEVICE_TABLE(of, ad4691_of_match);

static struct spi_driver ad4691_driver = {
	.driver = {
		.name = "ad4691",
		.of_match_table = ad4691_of_match,
	},
	.probe = ad4691_probe,
	.id_table = ad4691_id,
};
module_spi_driver(ad4691_driver);

MODULE_AUTHOR("Radu Sabau <radu.sabau@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD4691 Family ADC Driver");
MODULE_LICENSE("GPL");
