// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2024-2026 Analog Devices, Inc.
 * Author: Radu Sabau <radu.sabau@analog.com>
 */
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/property.h>
#include <linux/pwm.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>
#include <linux/units.h>
#include <linux/unaligned.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

#define AD4691_VREF_uV_MIN			2400000
#define AD4691_VREF_uV_MAX			5250000
#define AD4691_VREF_2P5_uV_MAX			2750000
#define AD4691_VREF_3P0_uV_MAX			3250000
#define AD4691_VREF_3P3_uV_MAX			3750000
#define AD4691_VREF_4P096_uV_MAX		4500000

#define AD4691_CNV_DUTY_CYCLE_NS		380

#define AD4691_SPI_CONFIG_A_REG			0x000
#define AD4691_SW_RESET				(BIT(7) | BIT(0))

#define AD4691_STATUS_REG			0x014
#define AD4691_CLAMP_STATUS1_REG		0x01A
#define AD4691_CLAMP_STATUS2_REG		0x01B
#define AD4691_DEVICE_SETUP			0x020
#define AD4691_MANUAL_MODE			BIT(2)
#define AD4691_LDO_EN				BIT(4)
#define AD4691_REF_CTRL				0x021
#define AD4691_REF_CTRL_MASK			GENMASK(4, 2)
#define AD4691_REFBUF_EN			BIT(0)
#define AD4691_OSC_FREQ_REG			0x023
#define AD4691_OSC_FREQ_MASK			GENMASK(3, 0)
#define AD4691_STD_SEQ_CONFIG			0x025
#define AD4691_SEQ_ALL_CHANNELS_OFF		0x00
#define AD4691_SPARE_CONTROL			0x02A

#define AD4691_NOOP				0x00
#define AD4691_ADC_CHAN(ch)			((0x10 + (ch)) << 3)

#define AD4691_OSC_EN_REG			0x180
#define AD4691_STATE_RESET_REG			0x181
#define AD4691_STATE_RESET_ALL			0x01
#define AD4691_ADC_SETUP			0x182
#define AD4691_CNV_BURST_MODE			0x01
#define AD4691_AUTONOMOUS_MODE			0x02
/*
 * ACC_MASK_REG covers both mask bytes via ADDR_DESCENDING SPI: writing a
 * 16-bit BE value to 0x185 auto-decrements to 0x184 for the second byte.
 */
#define AD4691_ACC_MASK_REG			0x185
#define AD4691_ACC_COUNT_LIMIT(n)		(0x186 + (n))
#define AD4691_ACC_COUNT_VAL			0x1
#define AD4691_GPIO_MODE1_REG			0x196
#define AD4691_GPIO_MODE2_REG			0x197
#define AD4691_GP_MODE_MASK			GENMASK(3, 0)
#define AD4691_GP_MODE_DATA_READY		0x06
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

enum ad4691_ref_ctrl {
	AD4691_VREF_2P5   = 0,
	AD4691_VREF_3P0   = 1,
	AD4691_VREF_3P3   = 2,
	AD4691_VREF_4P096 = 3,
	AD4691_VREF_5P0   = 4,
};

struct ad4691_chip_info {
	const struct iio_chan_spec *channels;
	const char *name;
	unsigned int num_channels;
	unsigned int max_rate;
};

#define AD4691_CHANNEL(ch)						\
	{								\
		.type = IIO_VOLTAGE,					\
		.indexed = 1,						\
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW)		\
				    | BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
		.info_mask_separate_available =				\
				      BIT(IIO_CHAN_INFO_SAMP_FREQ),	\
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SCALE),	\
		.channel = ch,						\
		.scan_index = ch,					\
		.scan_type = {						\
			.sign = 'u',					\
			.realbits = 16,					\
			.storagebits = 16,				\
			.shift = 0,					\
		},							\
	}

static const struct iio_chan_spec ad4691_channels[] = {
	AD4691_CHANNEL(0),
	AD4691_CHANNEL(1),
	AD4691_CHANNEL(2),
	AD4691_CHANNEL(3),
	AD4691_CHANNEL(4),
	AD4691_CHANNEL(5),
	AD4691_CHANNEL(6),
	AD4691_CHANNEL(7),
	AD4691_CHANNEL(8),
	AD4691_CHANNEL(9),
	AD4691_CHANNEL(10),
	AD4691_CHANNEL(11),
	AD4691_CHANNEL(12),
	AD4691_CHANNEL(13),
	AD4691_CHANNEL(14),
	AD4691_CHANNEL(15),
	IIO_CHAN_SOFT_TIMESTAMP(16),
};

static const struct iio_chan_spec ad4693_channels[] = {
	AD4691_CHANNEL(0),
	AD4691_CHANNEL(1),
	AD4691_CHANNEL(2),
	AD4691_CHANNEL(3),
	AD4691_CHANNEL(4),
	AD4691_CHANNEL(5),
	AD4691_CHANNEL(6),
	AD4691_CHANNEL(7),
	IIO_CHAN_SOFT_TIMESTAMP(16),
};

/*
 * Internal oscillator frequency table. Index is the OSC_FREQ_REG[3:0] value.
 * Index 0 (1 MHz) is only valid for AD4692/AD4694; AD4691/AD4693 support
 * up to 500 kHz and use index 1 as their highest valid rate.
 */
static const unsigned int ad4691_osc_freqs[] = {
	1000000,	/* 0x0: 1 MHz */
	500000,		/* 0x1: 500 kHz */
	400000,		/* 0x2: 400 kHz */
	250000,		/* 0x3: 250 kHz */
	200000,		/* 0x4: 200 kHz */
	167000,		/* 0x5: 167 kHz */
	133000,		/* 0x6: 133 kHz */
	125000,		/* 0x7: 125 kHz */
	100000,		/* 0x8: 100 kHz */
	50000,		/* 0x9: 50 kHz */
	25000,		/* 0xA: 25 kHz */
	12500,		/* 0xB: 12.5 kHz */
	10000,		/* 0xC: 10 kHz */
	5000,		/* 0xD: 5 kHz */
	2500,		/* 0xE: 2.5 kHz */
	1250,		/* 0xF: 1.25 kHz */
};

static const struct ad4691_chip_info ad4691_chip_info = {
	.channels = ad4691_channels,
	.name = "ad4691",
	.num_channels = ARRAY_SIZE(ad4691_channels),
	.max_rate = 500 * HZ_PER_KHZ,
};

static const struct ad4691_chip_info ad4692_chip_info = {
	.channels = ad4691_channels,
	.name = "ad4692",
	.num_channels = ARRAY_SIZE(ad4691_channels),
	.max_rate = 1 * HZ_PER_MHZ,
};

static const struct ad4691_chip_info ad4693_chip_info = {
	.channels = ad4693_channels,
	.name = "ad4693",
	.num_channels = ARRAY_SIZE(ad4693_channels),
	.max_rate = 500 * HZ_PER_KHZ,
};

static const struct ad4691_chip_info ad4694_chip_info = {
	.channels = ad4693_channels,
	.name = "ad4694",
	.num_channels = ARRAY_SIZE(ad4693_channels),
	.max_rate = 1 * HZ_PER_MHZ,
};

struct ad4691_state {
	const struct ad4691_chip_info	*info;
	struct regmap			*regmap;

	struct pwm_device		*conv_trigger;
	struct iio_trigger		*trig;
	int				irq;

	bool				manual_mode;

	int				vref_uV;
	bool				refbuf_en;
	bool				ldo_en;
	u32				cnv_period_ns;
	/*
	 * Synchronize access to members of the driver state, and ensure
	 * atomicity of consecutive SPI operations.
	 */
	struct mutex			lock;
	/*
	 * Per-buffer-enabl ree lifetimesources:
	 * Manual Mode - a pre-built SPI message that clocks out N+1
	 *		 transfers in one go.
	 * CNV Burst Mode - a pre-built SPI message that clocks out 2*N
	 *		    transfers in one go.
	 */
	void				*scan_devm_group;
	struct spi_message		scan_msg;
	struct spi_transfer		*scan_xfers;
	__be16				*scan_tx;
	__be16				*scan_rx;
	/* Scan buffer: one slot per channel (u16) plus timestamp */
	struct {
		u16 vals[16];
		s64 ts __aligned(8);
	} scan __aligned(IIO_DMA_MINALIGN);
};

/*
 * Configure the given GP pin (0-3) as DATA_READY output.
 * GP0/GP1 → GPIO_MODE1_REG, GP2/GP3 → GPIO_MODE2_REG.
 * Even pins occupy bits [3:0], odd pins bits [7:4].
 */
static int ad4691_gpio_setup(struct ad4691_state *st, unsigned int gp_num)
{
	unsigned int shift = 4 * (gp_num % 2);

	return regmap_update_bits(st->regmap,
				  AD4691_GPIO_MODE1_REG + gp_num / 2,
				  AD4691_GP_MODE_MASK << shift,
				  AD4691_GP_MODE_DATA_READY << shift);
}

static void ad4691_disable_pwm(void *data)
{
	struct pwm_state state = { .enabled = false };

	pwm_apply_might_sleep(data, &state);
}

static int ad4691_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct spi_device *spi = context;
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
	struct spi_device *spi = context;
	u8 tx[4];

	put_unaligned_be16(reg, tx);

	switch (reg) {
	case 0 ... AD4691_OSC_FREQ_REG:
	case AD4691_SPARE_CONTROL ... AD4691_ACC_MASK_REG - 1:
	case AD4691_ACC_MASK_REG + 1 ... AD4691_GPIO_MODE2_REG:
		if (val > 0xFF)
			return -EINVAL;
		tx[2] = val;
		return spi_write_then_read(spi, tx, 3, NULL, 0);
	case AD4691_ACC_MASK_REG:
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

static int ad4691_get_sampling_freq(struct ad4691_state *st, int *val)
{
	unsigned int reg_val;
	int ret;

	ret = regmap_read(st->regmap, AD4691_OSC_FREQ_REG, &reg_val);
	if (ret)
		return ret;

	*val = ad4691_osc_freqs[FIELD_GET(AD4691_OSC_FREQ_MASK, reg_val)];
	return IIO_VAL_INT;
}

static int ad4691_set_sampling_freq(struct iio_dev *indio_dev, int freq)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	unsigned int i;

	if (freq > st->info->max_rate)
		return -EINVAL;

	IIO_DEV_ACQUIRE_DIRECT_MODE(indio_dev, claim);
	if (IIO_DEV_ACQUIRE_FAILED(claim))
		return -EBUSY;

	for (i = 0; i < ARRAY_SIZE(ad4691_osc_freqs); i++) {
		if ((int)ad4691_osc_freqs[i] == freq)
			return regmap_update_bits(st->regmap, AD4691_OSC_FREQ_REG,
						  AD4691_OSC_FREQ_MASK, i);
	}

	return -EINVAL;
}

static int ad4691_read_avail(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     const int **vals, int *type,
			     int *length, long mask)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	unsigned int start;

	/* Skip frequencies that exceed this chip's maximum rate. */
	start = (ad4691_osc_freqs[0] > st->info->max_rate) ? 1 : 0;

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*vals = (const int *)&ad4691_osc_freqs[start];
		*type = IIO_VAL_INT;
		*length = ARRAY_SIZE(ad4691_osc_freqs) - start;
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static int ad4691_single_shot_read(struct iio_dev *indio_dev,
				   struct iio_chan_spec const *chan, int *val)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	unsigned int reg_val;
	int ret;

	guard(mutex)(&st->lock);

	/*
	 * Use AUTONOMOUS mode for single-shot reads.
	 */
	ret = regmap_write(st->regmap, AD4691_STATE_RESET_REG,
			   AD4691_STATE_RESET_ALL);
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, AD4691_STD_SEQ_CONFIG,
			   BIT(chan->channel));
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, AD4691_ACC_MASK_REG,
			   (u16)~BIT(chan->channel));
	if (ret)
		return ret;

	ret = regmap_read(st->regmap, AD4691_OSC_FREQ_REG, &reg_val);
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, AD4691_OSC_EN_REG, 1);
	if (ret)
		return ret;

	/*
	 * Wait for at least 2 internal oscillator periods for the
	 * conversion to complete.
	 */
	fsleep(DIV_ROUND_UP(2 * USEC_PER_SEC,
		ad4691_osc_freqs[FIELD_GET(AD4691_OSC_FREQ_MASK, reg_val)]));

	ret = regmap_write(st->regmap, AD4691_OSC_EN_REG, 0);
	if (ret)
		return ret;

	ret = regmap_read(st->regmap, AD4691_AVG_IN(chan->channel), &reg_val);
	if (ret)
		return ret;

	*val = reg_val;

	ret = regmap_write(st->regmap, AD4691_STATE_RESET_REG, AD4691_STATE_RESET_ALL);
	if (ret)
		return ret;

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
		return ad4691_get_sampling_freq(st, val);
	case IIO_CHAN_INFO_SCALE:
		*val = st->vref_uV / (MICRO / MILLI);
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

static int ad4691_set_pwm_freq(struct ad4691_state *st, int freq)
{
	if (!freq)
		return -EINVAL;

	st->cnv_period_ns = DIV_ROUND_CLOSEST(NSEC_PER_SEC, freq);
	return 0;
}

static int ad4691_sampling_enable(struct ad4691_state *st, bool enable)
{
	struct pwm_state conv_state = { };

	conv_state.period = st->cnv_period_ns;
	conv_state.duty_cycle = AD4691_CNV_DUTY_CYCLE_NS;
	conv_state.polarity = PWM_POLARITY_NORMAL;
	conv_state.enabled = enable;

	return pwm_apply_might_sleep(st->conv_trigger, &conv_state);
}

/*
 * ad4691_enter_conversion_mode - Switch the chip to its buffer conversion mode.
 *
 * Configures the ADC hardware registers for the mode selected at probe
 * (CNV_BURST or MANUAL). Called from buffer preenable before starting
 * sampling. The chip is in AUTONOMOUS mode during idle (for read_raw).
 */
static int ad4691_enter_conversion_mode(struct ad4691_state *st)
{
	int ret;

	if (st->manual_mode)
		return regmap_update_bits(st->regmap, AD4691_DEVICE_SETUP,
					  AD4691_MANUAL_MODE, AD4691_MANUAL_MODE);

	ret = regmap_write(st->regmap, AD4691_ADC_SETUP, AD4691_CNV_BURST_MODE);
	if (ret)
		return ret;

	return regmap_write(st->regmap, AD4691_STATE_RESET_REG,
			    AD4691_STATE_RESET_ALL);
}

/*
 * ad4691_exit_conversion_mode - Return the chip to AUTONOMOUS mode.
 *
 * Called from buffer postdisable to restore the chip to the
 * idle state used by read_raw. Clears the sequencer and resets state.
 */
static int ad4691_exit_conversion_mode(struct ad4691_state *st)
{
	if (st->manual_mode)
		return regmap_update_bits(st->regmap, AD4691_DEVICE_SETUP,
					  AD4691_MANUAL_MODE, 0);

	return regmap_write(st->regmap, AD4691_ADC_SETUP,
			    AD4691_AUTONOMOUS_MODE);
}

static int ad4691_manual_buffer_preenable(struct iio_dev *indio_dev)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(st->regmap);
	struct spi_device *spi = to_spi_device(dev);
	unsigned int n_active = hweight_long(*indio_dev->active_scan_mask);
	unsigned int n_xfers = n_active + 1;
	unsigned int k, i;
	int ret;

	st->scan_devm_group = devres_open_group(dev, NULL, GFP_KERNEL);
	if (!st->scan_devm_group)
		return -ENOMEM;

	st->scan_xfers = devm_kcalloc(dev, n_xfers, sizeof(*st->scan_xfers),
				      GFP_KERNEL);
	st->scan_tx = devm_kcalloc(dev, n_xfers, sizeof(*st->scan_tx),
				   GFP_KERNEL);
	st->scan_rx = devm_kcalloc(dev, n_xfers, sizeof(*st->scan_rx),
				   GFP_KERNEL);
	if (!st->scan_xfers || !st->scan_tx || !st->scan_rx) {
		devres_release_group(dev, st->scan_devm_group);
		return -ENOMEM;
	}

	spi_message_init(&st->scan_msg);

	k = 0;
	iio_for_each_active_channel(indio_dev, i) {
		st->scan_tx[k] = cpu_to_be16(AD4691_ADC_CHAN(i));
		st->scan_xfers[k].tx_buf = &st->scan_tx[k];
		st->scan_xfers[k].rx_buf = &st->scan_rx[k];
		st->scan_xfers[k].len = sizeof(__be16);
		st->scan_xfers[k].cs_change = 1;
		spi_message_add_tail(&st->scan_xfers[k], &st->scan_msg);
		k++;
	}

	/* Final NOOP transfer to retrieve last channel's result. */
	st->scan_tx[k] = cpu_to_be16(AD4691_NOOP);
	st->scan_xfers[k].tx_buf = &st->scan_tx[k];
	st->scan_xfers[k].rx_buf = &st->scan_rx[k];
	st->scan_xfers[k].len = sizeof(__be16);
	spi_message_add_tail(&st->scan_xfers[k], &st->scan_msg);

	devres_close_group(dev, st->scan_devm_group);

	st->scan_msg.spi = spi;

	ret = spi_optimize_message(spi, &st->scan_msg);
	if (ret) {
		devres_release_group(dev, st->scan_devm_group);
		return ret;
	}

	ret = ad4691_enter_conversion_mode(st);
	if (ret) {
		spi_unoptimize_message(&st->scan_msg);
		devres_release_group(dev, st->scan_devm_group);
		return ret;
	}

	return 0;
}

static int ad4691_manual_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(st->regmap);
	int ret;

	ret = ad4691_exit_conversion_mode(st);
	spi_unoptimize_message(&st->scan_msg);
	devres_release_group(dev, st->scan_devm_group);
	return ret;
}

static const struct iio_buffer_setup_ops ad4691_manual_buffer_setup_ops = {
	.preenable = &ad4691_manual_buffer_preenable,
	.postdisable = &ad4691_manual_buffer_postdisable,
};

static int ad4691_cnv_burst_buffer_preenable(struct iio_dev *indio_dev)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(st->regmap);
	struct spi_device *spi = to_spi_device(dev);
	unsigned int n_active = hweight_long(*indio_dev->active_scan_mask);
	unsigned int bit, k, i;
	int ret;

	st->scan_devm_group = devres_open_group(dev, NULL, GFP_KERNEL);
	if (!st->scan_devm_group)
		return -ENOMEM;

	st->scan_xfers = devm_kcalloc(dev, 2 * n_active, sizeof(*st->scan_xfers),
				      GFP_KERNEL);
	st->scan_tx = devm_kcalloc(dev, n_active, sizeof(*st->scan_tx),
				   GFP_KERNEL);
	st->scan_rx = devm_kcalloc(dev, n_active, sizeof(*st->scan_rx),
				   GFP_KERNEL);
	if (!st->scan_xfers || !st->scan_tx || !st->scan_rx) {
		devres_release_group(dev, st->scan_devm_group);
		return -ENOMEM;
	}

	spi_message_init(&st->scan_msg);

	/*
	 * Each AVG_IN read needs two transfers: a 2-byte address write phase
	 * followed by a 2-byte data read phase. CS toggles between channels
	 * (cs_change=1 on the read phase of all but the last channel).
	 */
	k = 0;
	iio_for_each_active_channel(indio_dev, i) {
		st->scan_tx[k] = cpu_to_be16(0x8000 | AD4691_AVG_IN(i));
		st->scan_xfers[2 * k].tx_buf = &st->scan_tx[k];
		st->scan_xfers[2 * k].len = sizeof(__be16);
		spi_message_add_tail(&st->scan_xfers[2 * k], &st->scan_msg);
		st->scan_xfers[2 * k + 1].rx_buf = &st->scan_rx[k];
		st->scan_xfers[2 * k + 1].len = sizeof(__be16);
		if (k < n_active - 1)
			st->scan_xfers[2 * k + 1].cs_change = 1;
		spi_message_add_tail(&st->scan_xfers[2 * k + 1], &st->scan_msg);
		k++;
	}

	devres_close_group(dev, st->scan_devm_group);

	st->scan_msg.spi = spi;

	ret = spi_optimize_message(spi, &st->scan_msg);
	if (ret) {
		devres_release_group(dev, st->scan_devm_group);
		return ret;
	}

	ret = regmap_write(st->regmap, AD4691_ACC_MASK_REG,
			   (u16)~(*indio_dev->active_scan_mask));
	if (ret)
		goto err;

	ret = regmap_write(st->regmap, AD4691_STD_SEQ_CONFIG,
			   *indio_dev->active_scan_mask);
	if (ret)
		goto err;

	iio_for_each_active_channel(indio_dev, bit) {
		ret = regmap_write(st->regmap, AD4691_ACC_COUNT_LIMIT(bit),
				   AD4691_ACC_COUNT_VAL);
		if (ret)
			goto err;
	}

	ret = ad4691_enter_conversion_mode(st);
	if (ret)
		goto err;

	ret = ad4691_sampling_enable(st, true);
	if (ret)
		goto err;

	enable_irq(st->irq);
	return 0;
err:
	spi_unoptimize_message(&st->scan_msg);
	devres_release_group(dev, st->scan_devm_group);
	return ret;
}

static int ad4691_cnv_burst_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct ad4691_state *st = iio_priv(indio_dev);
	struct device *dev = regmap_get_device(st->regmap);
	int ret;

	disable_irq(st->irq);

	ret = ad4691_sampling_enable(st, false);
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, AD4691_STD_SEQ_CONFIG,
			   AD4691_SEQ_ALL_CHANNELS_OFF);
	if (ret)
		return ret;

	ret = ad4691_exit_conversion_mode(st);
	spi_unoptimize_message(&st->scan_msg);
	devres_release_group(dev, st->scan_devm_group);
	return ret;
}

static const struct iio_buffer_setup_ops ad4691_cnv_burst_buffer_setup_ops = {
	.preenable = &ad4691_cnv_burst_buffer_preenable,
	.postdisable = &ad4691_cnv_burst_buffer_postdisable,
};

static ssize_t sampling_frequency_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct ad4691_state *st = iio_priv(indio_dev);

	if (st->manual_mode)
		return -ENODEV;

	return sysfs_emit(buf, "%u\n", (u32)(NSEC_PER_SEC / st->cnv_period_ns));
}

static ssize_t sampling_frequency_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct ad4691_state *st = iio_priv(indio_dev);
	int freq, ret;

	if (st->manual_mode)
		return -ENODEV;

	ret = kstrtoint(buf, 10, &freq);
	if (ret)
		return ret;

	guard(mutex)(&st->lock);

	ret = ad4691_set_pwm_freq(st, freq);
	if (ret)
		return ret;

	return len;
}

static IIO_DEVICE_ATTR(sampling_frequency, 0644,
		       sampling_frequency_show,
		       sampling_frequency_store, 0);

static const struct iio_dev_attr *ad4691_buffer_attrs[] = {
	&iio_dev_attr_sampling_frequency,
	NULL,
};

static irqreturn_t ad4691_irq(int irq, void *private)
{
	struct iio_dev *indio_dev = private;
	struct ad4691_state *st = iio_priv(indio_dev);

	/*
	 * GPx has asserted: stop conversions before reading so the
	 * accumulator does not continue sampling while the trigger handler
	 * processes the data. Then fire the IIO trigger to push the sample
	 * to the buffer.
	 */
	ad4691_sampling_enable(st, false);
	iio_trigger_poll(st->trig);

	return IRQ_HANDLED;
}

static const struct iio_trigger_ops ad4691_trigger_ops = {
	.validate_device = iio_trigger_validate_own_device,
};

static irqreturn_t ad4691_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ad4691_state *st = iio_priv(indio_dev);
	unsigned int i, k = 0;
	int ret;

	guard(mutex)(&st->lock);

	ret = spi_sync(st->scan_msg.spi, &st->scan_msg);
	if (ret)
		goto done;

	if (st->manual_mode) {
		iio_for_each_active_channel(indio_dev, i) {
			st->scan.vals[i] = be16_to_cpu(st->scan_rx[k + 1]);
			k++;
		}
	} else {
		iio_for_each_active_channel(indio_dev, i) {
			st->scan.vals[i] = be16_to_cpu(st->scan_rx[k]);
			k++;
		}

		ret = regmap_write(st->regmap, AD4691_STATE_RESET_REG,
				   AD4691_STATE_RESET_ALL);
		if (ret)
			goto done;

		ret = ad4691_sampling_enable(st, true);
		if (ret)
			goto done;
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
	.read_avail = &ad4691_read_avail,
	.debugfs_reg_access = &ad4691_reg_access,
};

static int ad4691_pwm_setup(struct ad4691_state *st)
{
	struct device *dev = regmap_get_device(st->regmap);
	int ret;

	st->conv_trigger = devm_pwm_get(dev, "cnv");
	if (IS_ERR(st->conv_trigger))
		return dev_err_probe(dev, PTR_ERR(st->conv_trigger),
				     "Failed to get cnv pwm\n");

	ret = devm_add_action_or_reset(dev, ad4691_disable_pwm,
				       st->conv_trigger);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to register PWM disable action\n");

	return ad4691_set_pwm_freq(st, st->info->max_rate);
}

static int ad4691_regulator_setup(struct ad4691_state *st)
{
	struct device *dev = regmap_get_device(st->regmap);
	int ret;

	ret = devm_regulator_get_enable(dev, "avdd");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get and enable AVDD\n");

	ret = devm_regulator_get_enable(dev, "ldo-in");
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "Failed to get and enable LDO-IN\n");
	st->ldo_en = (ret == -ENODEV);

	ret = devm_regulator_get_enable(dev, "vio");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get and enable VIO\n");

	st->vref_uV = devm_regulator_get_enable_read_voltage(dev, "ref");
	if (st->vref_uV >= 0) {
		st->refbuf_en = false;
	} else if (st->vref_uV == -ENODEV) {
		st->vref_uV = devm_regulator_get_enable_read_voltage(dev, "refin");
		st->refbuf_en = true;
	}
	if (st->vref_uV < 0)
		return dev_err_probe(dev, st->vref_uV,
				     "Failed to get reference supply\n");

	if (st->vref_uV < AD4691_VREF_uV_MIN || st->vref_uV > AD4691_VREF_uV_MAX)
		return dev_err_probe(dev, -EINVAL,
				     "vref(%d) must be in the range [%u...%u]\n",
				     st->vref_uV, AD4691_VREF_uV_MIN,
				     AD4691_VREF_uV_MAX);

	return 0;
}

static int ad4691_reset(struct ad4691_state *st)
{
	struct device *dev = regmap_get_device(st->regmap);
	struct reset_control *rst;

	rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "Failed to get reset\n");

	if (!rst)
		/* No hardware reset available, fall back to software reset. */
		return regmap_write(st->regmap, AD4691_SPI_CONFIG_A_REG,
				    AD4691_SW_RESET);

	reset_control_assert(rst);
	/* Reset delay required. See datasheet Table 5. */
	fsleep(300);
	reset_control_deassert(rst);

	return 0;
}

static int ad4691_config(struct ad4691_state *st, u32 max_speed_hz)
{
	struct device *dev = regmap_get_device(st->regmap);
	enum ad4691_ref_ctrl ref_val;
	const char *irq_name;
	unsigned int gp_num;
	int ret;

	/*
	 * Determine buffer conversion mode from DT: if a PWM is provided it
	 * drives the CNV pin (CNV_BURST_MODE); otherwise CNV is tied to CS
	 * and each SPI transfer triggers a conversion (MANUAL_MODE).
	 * Both modes idle in AUTONOMOUS mode so that read_raw can use the
	 * internal oscillator without disturbing the hardware configuration.
	 */
	if (device_property_present(dev, "pwms")) {
		st->manual_mode = false;
		ret = ad4691_pwm_setup(st);
		if (ret)
			return ret;
	} else {
		st->manual_mode = true;
	}

	switch (st->vref_uV) {
	case AD4691_VREF_uV_MIN ... AD4691_VREF_2P5_uV_MAX:
		ref_val = AD4691_VREF_2P5;
		break;
	case AD4691_VREF_2P5_uV_MAX + 1 ... AD4691_VREF_3P0_uV_MAX:
		ref_val = AD4691_VREF_3P0;
		break;
	case AD4691_VREF_3P0_uV_MAX + 1 ... AD4691_VREF_3P3_uV_MAX:
		ref_val = AD4691_VREF_3P3;
		break;
	case AD4691_VREF_3P3_uV_MAX + 1 ... AD4691_VREF_4P096_uV_MAX:
		ref_val = AD4691_VREF_4P096;
		break;
	case AD4691_VREF_4P096_uV_MAX + 1 ... AD4691_VREF_uV_MAX:
		ref_val = AD4691_VREF_5P0;
		break;
	default:
		return dev_err_probe(dev, -EINVAL,
				     "Unsupported vref voltage: %d uV\n",
				     st->vref_uV);
	}

	ret = regmap_write(st->regmap, AD4691_REF_CTRL,
			   FIELD_PREP(AD4691_REF_CTRL_MASK, ref_val) |
			   (st->refbuf_en ? AD4691_REFBUF_EN : 0));
	if (ret)
		return dev_err_probe(dev, ret, "Failed to write REF_CTRL\n");

	ret = regmap_write(st->regmap, AD4691_DEVICE_SETUP,
			   st->ldo_en ? AD4691_LDO_EN : 0);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to write DEVICE_SETUP\n");

	/*
	 * Set the internal oscillator to the highest rate this chip supports.
	 * Index 0 (1 MHz) exceeds the 500 kHz max of AD4691/AD4693, so those
	 * chips start at index 1 (500 kHz).
	 */
	ret = regmap_write(st->regmap, AD4691_OSC_FREQ_REG,
			   (ad4691_osc_freqs[0] > st->info->max_rate) ? 1 : 0);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to write OSC_FREQ\n");

	/* Device always operates in AUTONOMOUS mode. */
	ret = regmap_write(st->regmap, AD4691_ADC_SETUP, AD4691_AUTONOMOUS_MODE);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to write ADC_SETUp\n");

	if (st->manual_mode)
		return 0;

	ret = device_property_read_string_array(dev, "interrupt-names",
						&irq_name, 1);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "Failed to read interrupt-names\n");

	if (strncmp(irq_name, "gp", 2) != 0 ||
	    kstrtouint(irq_name + 2, 10, &gp_num) || gp_num > 3)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid interrupt name '%s'\n", irq_name);

	return ad4691_gpio_setup(st, gp_num);
}

static int ad4691_setup_triggered_buffer(struct iio_dev *indio_dev,
					 struct ad4691_state *st)
{
	struct device *dev = regmap_get_device(st->regmap);
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

	if (!st->manual_mode) {
		/*
		 * GP0 asserts at end-of-conversion. The IRQ handler stops
		 * conversions and fires the IIO trigger so the trigger handler
		 * can read and push the sample to the buffer. The IRQ is kept
		 * disabled until the buffer is enabled.
		 */
		irq = fwnode_irq_get(dev_fwnode(dev), 0);
		if (irq < 0)
			return dev_err_probe(dev, irq,
					     "failed to get GP interrupt\n");

		st->irq = irq;

		ret = devm_request_threaded_irq(dev, irq, NULL,
						&ad4691_irq,
						IRQF_ONESHOT | IRQF_NO_AUTOEN,
						indio_dev->name, indio_dev);
		if (ret)
			return ret;

		return devm_iio_triggered_buffer_setup_ext(dev, indio_dev,
							   &iio_pollfunc_store_time,
							   &ad4691_trigger_handler,
							   IIO_BUFFER_DIRECTION_IN,
							   &ad4691_cnv_burst_buffer_setup_ops,
							   ad4691_buffer_attrs);
	}

	return devm_iio_triggered_buffer_setup(dev, indio_dev,
					       &iio_pollfunc_store_time,
					       &ad4691_trigger_handler,
					       &ad4691_manual_buffer_setup_ops);
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
	st->info = spi_get_device_match_data(spi);

	ret = devm_mutex_init(dev, &st->lock);
	if (ret)
		return ret;

	st->regmap = devm_regmap_init(dev, NULL, spi, &ad4691_regmap_config);
	if (IS_ERR(st->regmap))
		return dev_err_probe(dev, PTR_ERR(st->regmap),
				     "Failed to initialize regmap\n");

	ret = ad4691_regulator_setup(st);
	if (ret)
		return ret;

	ret = ad4691_reset(st);
	if (ret)
		return ret;

	ret = ad4691_config(st, spi->max_speed_hz);
	if (ret)
		return ret;

	indio_dev->name = st->info->name;
	indio_dev->info = &ad4691_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	indio_dev->channels = st->info->channels;
	indio_dev->num_channels = st->info->num_channels;

	ret = ad4691_setup_triggered_buffer(indio_dev, st);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id ad4691_of_match[] = {
	{ .compatible = "adi,ad4691", .data = &ad4691_chip_info },
	{ .compatible = "adi,ad4692", .data = &ad4692_chip_info },
	{ .compatible = "adi,ad4693", .data = &ad4693_chip_info },
	{ .compatible = "adi,ad4694", .data = &ad4694_chip_info },
	{ }
};
MODULE_DEVICE_TABLE(of, ad4691_of_match);

static const struct spi_device_id ad4691_id[] = {
	{ "ad4691", (kernel_ulong_t)&ad4691_chip_info },
	{ "ad4692", (kernel_ulong_t)&ad4692_chip_info },
	{ "ad4693", (kernel_ulong_t)&ad4693_chip_info },
	{ "ad4694", (kernel_ulong_t)&ad4694_chip_info },
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
