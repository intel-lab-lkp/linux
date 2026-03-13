// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024-2026 Analog Devices, Inc.
 * Author: Radu Sabau <radu.sabau@analog.com>
 */
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/reset.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
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

#define AD4691_VREF_uV_MIN			2400000
#define AD4691_VREF_uV_MAX			5250000

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
#define AD4691_DEVICE_REGISTER			0x10
#define AD4691_AUTONOMOUS_MODE_VAL		0x02

#define AD4691_NOOP				0x00
#define AD4691_ADC_CHAN(ch)			((0x10 + (ch)) << 3)

#define AD4691_STATUS_REG			0x014
#define AD4691_CLAMP_STATUS1_REG		0x01A
#define AD4691_CLAMP_STATUS2_REG		0x01B
#define AD4691_DEVICE_SETUP			0x020
#define AD4691_REF_CTRL				0x021
#define AD4691_OSC_FREQ_REG			0x023
#define AD4691_STD_SEQ_CONFIG			0x025
#define AD4691_SPARE_CONTROL			0x02A

#define AD4691_OSC_EN_REG			0x180
#define AD4691_STATE_RESET_REG			0x181
#define AD4691_ADC_SETUP			0x182
#define AD4691_ACC_MASK1_REG			0x184
#define AD4691_ACC_MASK2_REG			0x185
#define AD4691_ACC_COUNT_LIMIT(n)		(0x186 + (n))
#define AD4691_ACC_COUNT_VAL			0x01
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

enum ad4691_adc_mode {
	AD4691_CNV_CLOCK_MODE,
	AD4691_MANUAL_MODE,
};

enum ad4691_gpio_mode {
	AD4691_ADC_BUSY   = 4,
	AD4691_DATA_READY = 6,
};

enum ad4691_ref_ctrl {
	AD4691_VREF_2P5   = 0,
	AD4691_VREF_3P0   = 1,
	AD4691_VREF_3P3   = 2,
	AD4691_VREF_4P096 = 3,
	AD4691_VREF_5P0   = 4,
};

struct ad4691_chip_info {
	const struct iio_chan_spec *channels;
	const struct iio_chan_spec *manual_channels;
	const char *name;
	unsigned int num_channels;
	unsigned int max_rate;
};

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
	AD4691_CHANNEL(0, 0, 16, 32, 8),
	AD4691_CHANNEL(1, 1, 16, 32, 8),
	AD4691_CHANNEL(2, 2, 16, 32, 8),
	AD4691_CHANNEL(3, 3, 16, 32, 8),
	AD4691_CHANNEL(4, 4, 16, 32, 8),
	AD4691_CHANNEL(5, 5, 16, 32, 8),
	AD4691_CHANNEL(6, 6, 16, 32, 8),
	AD4691_CHANNEL(7, 7, 16, 32, 8),
	AD4691_CHANNEL(8, 8, 16, 32, 8),
	AD4691_CHANNEL(9, 9, 16, 32, 8),
	AD4691_CHANNEL(10, 10, 16, 32, 8),
	AD4691_CHANNEL(11, 11, 16, 32, 8),
	AD4691_CHANNEL(12, 12, 16, 32, 8),
	AD4691_CHANNEL(13, 13, 16, 32, 8),
	AD4691_CHANNEL(14, 14, 16, 32, 8),
	AD4691_CHANNEL(15, 15, 16, 32, 8)
};

static const struct iio_chan_spec ad4693_manual_channels[] = {
	AD4691_CHANNEL(0, 0, 16, 32, 8),
	AD4691_CHANNEL(1, 1, 16, 32, 8),
	AD4691_CHANNEL(2, 2, 16, 32, 8),
	AD4691_CHANNEL(3, 3, 16, 32, 8),
	AD4691_CHANNEL(4, 4, 16, 32, 8),
	AD4691_CHANNEL(5, 5, 16, 32, 8),
	AD4691_CHANNEL(6, 6, 16, 32, 8),
	AD4691_CHANNEL(7, 7, 16, 32, 8)
};

static const struct ad4691_chip_info ad4691_ad4691 = {
	.channels = ad4691_channels,
	.manual_channels = ad4691_manual_channels,
	.name = "ad4691",
	.num_channels = ARRAY_SIZE(ad4691_channels),
	.max_rate = 500 * HZ_PER_KHZ,
};

static const struct ad4691_chip_info ad4691_ad4692 = {
	.channels = ad4691_channels,
	.manual_channels = ad4691_manual_channels,
	.name = "ad4692",
	.num_channels = ARRAY_SIZE(ad4691_channels),
	.max_rate = 1 * HZ_PER_MHZ,
};

static const struct ad4691_chip_info ad4691_ad4693 = {
	.channels = ad4693_channels,
	.manual_channels = ad4693_manual_channels,
	.name = "ad4693",
	.num_channels = ARRAY_SIZE(ad4693_channels),
	.max_rate = 500 * HZ_PER_KHZ,
};

static const struct ad4691_chip_info ad4691_ad4694 = {
	.channels = ad4693_channels,
	.manual_channels = ad4693_manual_channels,
	.name = "ad4694",
	.num_channels = ARRAY_SIZE(ad4693_channels),
	.max_rate = 1 * HZ_PER_MHZ,
};

struct ad4691_state {
	const struct ad4691_chip_info	*chip;
	struct regmap			*regmap;

	unsigned long			ref_clk_rate;
	struct pwm_device		*conv_trigger;

	struct iio_trigger		*trig;

	enum ad4691_adc_mode		adc_mode;

	int				vref_uV;
	u64				cnv_period;
	ktime_t				sampling_period;
	/*
	 * Synchronize access to members of the driver state, and ensure
	 * atomicity of consecutive SPI operations.
	 */
	struct mutex			lock;

	/* hrtimer for MANUAL_MODE triggered buffer (non-offload) */
	struct hrtimer			sampling_timer;

	/*
	 * DMA (thus cache coherency maintenance) may require the
	 * transfer buffers to live in their own cache lines.
	 */
	unsigned char rx_data[ALIGN(3, sizeof(s64)) + sizeof(s64)]	__aligned(IIO_DMA_MINALIGN);
	unsigned char tx_data[ALIGN(3, sizeof(s64)) + sizeof(s64)];
	/* Scan buffer: one slot per channel (u32) plus timestamp */
	struct {
		u32 vals[16];
		s64 ts __aligned(8);
	} scan __aligned(IIO_DMA_MINALIGN);
};

static void ad4691_disable_pwm(void *data)
{
	struct pwm_device *pwm = data;
	struct pwm_state state;

	pwm_get_state(pwm, &state);
	state.enabled = false;
	pwm_apply_might_sleep(pwm, &state);
}

static int ad4691_regulator_get(struct ad4691_state *st)
{
	struct device *dev = regmap_get_device(st->regmap);
	int ret;

	ret = devm_regulator_get_enable(dev, "vio");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get and enable VIO\n");

	st->vref_uV = devm_regulator_get_enable_read_voltage(dev, "vref");
	if (st->vref_uV == -ENODEV)
		st->vref_uV = devm_regulator_get_enable_read_voltage(dev, "vrefin");
	if (st->vref_uV < 0)
		return dev_err_probe(dev, st->vref_uV,
				     "Failed to get reference supply\n");

	if (st->vref_uV < AD4691_VREF_uV_MIN || st->vref_uV > AD4691_VREF_uV_MAX)
		return dev_err_probe(dev, -EINVAL, "vref(%d) must be under [%u %u]\n",
				     st->vref_uV, AD4691_VREF_uV_MIN, AD4691_VREF_uV_MAX);

	return 0;
}

static int ad4691_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct ad4691_state *st = context;
	struct spi_device *spi = to_spi_device(regmap_get_device(st->regmap));
	u8 tx[2], rx[4];
	int ret;

	put_unaligned_be16(0x8000 | reg, tx);

	switch (reg) {
	case 0 ... AD4691_OSC_FREQ_REG:
	case AD4691_SPARE_CONTROL ... AD4691_ACC_SAT_OVR_REG(15):
		ret = spi_write_then_read(spi, tx, 2, rx, 1);
		if (ret)
			return ret;
		*val = rx[0];
		return 0;
	case AD4691_STD_SEQ_CONFIG:
	case AD4691_AVG_IN(0) ... AD4691_AVG_IN(15):
		ret = spi_write_then_read(spi, tx, 2, rx, 2);
		if (ret)
			return ret;
		*val = get_unaligned_be16(rx);
		return 0;
	case AD4691_AVG_STS_IN(0) ... AD4691_AVG_STS_IN(15):
	case AD4691_ACC_IN(0) ... AD4691_ACC_IN(15):
		ret = spi_write_then_read(spi, tx, 2, rx, 3);
		if (ret)
			return ret;
		*val = get_unaligned_be24(rx);
		return 0;
	case AD4691_ACC_STS_DATA(0) ... AD4691_ACC_STS_DATA(15):
		ret = spi_write_then_read(spi, tx, 2, rx, 4);
		if (ret)
			return ret;
		*val = get_unaligned_be32(rx);
		return 0;
	default:
		return -EINVAL;
	}
}

static int ad4691_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct ad4691_state *st = context;
	struct spi_device *spi = to_spi_device(regmap_get_device(st->regmap));
	u8 tx[4];

	put_unaligned_be16(reg, tx);

	switch (reg) {
	case 0 ... AD4691_OSC_FREQ_REG:
	case AD4691_SPARE_CONTROL ... AD4691_GPIO_MODE2_REG:
		if (val > 0xFF)
			return -EINVAL;
		tx[2] = val;
		return spi_write_then_read(spi, tx, 3, NULL, 0);
	case AD4691_STD_SEQ_CONFIG:
		if (val > 0xFFFF)
			return -EINVAL;
		put_unaligned_be16(val, &tx[2]);
		return spi_write_then_read(spi, tx, 4, NULL, 0);
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
	.cache_type = REGCACHE_MAPLE,
};

static int ad4691_transfer(struct ad4691_state *st, int command,
			   unsigned int *val)
{
	struct spi_device *spi = to_spi_device(regmap_get_device(st->regmap));
	struct spi_transfer xfer = {
		.tx_buf = st->tx_data,
		.rx_buf = st->rx_data,
		.len = 3,
	};
	int ret;

	memcpy(st->tx_data, &command, 3);

	ret = spi_sync_transfer(spi, &xfer, 1);
	if (ret)
		return ret;

	*val = get_unaligned_be24(st->rx_data);

	return 0;
}

static int ad4691_get_sampling_freq(struct ad4691_state *st)
{
	if (st->adc_mode == AD4691_MANUAL_MODE)
		return DIV_ROUND_CLOSEST(NSEC_PER_SEC,
					 ktime_to_ns(st->sampling_period));

	return DIV_ROUND_CLOSEST(NSEC_PER_SEC,
				 pwm_get_period(st->conv_trigger));
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

static int ad4691_pwm_get(struct ad4691_state *st)
{
	struct device *dev = regmap_get_device(st->regmap);
	struct clk *ref_clk;
	int ret;

	ref_clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(ref_clk))
		return dev_err_probe(dev, PTR_ERR(ref_clk),
				     "Failed to get ref clock\n");

	st->ref_clk_rate = clk_get_rate(ref_clk);

	st->conv_trigger = devm_pwm_get(dev, "cnv");
	if (IS_ERR(st->conv_trigger))
		return dev_err_probe(dev, PTR_ERR(st->conv_trigger),
				     "Failed to get cnv pwm\n");

	ret = devm_add_action_or_reset(dev, ad4691_disable_pwm,
				       st->conv_trigger);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register PWM disable action\n");

	return __ad4691_set_sampling_freq(st, st->chip->max_rate);
}

static int ad4691_set_sampling_freq(struct iio_dev *indio_dev, unsigned int freq)
{
	struct ad4691_state *st = iio_priv(indio_dev);

	IIO_DEV_ACQUIRE_DIRECT_MODE(indio_dev, claim);

	if (IIO_DEV_ACQUIRE_FAILED(claim))
		return -EBUSY;

	guard(mutex)(&st->lock);

	if (st->adc_mode == AD4691_MANUAL_MODE) {
		if (!freq || freq > st->chip->max_rate)
			return -ERANGE;

		st->sampling_period = ns_to_ktime(DIV_ROUND_CLOSEST(NSEC_PER_SEC,
									 freq));
		return 0;
	}

	if (!st->conv_trigger)
		return -ENODEV;

	if (!freq || freq > st->chip->max_rate)
		return -ERANGE;

	return __ad4691_set_sampling_freq(st, freq);
}

static int ad4691_sampling_enable(struct ad4691_state *st, bool enable)
{
	struct pwm_state conv_state = { };

	conv_state.period = st->cnv_period;
	conv_state.duty_cycle = AD4691_CNV_DUTY_CYCLE_NS;
	conv_state.polarity = PWM_POLARITY_NORMAL;
	conv_state.enabled = enable;

	return pwm_apply_might_sleep(st->conv_trigger, &conv_state);
}

static int ad4691_single_shot_read(struct iio_dev *indio_dev,
				   struct iio_chan_spec const *chan, int *val)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	u16 mask = ~BIT(chan->channel);
	u32 acc_mask[2] = { mask & 0xFF, mask >> 8 };
	unsigned int reg_val;
	int ret;

	/*
	 * Always use AUTONOMOUS mode for single-shot reads, regardless
	 * of the buffer mode (CNV_CLOCK or MANUAL). The chip is kept
	 * in AUTONOMOUS mode during idle; enter_conversion_mode() and
	 * exit_conversion_mode() handle the switch for buffer operation.
	 */
	ret = regmap_write(st->regmap, AD4691_STATE_RESET_REG,
			   AD4691_STATE_RESET_ALL);
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, AD4691_STD_SEQ_CONFIG,
			   BIT(chan->channel));
	if (ret)
		return ret;

	ret = regmap_bulk_write(st->regmap, AD4691_ACC_MASK1_REG, acc_mask, 2);
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, AD4691_OSC_EN_REG, 1);
	if (ret)
		return ret;

	/*
	 * Wait for conversion to complete using a timed delay.
	 * A single read needs 2 internal oscillator periods.
	 * OSC_FREQ_REG is never modified by the driver, so the
	 * oscillator runs at reset-default speed. Use chip->max_rate
	 * as a conservative proxy: it is always <= the OSC frequency,
	 * so the computed delay is >= the actual conversion time.
	 */
	unsigned long conv_us = DIV_ROUND_UP(2 * USEC_PER_SEC,
					     st->chip->max_rate);
	fsleep(conv_us);

	ret = regmap_write(st->regmap, AD4691_OSC_EN_REG, 0);
	if (ret)
		return ret;

	ret = regmap_read(st->regmap, AD4691_AVG_IN(chan->channel), &reg_val);
	if (ret)
		return ret;

	*val = reg_val;
	regmap_write(st->regmap, AD4691_STATE_RESET_REG, AD4691_STATE_RESET_ALL);

	return IIO_VAL_INT;
}

static int ad4691_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan, int *val,
			   int *val2, long info)
{
	struct ad4691_state *st = iio_priv(indio_dev);

	switch (info) {
	case IIO_CHAN_INFO_RAW: {
		IIO_DEV_ACQUIRE_DIRECT_MODE(indio_dev, claim);

		if (IIO_DEV_ACQUIRE_FAILED(claim))
			return -EBUSY;

		return ad4691_single_shot_read(indio_dev, chan, val);
	}
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = ad4691_get_sampling_freq(st);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = st->vref_uV / 1000;
		*val2 = chan->scan_type.realbits;
		return IIO_VAL_FRACTIONAL_LOG2;
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

	guard(mutex)(&st->lock);

	if (readval)
		return regmap_read(st->regmap, reg, readval);

	return regmap_write(st->regmap, reg, writeval);
}

/*
 * ad4691_enter_conversion_mode - Switch the chip to its buffer conversion mode.
 *
 * Configures the ADC hardware registers for the mode selected at probe
 * (CNV_CLOCK or MANUAL). Called from buffer postenable before starting
 * sampling. The chip is in AUTONOMOUS mode during idle (for read_raw).
 */
static int ad4691_enter_conversion_mode(struct ad4691_state *st)
{
	int ret;

	if (st->adc_mode == AD4691_MANUAL_MODE)
		return regmap_write(st->regmap, AD4691_DEVICE_SETUP,
				    AD4691_DEVICE_MANUAL);

	ret = regmap_write(st->regmap, AD4691_ADC_SETUP, AD4691_CNV_CLOCK_MODE);
	if (ret)
		return ret;

	return regmap_write(st->regmap, AD4691_GPIO_MODE1_REG,
			    AD4691_DATA_READY);
}

/*
 * ad4691_exit_conversion_mode - Return the chip to AUTONOMOUS mode.
 *
 * Called from buffer postdisable/predisable to restore the chip to the
 * idle state used by read_raw. Clears the sequencer and resets state.
 */
static int ad4691_exit_conversion_mode(struct ad4691_state *st)
{
	int ret;

	if (st->adc_mode == AD4691_MANUAL_MODE) {
		ret = regmap_write(st->regmap, AD4691_DEVICE_SETUP,
				   AD4691_DEVICE_REGISTER);
		if (ret)
			return ret;
	}

	ret = regmap_write(st->regmap, AD4691_ADC_SETUP, AD4691_AUTONOMOUS_MODE_VAL);
	if (ret)
		return ret;

	/* Restore GP0 to ADC_BUSY for AUTONOMOUS idle (enter set it to DATA_READY) */
	ret = regmap_write(st->regmap, AD4691_GPIO_MODE1_REG, AD4691_ADC_BUSY);
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, AD4691_STD_SEQ_CONFIG,
			   AD4691_SEQ_ALL_CHANNELS_OFF);
	if (ret)
		return ret;

	return regmap_write(st->regmap, AD4691_STATE_RESET_REG,
			    AD4691_STATE_RESET_ALL);
}

static int ad4691_buffer_postenable(struct iio_dev *indio_dev)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(st->regmap);
	struct spi_device *spi = to_spi_device(dev);
	u16 mask = ~(*indio_dev->active_scan_mask);
	u32 acc_mask[2] = { mask & 0xFF, mask >> 8 };
	int n_active = hweight_long(*indio_dev->active_scan_mask);
	unsigned int bit;
	int ret;

	ret = ad4691_enter_conversion_mode(st);
	if (ret)
		return ret;

	if (st->adc_mode == AD4691_MANUAL_MODE) {
		u64 min_period_ns;

		/* N+1 transfers needed for N channels, with 50% overhead */
		min_period_ns = div64_u64((u64)(n_active + 1) * AD4691_BITS_PER_XFER *
					  NSEC_PER_SEC * 3,
					  spi->max_speed_hz * 2);

		if (ktime_to_ns(st->sampling_period) < min_period_ns) {
			dev_err(dev,
				"Sampling period %lld ns too short for %d channels. Min: %llu ns\n",
				ktime_to_ns(st->sampling_period), n_active,
				min_period_ns);
			return -EINVAL;
		}

		hrtimer_start(&st->sampling_timer, st->sampling_period,
			      HRTIMER_MODE_REL);
		return 0;
	}

	/* CNV_CLOCK_MODE: configure sequencer and start PWM */
	ret = regmap_write(st->regmap, AD4691_STATE_RESET_REG,
			   AD4691_STATE_RESET_ALL);
	if (ret)
		return ret;

	ret = regmap_bulk_write(st->regmap, AD4691_ACC_MASK1_REG, acc_mask, 2);
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, AD4691_STD_SEQ_CONFIG,
			   *indio_dev->active_scan_mask);
	if (ret)
		return ret;

	iio_for_each_active_channel(indio_dev, bit) {
		ret = regmap_write(st->regmap, AD4691_ACC_COUNT_LIMIT(bit),
				   AD4691_ACC_COUNT_VAL);
		if (ret)
			return ret;
	}

	return ad4691_sampling_enable(st, true);
}

static int ad4691_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct ad4691_state *st = iio_priv(indio_dev);

	if (st->adc_mode == AD4691_MANUAL_MODE)
		hrtimer_cancel_wait_running(&st->sampling_timer);
	else
		ad4691_sampling_enable(st, false);

	return ad4691_exit_conversion_mode(st);
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
	 */
	ad4691_sampling_enable(st, false);
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
	unsigned int val, i;
	int ret;

	guard(mutex)(&st->lock);

	if (st->adc_mode == AD4691_MANUAL_MODE) {
		unsigned int prev_val;
		int prev_chan = -1;

		/*
		 * MANUAL_MODE with CNV tied to CS: each transfer triggers a
		 * conversion AND returns the previous conversion's result.
		 * First transfer returns garbage, so we do N+1 transfers for
		 * N channels. Collect all results into scan.vals[], then push
		 * the complete scan once.
		 */
		iio_for_each_active_channel(indio_dev, i) {
			ret = ad4691_transfer(st, AD4691_ADC_CHAN(i), &val);
			if (ret)
				goto done;

			if (prev_chan >= 0)
				st->scan.vals[prev_chan] = prev_val;
			prev_val = val;
			prev_chan = i;
		}

		/* Final NOOP transfer to retrieve last channel's result */
		ret = ad4691_transfer(st, AD4691_NOOP, &val);
		if (ret)
			goto done;

		st->scan.vals[prev_chan] = val;
	} else {
		iio_for_each_active_channel(indio_dev, i) {
			ret = regmap_read(st->regmap, AD4691_AVG_IN(i), &val);
			if (ret)
				goto done;

			st->scan.vals[i] = val;
		}

		regmap_write(st->regmap, AD4691_STATE_RESET_REG, AD4691_STATE_RESET_ALL);

		/* Restart conversions for the next trigger cycle. */
		ad4691_sampling_enable(st, true);
	}

	iio_push_to_buffers_with_ts(indio_dev, &st->scan, sizeof(st->scan),
				    pf->timestamp);

done:
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

static const struct iio_info ad4691_info = {
	.read_raw = &ad4691_read_raw,
	.write_raw = &ad4691_write_raw,
	.debugfs_reg_access = &ad4691_reg_access,
};

static int ad4691_reset(struct ad4691_state *st)
{
	struct device *dev = regmap_get_device(st->regmap);
	struct reset_control *rst;

	rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst),
				     "Failed to get reset\n");

	if (!rst)
		return 0;

	reset_control_assert(rst);
	/* Reset delay required. See datasheet Table 5. */
	fsleep(300);
	reset_control_deassert(rst);

	return 0;
}

static int ad4691_config(struct ad4691_state *st)
{
	struct device *dev = regmap_get_device(st->regmap);
	struct spi_device *spi = to_spi_device(dev);
	enum ad4691_ref_ctrl ref_val;
	unsigned int reg_val;
	int ret;

	/*
	 * Determine buffer conversion mode from DT: if a PWM is provided it
	 * drives the CNV pin (CNV_CLOCK_MODE); otherwise CNV is tied to CS
	 * and each SPI transfer triggers a conversion (MANUAL_MODE).
	 * Both modes idle in AUTONOMOUS mode so that read_raw can use the
	 * internal oscillator without disturbing the hardware configuration.
	 */
	if (device_property_present(dev, "pwms")) {
		st->adc_mode = AD4691_CNV_CLOCK_MODE;
		ret = ad4691_pwm_get(st);
		if (ret)
			return ret;
	} else {
		st->adc_mode = AD4691_MANUAL_MODE;
		st->sampling_period = ns_to_ktime(DIV_ROUND_CLOSEST_ULL(NSEC_PER_SEC,
			AD4691_MANUAL_MODE_STD_FREQ(st->chip->num_channels,
						    spi->max_speed_hz)));
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

	switch (st->vref_uV) {
	case AD4691_VREF_uV_MIN ... 2750000:
		ref_val = AD4691_VREF_2P5;
		break;
	case 2750001 ... 3250000:
		ref_val = AD4691_VREF_3P0;
		break;
	case 3250001 ... 3750000:
		ref_val = AD4691_VREF_3P3;
		break;
	case 3750001 ... 4500000:
		ref_val = AD4691_VREF_4P096;
		break;
	case 4500001 ... AD4691_VREF_uV_MAX:
		ref_val = AD4691_VREF_5P0;
		break;
	default:
		return dev_err_probe(dev, -EINVAL,
				     "Unsupported vref voltage: %d uV\n",
				     st->vref_uV);
	}

	ret = regmap_write(st->regmap, AD4691_REF_CTRL,
			   FIELD_PREP(AD4691_REF_CTRL_MASK, ref_val));
	if (ret)
		return dev_err_probe(dev, ret, "Failed to write REF_CTRL\n");

	/* Both CNV_CLOCK and MANUAL devices start in AUTONOMOUS mode. */
	ret = regmap_write(st->regmap, AD4691_ADC_SETUP, AD4691_AUTONOMOUS_MODE_VAL);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to write ADC_SETUP\n");

	return regmap_write(st->regmap, AD4691_GPIO_MODE1_REG, AD4691_ADC_BUSY);
}

static int ad4691_setup_triggered_buffer(struct iio_dev *indio_dev,
					 struct ad4691_state *st)
{
	struct device *dev = regmap_get_device(st->regmap);
	struct spi_device *spi = to_spi_device(dev);
	int irq, ret;

	st->trig = devm_iio_trigger_alloc(dev, "%s-dev%d",
					  indio_dev->name,
					  iio_device_id(indio_dev));
	if (!st->trig)
		return -ENOMEM;

	st->trig->ops = &ad4691_trigger_ops;
	iio_trigger_set_drvdata(st->trig, st);

	ret = devm_iio_trigger_register(dev, st->trig);
	if (ret)
		return dev_err_probe(dev, ret, "IIO trigger register failed\n");

	indio_dev->trig = iio_trigger_get(st->trig);

	if (st->adc_mode == AD4691_MANUAL_MODE) {
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
						    spi->max_speed_hz)));
	} else {
		/*
		 * DATA_READY asserts at end-of-conversion. The IRQ handler
		 * stops conversions and fires the IIO trigger so the trigger
		 * handler can read and push the sample to the buffer.
		 */
		irq = fwnode_irq_get(dev_fwnode(dev), 0);
		if (irq < 0)
			return dev_err_probe(dev, irq,
					     "failed to get DATA_READY interrupt\n");

		ret = devm_request_threaded_irq(dev, irq, NULL,
						&ad4691_irq,
						IRQF_ONESHOT,
						indio_dev->name, indio_dev);
		if (ret)
			return ret;
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
	ret = devm_mutex_init(dev, &st->lock);
	if (ret)
		return ret;

	st->regmap = devm_regmap_init(dev, NULL, st, &ad4691_regmap_config);
	if (IS_ERR(st->regmap))
		return dev_err_probe(dev, PTR_ERR(st->regmap),
				     "Failed to initialize regmap\n");

	st->chip = spi_get_device_match_data(spi);

	ret = ad4691_regulator_get(st);
	if (ret)
		return ret;

	ret = ad4691_reset(st);
	if (ret)
		return ret;

	ret = ad4691_config(st);
	if (ret)
		return ret;

	indio_dev->name = st->chip->name;
	indio_dev->info = &ad4691_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	indio_dev->channels = (st->adc_mode == AD4691_MANUAL_MODE) ?
		st->chip->manual_channels : st->chip->channels;
	indio_dev->num_channels = st->chip->num_channels;

	ret = ad4691_setup_triggered_buffer(indio_dev, st);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id ad4691_of_match[] = {
	{ .compatible = "adi,ad4691", .data = &ad4691_ad4691 },
	{ .compatible = "adi,ad4692", .data = &ad4691_ad4692 },
	{ .compatible = "adi,ad4693", .data = &ad4691_ad4693 },
	{ .compatible = "adi,ad4694", .data = &ad4691_ad4694 },
	{ }
};
MODULE_DEVICE_TABLE(of, ad4691_of_match);

static const struct spi_device_id ad4691_id[] = {
	{ "ad4691", (kernel_ulong_t)&ad4691_ad4691 },
	{ "ad4692", (kernel_ulong_t)&ad4691_ad4692 },
	{ "ad4693", (kernel_ulong_t)&ad4691_ad4693 },
	{ "ad4694", (kernel_ulong_t)&ad4691_ad4694 },
	{ }
};
MODULE_DEVICE_TABLE(spi, ad4691_id);

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
