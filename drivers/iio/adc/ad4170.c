// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2024 Analog Devices, Inc.
 * Author: Ana-Maria Cusco <ana-maria.cusco@analog.com>
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/units.h>
#include <linux/util_macros.h>

#include <asm/div64.h>
#include <linux/unaligned.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/kfifo_buf.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>
#include "ad4170.h"

#define AD4170_SINC5_AVG_CONF		0x0
#define AD4170_SINC5_CONF		0x4
#define AD4170_SINC3_CONF		0x6

enum ad4170_regulator {
	AD4170_AVDD_SUP,
	AD4170_AVSS_SUP,
	AD4170_IOVDD_SUP,
	AD4170_REFIN1P_SUP,
	AD4170_REFIN1N_SUP,
	AD4170_REFIN2P_SUP,
	AD4170_REFIN2N_SUP,
};

static const char *const ad4170_clk_sel[] = {
	"ext-clk", "xtal"
};

static const char *const ad4170_i_out_pin_dt_props[] = {
	"adi,excitation-pin-0",
	"adi,excitation-pin-1",
	"adi,excitation-pin-2",
	"adi,excitation-pin-3",
};

static const char *const ad4170_i_out_val_dt_props[] = {
	"adi,excitation-current-0-microamp",
	"adi,excitation-current-1-microamp",
	"adi,excitation-current-2-microamp",
	"adi,excitation-current-3-microamp",
};

#define AD4170_SINC5_FS_PADDING		8
#define AD4170_SINC3_FS_OFFSET		2

#define AD4170_SINC3_MIN_FS		4
#define AD4170_SINC3_MAX_FS		65532
#define AD4170_SINC5_MIN_FS		1
#define AD4170_SINC5_MAX_FS		256

/* Make separate fs tables? One for SINC5 other for SINC5+AVG and SINC3? */
static const unsigned int ad4170_filt_fs_tbl[] = {
	1, /* SINC5 minimum filter_fs */
	2,
	4, /* SINC5+AVG and SINC3 minimum filter_fs */
	8,
	12,
	16,
	20,
	40,
	48,
	80,
	100,
	256, /* SINC5 maximum filter_fs */
	500,
	1000,
	5000,
	8332,
	10000,
	25000,
	50000,
	65532 /* SINC5+AVG and SINC3 maximum filter_fs */
};

/*
 * There are 8 of each MISC, AFE, FILTER, FILTER_FS, OFFSET, and GAIN
 * configuration registers. That is, there are 8 miscellaneous registers, MISC0
 * to MISC7. Each MISC register is associated with a setup; MISCN is associated
 * with setup number N. The other 5 above mentioned types of registers have
 * analogous structure. A setup is a set of those registers. For example,
 * setup 1 comprises of MISC1, AFE1, FILTER1, FILTER_FS1, OFFSET1, and GAIN1
 * registers. Also, there are 16 CHANNEL_SETUP registers (CHANNEL_SETUP0 to
 * CHANNEL_SETUP15). Each channel setup is associated with one of the 8 possible
 * setups. Thus, AD4170 can support up to 16 channels but, since there are only
 * 8 available setups, channels must share settings if more than 8 channels are
 * configured.
 */
struct ad4170_setup {
	u16 misc;
	u16 afe;
	u16 filter;
	u16 filter_fs;
	u32 offset; /* For calibration purposes */
	u32 gain; /* For calibration purposes */
};

struct ad4170_chan_info {
	int setup_num;
	int input_range_uv;
	u32 scale_tbl[10][2];
	int offset_tbl[10];
	bool enabled;
};

static const char * const ad4170_filt_names[] = {
	[AD4170_SINC5_AVG] = "sinc5+avg",
	[AD4170_SINC5] = "sinc5",
	[AD4170_SINC3] = "sinc3",
};

struct ad4170_state {
	struct regmap *regmap;
	struct spi_device *spi;
	struct regulator_bulk_data supplies[7];
	struct mutex lock; /* Protect filter, PGA, GPIO, chan read, chan config */
	struct iio_chan_spec chans[AD4170_MAX_CHANNELS];
	struct ad4170_chan_info chans_info[AD4170_MAX_CHANNELS];
	struct ad4170_setup setups[AD4170_MAX_SETUPS];
	u32 mclk_hz;
	enum ad4170_pin_function pins_fn[AD4170_NUM_ANALOG_PINS];
	enum ad4170_gpio_function gpio_fn[AD4170_NUM_GPIO_PINS];
	unsigned int clock_ctrl;
	struct clk *ext_clk;
	struct clk_hw int_clk_hw;
	int sps_tbl[ARRAY_SIZE(ad4170_filt_names)][ARRAY_SIZE(ad4170_filt_fs_tbl)][2];
	struct completion completion;
	struct iio_trigger *trig;
	u32 data[AD4170_MAX_CHANNELS];

	struct spi_transfer xfer;
	struct spi_message msg;
	/*
	 * DMA (thus cache coherency maintenance) requires the transfer buffers
	 * to live in their own cache lines.
	 */
	u8 reg_write_tx_buf[6];
	__be32 reg_read_rx_buf __aligned(IIO_DMA_MINALIGN);
	__be16 reg_read_tx_buf;
};

static void ad4170_fill_sps_tbl(struct ad4170_state *st)
{
	unsigned int tmp0, tmp1, i;

	/*
	 * The ODR can be calculated the same way for sinc5+avg, sinc5, and
	 * sinc3 filter types with the exception that sinc5 filter has a
	 * narrowed range of allowed FILTER_FS values.
	 */
	for (i = AD4170_SINC3_FS_OFFSET; i < ARRAY_SIZE(ad4170_filt_fs_tbl); i++) {
		tmp0 = div_u64_rem(st->mclk_hz, 32 * ad4170_filt_fs_tbl[i], &tmp1);
		tmp1 = mult_frac(tmp1, MICRO, 32 * ad4170_filt_fs_tbl[i]);
		// sinc5+avg and sinc3 havethe same SPS table. Drop one of them?
		/* Fill sinc5+avg filter SPS table */
		st->sps_tbl[AD4170_SINC5_AVG][i][0] = tmp0; /* Integer part */
		st->sps_tbl[AD4170_SINC5_AVG][i][1] = tmp1; /* Fractional part */

		/* Fill sinc3 filter SPS table */
		st->sps_tbl[AD4170_SINC3][i][0] = tmp0; /* Integer part */
		st->sps_tbl[AD4170_SINC3][i][1] = tmp1; /* Fractional part */
	}
	/* Sinc5 filter ODR doesn't use all FILTER_FS bits */
	for (i = 0; i < ARRAY_SIZE(ad4170_filt_fs_tbl) - AD4170_SINC5_FS_PADDING; i++) {
		tmp0 = div_u64_rem(st->mclk_hz, 32 * ad4170_filt_fs_tbl[i], &tmp1);
		tmp1 = mult_frac(tmp1, MICRO, 32 * ad4170_filt_fs_tbl[i]);
		/* Fill sinc5 filter SPS table */
		st->sps_tbl[AD4170_SINC5][i][0] = tmp0; /* Integer part */
		st->sps_tbl[AD4170_SINC5][i][1] = tmp1; /* Fractional part */
	}
}

static int ad4170_get_reg_size(struct ad4170_state *st, unsigned int reg,
			       unsigned int *size)
{
	if (reg >= ARRAY_SIZE(ad4170_reg_size))
		return -EINVAL;

	if (!ad4170_reg_size[reg])
		return -EINVAL;

	*size = ad4170_reg_size[reg];

	return 0;
}

static int ad4170_reg_access(struct iio_dev *indio_dev, unsigned int reg,
			     unsigned int writeval, unsigned int *readval)
{
	struct ad4170_state *st = iio_priv(indio_dev);

	if (readval)
		return regmap_read(st->regmap, reg, readval);

	return regmap_write(st->regmap, reg, writeval);
}

static int ad4170_reg_write(void *context, unsigned int reg, unsigned int val)
{
	struct ad4170_state *st = context;
	unsigned int size, addr;
	int ret;

	ret = ad4170_get_reg_size(st, reg, &size);
	if (ret)
		return ret;

	addr = reg + size - 1;
	put_unaligned_be16(addr, &st->reg_write_tx_buf[0]);

	switch (size) {
	case 4:
		put_unaligned_be32(val, &st->reg_write_tx_buf[2]);
		break;
	case 3:
		put_unaligned_be24(val, &st->reg_write_tx_buf[2]);
		break;
	case 2:
		put_unaligned_be16(val, &st->reg_write_tx_buf[2]);
		break;
	case 1:
		st->reg_write_tx_buf[2] = val;
		break;
	default:
		return -EINVAL;
	}

	return spi_write(st->spi, st->reg_write_tx_buf, size + 2);
}

static int ad4170_reg_read(void *context, unsigned int reg, unsigned int *val)
{
	struct ad4170_state *st = context;
	struct spi_transfer t[] = {
		{
			.tx_buf = &st->reg_read_tx_buf,
			.len = 2,
		},
		{
			.rx_buf = &st->reg_read_rx_buf,
		},
	};
	unsigned int size, addr;
	int ret;

	ret = ad4170_get_reg_size(st, reg, &size);
	if (ret)
		return ret;

	addr = reg + size - 1;
	put_unaligned_be16(AD4170_READ_MASK | addr, &st->reg_read_tx_buf);

	t[1].len = size;

	ret = spi_sync_transfer(st->spi, t, ARRAY_SIZE(t));
	if (ret)
		return ret;

	switch (size) {
	case 4:
		*val = get_unaligned_be32(&st->reg_read_rx_buf);
		break;
	case 3:
		*val = get_unaligned_be24(&st->reg_read_rx_buf);
		break;
	case 2:
		*val = get_unaligned_be16(&st->reg_read_rx_buf);
		break;
	case 1:
		*val = st->reg_read_rx_buf;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct regmap_config ad4170_regmap_config = {
	.reg_bits = 14,
	.val_bits = 32,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.reg_read = ad4170_reg_read,
	.reg_write = ad4170_reg_write,
};

static int ad4170_set_mode(struct ad4170_state *st, unsigned int mode)
{
	return regmap_update_bits(st->regmap, AD4170_ADC_CTRL_REG,
				  AD4170_REG_CTRL_MODE_MSK,
				  FIELD_PREP(AD4170_REG_CTRL_MODE_MSK, mode));
}

/* 8 possible setups (0-7)*/
static int ad4170_write_setup(struct ad4170_state *st, unsigned int setup_num,
			      struct ad4170_setup *setup)
{
	int ret;

	/*
	 * It is recommended to place the ADC in standby mode or idle mode to
	 * write to OFFSET and GAIN registers.
	 */
	ret = ad4170_set_mode(st, AD4170_ADC_CTRL_MODE_IDLE);
	if (ret)
		return ret;

	ret = regmap_write(st->regmap, AD4170_MISC_REG(setup_num), setup->misc);
	if (ret < 0)
		return ret;

	ret = regmap_write(st->regmap, AD4170_AFE_REG(setup_num), setup->afe);
	if (ret < 0)
		return ret;

	ret = regmap_write(st->regmap, AD4170_FILTER_REG(setup_num),
			   setup->filter);
	if (ret < 0)
		return ret;

	ret = regmap_write(st->regmap, AD4170_FILTER_FS_REG(setup_num),
			   setup->filter_fs);
	if (ret < 0)
		return ret;

	ret = regmap_write(st->regmap, AD4170_OFFSET_REG(setup_num),
			   setup->offset);
	if (ret < 0)
		return ret;

	ret = regmap_write(st->regmap, AD4170_GAIN_REG(setup_num), setup->gain);
	if (ret < 0)
		return ret;

	return 0;
}

static int ad4170_filter_to_filter_type(unsigned int filter)
{
	u16 f_conf = FIELD_GET(AD4170_SETUPS_FILTER_TYPE_MSK, filter);

	switch (f_conf) {
	case AD4170_SINC5_AVG_CONF:
		return AD4170_SINC5_AVG;
	case AD4170_SINC5_CONF:
		return AD4170_SINC5;
	case AD4170_SINC3_CONF:
		return AD4170_SINC3;
	default:
		return -EINVAL;
	}
}

static int ad4170_set_filter_type(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan,
				  unsigned int val)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct ad4170_chan_info *chan_info = &st->chans_info[chan->address];
	struct ad4170_setup *setup = &st->setups[chan_info->setup_num];
	unsigned int old_filter_fs, old_filter, filter_type_conf;
	int ret = 0;

	switch (val) {
	case AD4170_SINC5_AVG:
		filter_type_conf = AD4170_SINC5_AVG_CONF;
		break;
	case AD4170_SINC5:
		filter_type_conf = AD4170_SINC5_CONF;
		break;
	case AD4170_SINC3:
		filter_type_conf = AD4170_SINC3_CONF;
		break;
	default:
		return -EINVAL;
	}

	/*
	 * The filters provide the same ODR for a given filter_fs value but
	 * there are different minimum and maximum filter_fs limits for each
	 * filter. The filter_fs value will be adjusted if the current filter_fs
	 * is out of the limits of the just requested filter. Since the
	 * filter_fs value affects the ODR (sampling_frequency), changing the
	 * filter may lead to a change in the sampling frequency.
	 */
	iio_device_claim_direct_scoped(return -EBUSY, indio_dev) {
		old_filter = setup->filter;
		old_filter_fs = setup->filter_fs;
		if (val == AD4170_SINC5_AVG || val == AD4170_SINC3) {
			if (setup->filter_fs < AD4170_SINC3_MIN_FS)
				setup->filter_fs = AD4170_SINC3_MIN_FS;
			if (setup->filter_fs > AD4170_SINC3_MAX_FS)
				setup->filter_fs = AD4170_SINC3_MAX_FS;

		} else if (val == AD4170_SINC5) {
			if (setup->filter_fs < AD4170_SINC5_MIN_FS)
				setup->filter_fs = AD4170_SINC5_MIN_FS;
			if (setup->filter_fs > AD4170_SINC5_MAX_FS)
				setup->filter_fs = AD4170_SINC5_MAX_FS;
		}

		setup->filter &= ~AD4170_SETUPS_FILTER_TYPE_MSK;
		setup->filter |= FIELD_PREP(AD4170_SETUPS_FILTER_TYPE_MSK,
					    filter_type_conf);

		guard(mutex)(&st->lock);
		ret = ad4170_write_setup(st, chan_info->setup_num, setup);
		if (ret) {
			setup->filter = old_filter;
			setup->filter_fs = old_filter_fs;
		}
		return ret;
	}
	unreachable();
}

static int ad4170_get_filter_type(struct iio_dev *indio_dev,
				  const struct iio_chan_spec *chan)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct ad4170_chan_info *chan_info = &st->chans_info[chan->address];
	struct ad4170_setup *setup = &st->setups[chan_info->setup_num];

	return ad4170_filter_to_filter_type(setup->filter);
}

static const struct iio_enum ad4170_filter_type_enum = {
	.items = ad4170_filt_names,
	.num_items = ARRAY_SIZE(ad4170_filt_names),
	.get = ad4170_get_filter_type,
	.set = ad4170_set_filter_type,
};

static const struct iio_chan_spec_ext_info ad4170_filter_type_ext_info[] = {
	IIO_ENUM("filter_type", IIO_SEPARATE, &ad4170_filter_type_enum),
	IIO_ENUM_AVAILABLE("filter_type", IIO_SHARED_BY_TYPE,
			   &ad4170_filter_type_enum),
	{ }
};

static const struct iio_chan_spec ad4170_channel_template = {
	.type = IIO_VOLTAGE,
	.indexed = 1,
	.differential = 1,
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			      BIT(IIO_CHAN_INFO_SCALE) |
			      BIT(IIO_CHAN_INFO_OFFSET) |
			      BIT(IIO_CHAN_INFO_CALIBSCALE) |
			      BIT(IIO_CHAN_INFO_CALIBBIAS) |
			      BIT(IIO_CHAN_INFO_SAMP_FREQ),
	.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE) |
					BIT(IIO_CHAN_INFO_SAMP_FREQ),
	.ext_info = ad4170_filter_type_ext_info,
	.scan_type = {
		.realbits = 24,
		.storagebits = 32,
		.endianness = IIO_BE,
	},
};

static int _ad4170_find_table_index(const unsigned int *tbl, size_t len,
				    unsigned int val)
{
	unsigned int i;

	for (i = 0; i < len; i++)
		if (tbl[i] == val)
			return i;

	return -EINVAL;
}

#define ad4170_find_table_index(table, val) \
	_ad4170_find_table_index(table, ARRAY_SIZE(table), val)

static int ad4170_set_channel_enable(struct ad4170_state *st,
				     unsigned int channel, bool status)
{
	struct ad4170_chan_info *chan_info = &st->chans_info[channel];
	int ret;

	if (chan_info->enabled == status)
		return 0;

	ret = regmap_update_bits(st->regmap, AD4170_CHANNEL_EN_REG,
				 AD4170_CHANNEL_EN(channel),
				 status ? AD4170_CHANNEL_EN(channel) : 0);
	if (ret)
		return ret;

	chan_info->enabled = status;
	return 0;
}

static int ad4170_read_sample(struct iio_dev *indio_dev, unsigned int channel,
			      int *val)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct ad4170_chan_info *chan_info = &st->chans_info[channel];
	struct ad4170_setup *setup = &st->setups[chan_info->setup_num];
	int precision_bits = ad4170_channel_template.scan_type.realbits;
	int ret;

	guard(mutex)(&st->lock);
	ret = ad4170_set_channel_enable(st, channel, true);
	if (ret)
		return ret;

	reinit_completion(&st->completion);

	ret = ad4170_set_mode(st, AD4170_ADC_CTRL_MODE_SINGLE);
	if (ret)
		return ret;

	ret = wait_for_completion_timeout(&st->completion, HZ);
	if (!ret)
		goto out;

	ret = regmap_read(st->regmap, AD4170_DATA_24b_REG, val);
	if (ret)
		return ret;

	if (FIELD_GET(AD4170_AFE_BIPOLAR_MSK, setup->afe))
		*val = sign_extend32(*val, precision_bits - 1);
out:
	ret = ad4170_set_channel_enable(st, channel, false);
	if (ret)
		return ret;

	return IIO_VAL_INT;
}

/*
 * Receives the device state, the number of a multiplexed input (AINP_N
 * or AIM_N), and stores the voltage (in µV) of the specified input into the
 * third argument. If the input number is not one of the special multiplexed
 * inputs ((AVDD-AVSS)/5, ..., REFOUT), stores zero to the voltage argument.
 * If a voltage regulator required by the special input is unavailable, return
 * error code. Return 0 on success.
 *
 * @st: pointer to device state struct
 * @ain_n: number of a multiplexed AD4170 input
 * @ain_voltage: pointer to a variable where to store ain_n voltage
 */
static int ad4170_get_AINM_voltage(struct ad4170_state *st, int ain_n,
				   int *ain_voltage)
{
	int ret;

	*ain_voltage = 0;
	switch (ain_n) {
	case AD4170_MAP_AVDD_AVSS_N:
		ret = regulator_get_voltage(st->supplies[AD4170_AVDD_SUP].consumer);
		if (ret < 0)
			return ret;

		*ain_voltage = ret ? ret / 5 : 0;
		return 0;
	case AD4170_MAP_IOVDD_DGND_N:
		ret = regulator_get_voltage(st->supplies[AD4170_IOVDD_SUP].consumer);
		if (ret < 0)
			return ret;

		*ain_voltage = ret ? ret / 5 : 0;
		return 0;
	case AD4170_MAP_AVSS:
		ret = regulator_get_voltage(st->supplies[AD4170_AVSS_SUP].consumer);
		if (ret < 0)
			ret = 0; /* Assume AVSS at 0V if not provided */

		/* AVSS is never above 0V, i.e., it can only be negative. */
		*ain_voltage = -ret; /* AVSS is a negative voltage */
		return 0;
	case AD4170_MAP_DGND:
		*ain_voltage = 0;
		return 0;
	case AD4170_MAP_REFIN1_P:
		ret = regulator_get_voltage(st->supplies[AD4170_REFIN1P_SUP].consumer);
		if (ret < 0)
			return ret;

		*ain_voltage = ret;
		return 0;
	case AD4170_MAP_REFIN1_N:
		ret = regulator_get_voltage(st->supplies[AD4170_REFIN1N_SUP].consumer);
		if (ret < 0)
			return ret;

		/*
		 * Making the assumption negative inputs of voltage references
		 * are either at GND level or negative with respect to GND.
		 */
		*ain_voltage = -ret;
		return 0;
	case AD4170_MAP_REFIN2_P:
		ret = regulator_get_voltage(st->supplies[AD4170_REFIN2P_SUP].consumer);
		if (ret < 0)
			return ret;

		*ain_voltage = ret;
		return 0;
	case AD4170_MAP_REFIN2_N:
		ret = regulator_get_voltage(st->supplies[AD4170_REFIN2N_SUP].consumer);
		if (ret < 0)
			return ret;

		/*
		 * Making the assumption negative inputs of voltage references
		 * are either at GND level or negative with respect to GND.
		 */
		*ain_voltage = -ret;
		return 0;
	case AD4170_MAP_REFOUT:
		/* REFOUT is 2.5V relative to AVSS so take that into account */
		ret = regulator_get_voltage(st->supplies[AD4170_AVSS_SUP].consumer);
		if (ret < 0)
			ret = 0; /* Assume AVSS at GND (0V) if not provided */

		*ain_voltage = AD4170_INT_REF_2_5V - ret;
		return 0;
	}
	return -EINVAL;
}

static int ad4170_validate_analog_input(struct ad4170_state *st, int pin)
{
	if (pin <= AD4170_MAX_ANALOG_PINS) {
		if (st->pins_fn[pin] != AD4170_PIN_UNASIGNED)
			return dev_err_probe(&st->spi->dev, -EINVAL,
					     "Pin %d already used with fn %u.\n",
					     pin, st->pins_fn[pin]);

		st->pins_fn[pin] = AD4170_PIN_ANALOG_IN;
	}
	return 0;
}

static int ad4170_validate_channel_input(struct ad4170_state *st, int pin, bool com)
{
	/* Check common-mode input pin is mapped to a special input. */
	if (com && (pin < AD4170_MAP_AVDD_AVSS_P || pin > AD4170_MAP_REFOUT))
		return dev_err_probe(&st->spi->dev, -EINVAL,
				     "Invalid common-mode input pin number. %d\n",
				     pin);

	/* Check differential input pin is mapped to a analog input pin. */
	if (!com && pin > AD4170_MAX_ANALOG_PINS)
		return dev_err_probe(&st->spi->dev, -EINVAL,
				     "Invalid analog input pin number. %d\n",
				     pin);

	return ad4170_validate_analog_input(st, pin);
}

/*
 * Verifies whether the channel input configuration is valid by checking the
 * provided input type and input numbers.
 * Returns 0 on valid channel input configuration. -EINVAl otherwise.
 */
static int ad4170_validate_channel(struct ad4170_state *st,
				   struct iio_chan_spec const *chan)
{
	int ret;

	/* Check temperature channel mapping. */
	if (chan->channel == AD4170_MAP_TEMP_SENSOR_P) {
		if (chan->channel2 != AD4170_MAP_TEMP_SENSOR_N)
			return dev_err_probe(&st->spi->dev, -EINVAL,
					     "Invalid temperature channel pin. %d\n",
					     chan->channel2);

		return 0;
	}

	ret = ad4170_validate_channel_input(st, chan->channel, false);
	if (ret < 0)
		return ret;

	ret = ad4170_validate_channel_input(st, chan->channel2, !chan->differential);
	if (ret < 0)
		return ret;

	return 0;
}

/*
 * Receives the device state, the channel spec, a reference selection, and
 * returns the magnitude of the allowed input range in µV.
 * Verifies whether the channel configuration is valid by checking the provided
 * input type, polarity, and voltage references result in a sane input range.
 * Returns negative error code on failure.
 */
static int ad4170_get_input_range(struct ad4170_state *st,
				  struct iio_chan_spec const *chan,
				  unsigned int ref_sel)
{
	struct ad4170_chan_info *chan_info = &st->chans_info[chan->address];
	struct ad4170_setup *setup = &st->setups[chan_info->setup_num];
	bool bipolar = FIELD_GET(AD4170_AFE_BIPOLAR_MSK, setup->afe);
	int refp, refn, ain_voltage, ret;

	switch (ref_sel) {
	case AD4170_AFE_REFIN_REFIN1:
		refp = regulator_get_voltage(st->supplies[AD4170_REFIN1P_SUP].consumer);
		refn = regulator_get_voltage(st->supplies[AD4170_REFIN1N_SUP].consumer);
		break;
	case AD4170_AFE_REFIN_REFIN2:
		refp = regulator_get_voltage(st->supplies[AD4170_REFIN2P_SUP].consumer);
		refn = regulator_get_voltage(st->supplies[AD4170_REFIN2N_SUP].consumer);
		break;
	case AD4170_AFE_REFIN_AVDD:
		refp = regulator_get_voltage(st->supplies[AD4170_AVDD_SUP].consumer);
		ret = regulator_get_voltage(st->supplies[AD4170_AVSS_SUP].consumer);
		/*
		 * TODO AVSS is actually optional.
		 * Should we handle -EPROBE_DEFER here?
		 */
		if (ret < 0)
			ret = 0; /* Assume AVSS at GND if not provided */

		refn = ret;
		break;
	case AD4170_AFE_REFIN_REFOUT:
		refn = regulator_get_voltage(st->supplies[AD4170_AVSS_SUP].consumer);
		if (refn < 0)
			refn = 0;

		/* REFOUT is 2.5 V relative to AVSS */
		/* avss-supply is never above 0V. */
		refp = AD4170_INT_REF_2_5V - refn;
		break;
	default:
		return -EINVAL;
	}
	if (refp < 0)
		return refp;

	if (refn < 0)
		return refn;

	/*
	 * Find out the analog input range from the channel type, polarity, and
	 * voltage reference selection.
	 * AD4170 channels are either differential or pseudo-differential.
	 */
	/* Differential Input Voltage Range: −VREF/gain to +VREF/gain (datasheet page 6) */
	/* Single-Ended Input Voltage Range: 0 to VREF/gain (datasheet page 6) */
	if (chan->differential) {
		if (!bipolar)
			return dev_err_probe(&st->spi->dev, -EINVAL,
					     "Invalid channel %lu setup.\n",
					     chan->address);

		/* Differential bipolar channel */
		/* avss-supply is never above 0V. */
		/* Assuming refin1n-supply not above 0V. */
		/* Assuming refin2n-supply not above 0V. */
		return refp + refn;
	}
	/*
	 * Some configurations can lead to invalid setups.
	 * For example, if AVSS = -2.5V, REF_SELECT set to REFOUT (REFOUT/AVSS),
	 * and single-ended channel configuration set, then the input range
	 * should go from 0V to +VREF (single-ended - datasheet pg 10), but
	 * REFOUT/AVSS range would be -2.5V to 0V.
	 * Check the positive reference is higher than 0V for pseudo-diff
	 * channels.
	 */
	if (bipolar) {
		/* Pseudo-differential bipolar channel */
		/* Input allowed to swing from GND to +VREF */
		if (refp <= 0)
			return dev_err_probe(&st->spi->dev, -EINVAL,
					     "Invalid setup for channel %lu.\n",
					     chan->address);

		return refp;
	}

	/* Pseudo-differential unipolar channel */
	/* Input allowed to swing from IN- to +VREF */
	if (refp <= 0)
		return dev_err_probe(&st->spi->dev, -EINVAL,
				     "Invalid setup for channel %lu.\n",
				     chan->address);

	ret = ad4170_get_AINM_voltage(st, chan->channel2, &ain_voltage);
	if (ret < 0)
		return ret;

	if (refp - ain_voltage <= 0)
		return dev_err_probe(&st->spi->dev, -EINVAL,
				     "Invalid setup for channel %lu.\n",
				     chan->address);

	return refp - ain_voltage;
}

static int ad4170_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long info)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct ad4170_chan_info *chan_info = &st->chans_info[chan->address];
	struct ad4170_setup *setup = &st->setups[chan_info->setup_num];
	enum ad4170_filter_type f_type;
	unsigned int pga, fs_idx;

	switch (info) {
	case IIO_CHAN_INFO_RAW:
		iio_device_claim_direct_scoped(return -EBUSY, indio_dev)
			return ad4170_read_sample(indio_dev, chan->address, val);
		unreachable();
	case IIO_CHAN_INFO_SCALE:
		pga = FIELD_GET(AD4170_AFE_PGA_GAIN_MSK, setup->afe);
		*val = chan_info->scale_tbl[pga][0];
		*val2 = chan_info->scale_tbl[pga][1];
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_OFFSET:
		pga = FIELD_GET(AD4170_AFE_PGA_GAIN_MSK, setup->afe);
		*val = chan_info->offset_tbl[pga];
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SAMP_FREQ:
		f_type = ad4170_filter_to_filter_type(setup->filter);
		fs_idx = find_closest(setup->filter_fs, ad4170_filt_fs_tbl,
				      ARRAY_SIZE(ad4170_filt_fs_tbl));
		*val = st->sps_tbl[f_type][fs_idx][0];
		*val2 = st->sps_tbl[f_type][fs_idx][1];
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_CALIBBIAS:
		*val = setup->offset;
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_CALIBSCALE:
		*val = setup->gain;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int ad4170_fill_scale_tbl(struct iio_dev *indio_dev, int channel)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct ad4170_chan_info *chan_info = &st->chans_info[channel];
	struct ad4170_setup *setup = &st->setups[chan_info->setup_num];
	const struct iio_chan_spec *chan = &indio_dev->channels[channel];
	bool bipolar = FIELD_GET(AD4170_AFE_BIPOLAR_MSK, setup->afe);
	int ch_resolution = chan->scan_type.realbits - bipolar;
	int pga, ainm_voltage, ret;
	unsigned long long offset;

	ainm_voltage = 0;
	if (chan->channel2 > AD4170_MAP_TEMP_SENSOR_N) {
		ret = ad4170_get_AINM_voltage(st, chan->channel2, &ainm_voltage);
		if (ret < 0)
			return dev_err_probe(&st->spi->dev, ret,
					     "Failed to fill scale table\n");
	}

	for (pga = 0; pga < AD4170_PGA_OPTIONS; pga++) {
		u64 nv;
		unsigned int lshift, rshift;

		/*
		 * The scale factor to get ADC output codes to values in mV
		 * units is given by:
		 * _scale = (input_range / gain) / 2^precision
		 * AD4170 gain is a power of 2 so the above can be written as
		 * _scale = input_range / 2^(precision + gain)
		 * Keep the input range in µV before right shift to preserve
		 * scale precision.
		 */
		nv = (u64)chan_info->input_range_uv * NANO;
		lshift = (pga >> 3 & 1);  /* handle cases 8 and 9 */
		rshift = ch_resolution + (pga & 0x7) - lshift;
		chan_info->scale_tbl[pga][0] = 0;
		chan_info->scale_tbl[pga][1] = div_u64(nv >> rshift, MILLI);

		/*
		 * If the negative input is not at GND, the conversion result
		 * (which is relative to IN-) will be offset by the level at IN-.
		 * Use the scale factor the other way around to go from a known
		 * voltage to the corresponding ADC output code.
		 * With that, we are able to get to what would be the output
		 * code for the voltage at the negative input.
		 * If the negative input is not fixed, there is no offset.
		 */
		offset = ((unsigned long long)ainm_voltage) * MICRO;
		offset = DIV_ROUND_CLOSEST_ULL(offset, chan_info->scale_tbl[pga][1]);

		/*
		 * After divided by the scale, offset will always fit into 31
		 * bits. For _raw + _offset to be relative to GND, the value
		 * provided as _offset is of opposite sign than the real offset.
		 */
		chan_info->offset_tbl[pga] = -(int)(offset);
	}
	return 0;
}

static int ad4170_read_avail(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     const int **vals, int *type, int *length,
			     long info)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct ad4170_chan_info *chan_info = &st->chans_info[chan->address];
	struct ad4170_setup *setup = &st->setups[chan_info->setup_num];
	enum ad4170_filter_type f_type = ad4170_filter_to_filter_type(setup->filter);

	switch (info) {
	case IIO_CHAN_INFO_SCALE:
		*vals = (int *)chan_info->scale_tbl;
		*length = ARRAY_SIZE(chan_info->scale_tbl) * 2;
		*type = IIO_VAL_INT_PLUS_NANO;
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_SAMP_FREQ:
		switch (f_type) {
		case AD4170_SINC5_AVG:
			fallthrough;
		case AD4170_SINC3:
			*vals = (int *)&st->sps_tbl[f_type][AD4170_SINC3_FS_OFFSET];
			*length = (ARRAY_SIZE(ad4170_filt_fs_tbl)
				   - AD4170_SINC3_FS_OFFSET) * 2;
			break;
		case AD4170_SINC5:
			*vals = (int *)st->sps_tbl[f_type];
			*length = (ARRAY_SIZE(ad4170_filt_fs_tbl)
				   - AD4170_SINC5_FS_PADDING) * 2;
			break;
		default:
			return -EINVAL;
		}
		*type = IIO_VAL_INT_PLUS_MICRO;

		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static int ad4170_write_raw_get_fmt(struct iio_dev *indio_dev,
				    struct iio_chan_spec const *chan,
				    long info)
{
	switch (info) {
	case IIO_CHAN_INFO_SCALE:
		return IIO_VAL_INT_PLUS_NANO;
	case IIO_CHAN_INFO_SAMP_FREQ:
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_CALIBBIAS:
	case IIO_CHAN_INFO_CALIBSCALE:
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int ad4170_set_pga(struct iio_dev *indio_dev, struct ad4170_state *st,
			  unsigned int channel_addr, int val, int val2)
{
	struct ad4170_chan_info *chan_info = &st->chans_info[channel_addr];
	struct ad4170_setup *setup = &st->setups[chan_info->setup_num];
	unsigned int old_pga = FIELD_GET(AD4170_AFE_PGA_GAIN_MSK, setup->afe);
	unsigned int pga;
	int ret = 0;

	for (pga = 0; pga < AD4170_PGA_OPTIONS; pga++) {
		if (val == chan_info->scale_tbl[pga][0] &&
		    val2 == chan_info->scale_tbl[pga][1])
			break;
	}

	if (pga == AD4170_PGA_OPTIONS)
		return -EINVAL;

	if (pga == old_pga)
		return 0;

	setup->afe &= ~AD4170_AFE_PGA_GAIN_MSK;
	setup->afe |= FIELD_PREP(AD4170_AFE_PGA_GAIN_MSK, pga);

	guard(mutex)(&st->lock);
	ret = ad4170_write_setup(st, chan_info->setup_num, setup);
	if (ret) {
		setup->afe &= ~AD4170_AFE_PGA_GAIN_MSK;
		setup->afe |= FIELD_PREP(AD4170_AFE_PGA_GAIN_MSK, old_pga);
	}

	return ret;
}

static int ad4170_set_channel_freq(struct ad4170_state *st,
				   unsigned int channel_addr, int val, int val2)
{
	struct ad4170_chan_info *chan_info = &st->chans_info[channel_addr];
	struct ad4170_setup *setup = &st->setups[chan_info->setup_num];
	enum ad4170_filter_type f_type = ad4170_filter_to_filter_type(setup->filter);
	int offset, padding, i, ret = 0;
	unsigned int old_filter_fs;

	switch (f_type) {
	case AD4170_SINC5_AVG:
		fallthrough;
	case AD4170_SINC3:
		offset = AD4170_SINC3_FS_OFFSET;
		padding = 0;
		break;
	case AD4170_SINC5:
		offset = 0;
		padding = AD4170_SINC5_FS_PADDING;
		break;
	}

	for (i = offset; i < ARRAY_SIZE(ad4170_filt_fs_tbl) - padding; i++) {
		if (st->sps_tbl[f_type][i][0] == val &&
		    st->sps_tbl[f_type][i][1] == val2)
			break;
	}
	if (i >= ARRAY_SIZE(ad4170_filt_fs_tbl) - padding)
		return -EINVAL;

	old_filter_fs = setup->filter_fs;
	setup->filter_fs = ad4170_filt_fs_tbl[i];

	guard(mutex)(&st->lock);
	ret = ad4170_write_setup(st, chan_info->setup_num, setup);
	if (ret)
		setup->filter_fs = old_filter_fs;

	return ret;
}

static int ad4170_set_calib_gain(struct iio_dev *indio_dev, int addr, int val)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct ad4170_chan_info *chan_info = &st->chans_info[addr];
	struct ad4170_setup *setup = &st->setups[chan_info->setup_num];
	u32 old_gain;
	int ret;

	old_gain = setup->gain;
	setup->gain = val;

	guard(mutex)(&st->lock);
	ret = ad4170_write_setup(st, chan_info->setup_num, setup);
	if (ret)
		setup->gain = old_gain;

	return ret;
}

static int ad4170_set_calib_offset(struct iio_dev *indio_dev, int addr, int val)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct ad4170_chan_info *chan_info = &st->chans_info[addr];
	struct ad4170_setup *setup = &st->setups[chan_info->setup_num];
	u32 old_offset;
	int ret;

	old_offset = setup->offset;
	setup->offset = val;

	guard(mutex)(&st->lock);
	ret = ad4170_write_setup(st, chan_info->setup_num, setup);
	if (ret)
		setup->offset = old_offset;

	return ret;
}

static int ad4170_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val, int val2, long info)
{
	struct ad4170_state *st = iio_priv(indio_dev);

	iio_device_claim_direct_scoped(return -EBUSY, indio_dev) {
		switch (info) {
		case IIO_CHAN_INFO_SCALE:
			return ad4170_set_pga(indio_dev, st, chan->address, val,
					      val2);
		case IIO_CHAN_INFO_SAMP_FREQ:
			return ad4170_set_channel_freq(st, chan->address, val,
						       val2);
		case IIO_CHAN_INFO_CALIBBIAS:
			return ad4170_set_calib_offset(indio_dev, chan->address,
						       val);
		case IIO_CHAN_INFO_CALIBSCALE:
			return ad4170_set_calib_gain(indio_dev, chan->address,
						     val);
		default:
			return -EINVAL;
		}
	}
	unreachable();
}

static int ad4170_update_scan_mode(struct iio_dev *indio_dev,
				   const unsigned long *active_scan_mask)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	unsigned int channel;
	int ret;

	guard(mutex)(&st->lock);

	for_each_set_bit(channel, active_scan_mask, indio_dev->num_channels) {
		ret = ad4170_set_channel_enable(st, channel, true);
		if (ret)
			return ret;
	}
	return 0;
}

static const struct iio_info ad4170_info = {
	.read_raw = ad4170_read_raw,
	.read_avail = ad4170_read_avail,
	.write_raw = ad4170_write_raw,
	.write_raw_get_fmt = ad4170_write_raw_get_fmt,
	.update_scan_mode = ad4170_update_scan_mode,
	.debugfs_reg_access = ad4170_reg_access,
};

static int ad4170_soft_reset(struct ad4170_state *st)
{
	int ret;

	ret = regmap_write(st->regmap, AD4170_INTERFACE_CONFIG_A_REG,
			   AD4170_SW_RESET_MSK);
	if (ret < 0)
		return ret;

	/*
	 * AD4170-4 requires a minimum of 1 ms between any reset event and a
	 * register read/write transaction.
	 */
	fsleep(AD4170_RESET_SLEEP_US);

	return 0;
}

static void ad4170_clk_disable_unprepare(void *clk)
{
	clk_disable_unprepare(clk);
}

static int ad4170_parse_pin_muxing(struct iio_dev *indio_dev)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct device *dev = &st->spi->dev;
	u32 pin_muxing;
	u8 aux;
	int ret;

	/*
	 * Optional adi,dig-aux1 defaults to 0, DIG_AUX1 pin disabled.
	 */
	aux = AD4170_DIG_AUX1_DISABLED;
	ret = device_property_read_u8(dev, "adi,dig-aux1", &aux);
	if (ret) {
		if (aux < AD4170_DIG_AUX1_DISABLED || aux > AD4170_DIG_AUX1_SYNC)
			return dev_err_probe(dev, -EINVAL,
					     "Invalid adi,dig-aux1: %u\n", aux);
	}
	pin_muxing |= FIELD_PREP(AD4170_PIN_MUXING_DIG_AUX1_CTRL_MSK, aux);

	/*
	 * Optional adi,dig-aux2 defaults to 0, DIG_AUX2 pin disabled.
	 */
	aux = AD4170_DIG_AUX2_DISABLED;
	ret = device_property_read_u8(dev, "adi,dig-aux2", &aux);
	if (ret) {
		if (aux < AD4170_DIG_AUX2_DISABLED || aux > AD4170_DIG_AUX2_SYNC)
			return dev_err_probe(dev, -EINVAL,
					     "Invalid adi,dig-aux2: %u\n", aux);
	}
	pin_muxing |= FIELD_PREP(AD4170_PIN_MUXING_DIG_AUX2_CTRL_MSK, aux);

	/*
	 * Optional adi,sync-option defaults to 1, standard sync functionality.
	 */
	aux = AD4170_SYNC_STANDARD;
	ret = device_property_read_u8(dev, "adi,sync-option", &aux);
	if (ret) {
		if (aux < AD4170_SYNC_DISABLED || aux > AD4170_SYNC_ALTERNATE)
			return dev_err_probe(dev, -EINVAL,
					     "Invalid adi,sync-option: %u\n", aux);
	}
	pin_muxing |= FIELD_PREP(AD4170_PIN_MUXING_SYNC_CTRL_MSK, aux);

	return regmap_write(st->regmap, AD4170_PIN_MUXING_REG, pin_muxing);
}

static int ad4170_parse_setup(struct ad4170_state *st,
			      struct fwnode_handle *child,
			      struct ad4170_setup *setup)
{
	struct device *dev = &st->spi->dev;
	u32 tmp;
	u16 aux16;
	u8 aux;
	int ret;

	/* ADC input chopping setup */
	aux16 = AD4170_MISC_CHOP_ADC_OFF;
	ret = fwnode_property_read_u16(child, "adi,chop-adc", &aux16);
	if (!ret) {
		ret = ad4170_find_table_index(ad4170_chop_adc_tbl, aux16);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "Invalid ADC chop config: %u\n",
					     aux16);
	}
	setup->misc |= FIELD_PREP(AD4170_MISC_CHOP_ADC_MSK, aux16);

	/* Burnout current setup */
	tmp = AD4170_BURNOUT_OFF;
	fwnode_property_read_u32(child, "adi,burnout-current-nanoamp", &tmp);
	if (!ret) {
		ret = ad4170_find_table_index(ad4170_burnout_current_na_tbl, tmp);
		return dev_err_probe(dev, ret,
				     "Invalid burnout current %unA\n", tmp);
	}
	setup->misc |= FIELD_PREP(AD4170_MISC_BURNOUT_MSK, tmp);

	/* Positive reference buffer setup */
	tmp = AD4170_REF_BUF_PRE; /* Default to have precharge buffer enabled. */
	ret = fwnode_property_read_u32(child, "adi,buffered-positive", &tmp);
	if (ret) {
		if (tmp < AD4170_REF_BUF_PRE || tmp > AD4170_REF_BUF_BYPASS)
			return dev_err_probe(dev, -EINVAL,
					     "Invalid adi,buffered-positive: %u\n",
					     tmp);
	}
	setup->afe |= FIELD_PREP(AD4170_AFE_REF_BUF_P_MSK, tmp);

	/* Negative reference buffer setup */
	tmp = AD4170_REF_BUF_PRE; /* Default to have precharge buffer enabled. */
	ret = fwnode_property_read_u32(child, "adi,buffered-negative", &tmp);
	if (ret) {
		if (tmp < AD4170_REF_BUF_PRE || tmp > AD4170_REF_BUF_BYPASS)
			return dev_err_probe(dev, -EINVAL,
					     "Invalid adi,buffered-negative: %u\n",
					     tmp);
	}
	setup->afe |= FIELD_PREP(AD4170_AFE_REF_BUF_M_MSK, tmp);

	/* Voltage reference selection */
	aux = AD4170_AFE_REFIN_REFOUT; /* Default reference selection. */
	fwnode_property_read_u8(child, "adi,reference-select", &aux);
	if (aux >= AD4170_AFE_REFIN_AVDD)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid reference selected %u\n", aux);
	setup->afe |= FIELD_PREP(AD4170_AFE_REF_SELECT_MSK, aux);

	return 0;
}

static int ad4170_parse_channel_type(struct device *dev,
				     struct fwnode_handle *child,
				     struct iio_chan_spec *chan)
{
	u32 pins[2];
	int ret;

	ret = fwnode_property_read_u32_array(child, "diff-channels", pins,
					     ARRAY_SIZE(pins));
	if (!ret) {
		chan->differential = true;
		chan->channel = pins[0];
		chan->channel2 = pins[1];
		return 0;
	}
	ret = fwnode_property_read_u32(child, "single-channel", &pins[0]);
	if (!ret) {
		chan->differential = false;
		chan->channel = pins[0];

		ret = fwnode_property_read_u32(child, "common-mode-channel",
					       &pins[1]);
		if (ret)
			return dev_err_probe(dev, ret,
				"single-ended channels must define common-mode-channel\n");

		chan->channel2 = pins[1];
		return 0;
	}
	return dev_err_probe(dev, ret,
		"Channel must define one of diff-channels or single-channel.\n");
}

static int ad4170_parse_channel_node(struct iio_dev *indio_dev,
				     struct fwnode_handle *child, int chan_num)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	unsigned int index;
	struct device *dev = &st->spi->dev;
	struct ad4170_chan_info *chan_info;
	struct ad4170_setup *setup;
	struct iio_chan_spec *chan;
	u8 ref_select;
	bool bipolar;
	int ret;

	ret = fwnode_property_read_u32(child, "reg", &index);
	if (ret)
		return ret;

	if (index >= AD4170_MAX_CHANNELS)
		return dev_err_probe(dev, -EINVAL,
				     "Channel idx greater than no of channels\n");

	chan = &st->chans[chan_num];
	chan_info = &st->chans_info[chan_num];

	*chan = ad4170_channel_template;
	chan->address = chan_num; /* Channel index in ad4170_state chans array */
	chan->scan_index = index; /* Index to use with the sequencer */

	chan_info->setup_num = AD4170_INVALID_SETUP_SLOT;
	ret = fwnode_property_read_u32(child, "adi,config-setup-number",
				       &chan_info->setup_num);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to read adi,config-setup-number\n");

	if (chan_info->setup_num >= AD4170_MAX_SETUPS)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid adi,config-setup-number: %d\n",
				     chan_info->setup_num);

	setup = &st->setups[chan_info->setup_num];
	ret = ad4170_parse_setup(st, child, setup);
	if (ret)
		return ret;

	ret = ad4170_parse_channel_type(dev, child, chan);
	if (ret < 0)
		return ret;

	bipolar = fwnode_property_read_bool(child, "bipolar");
	setup->afe |= FIELD_PREP(AD4170_AFE_BIPOLAR_MSK, bipolar);
	if (bipolar)
		chan->scan_type.sign = 's';
	else
		chan->scan_type.sign = 'u';

	ref_select = FIELD_GET(AD4170_AFE_REF_SELECT_MSK, setup->afe);
	ret = ad4170_validate_channel(st, chan);
	if (ret < 0)
		return ret;

	ret = ad4170_get_input_range(st, chan, ref_select);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Cannot use reference %u\n",
				     ref_select);

	chan_info->input_range_uv = ret;
	return 0;
}

static int ad4170_parse_channels(struct iio_dev *indio_dev)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct device *dev = &st->spi->dev;
	int chan_num, i, ret;

	chan_num = 0;
	device_for_each_child_node_scoped(dev, child) {
		ret = ad4170_parse_channel_node(indio_dev, child, chan_num);
		if (ret)
			return ret;

		chan_num++;
	}
	for (i = 0; i < AD4170_MAX_CHANNELS; i++)
		if (st->chans[i].scan_index == 0)
			break;

	/*
	 * When more than one channel is enabled, channel 0 must always be used.
	 */
	if (chan_num > 1 && i == AD4170_MAX_CHANNELS)
		return dev_err_probe(dev, -EINVAL,
				     "Channel 0 must be configured\n");

	indio_dev->channels = st->chans;
	indio_dev->num_channels = chan_num;
	return 0;
}

/*
 * Parses firmware data describing output current source setup. There are 4
 * excitation currents (IOUT0 to IOUT3) that can be configured independently.
 * Excitation currents are added if they are output on the same pin.
 */
static int ad4170_parse_exc_current(struct iio_dev *indio_dev)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct device *dev = &st->spi->dev;
	int gpio_number;
	u16 current_src;
	u32 pin, val;
	int i, ret;

	for (i = 0; i < AD4170_NUM_CURRENT_SRC; i++) {
		/* Parse excitation current output pin properties. */
		pin = AD4170_CURRENT_IOUT_AIN0;
		ret = fwnode_property_read_u32(dev->fwnode,
					       ad4170_i_out_pin_dt_props[i],
					       &pin);
		if (!ret) {
			ret = ad4170_find_table_index(ad4170_iout_pin_tbl, pin);
			if (ret < 0)
				return dev_err_probe(dev, ret,
						     "Invalid %s: %u\n",
						     ad4170_i_out_pin_dt_props[i],
						     pin);
		}
		current_src |= FIELD_PREP(AD4170_CURRENT_SRC_I_OUT_PIN_MSK, pin);

		/* Parse excitation current value properties. */
		val = AD4170_I_OUT_0UA;
		ret = fwnode_property_read_u32(dev->fwnode,
					       ad4170_i_out_val_dt_props[i],
					       &val);
		if (!ret) {
			ret = ad4170_find_table_index(ad4170_iout_current_ua_tbl,
						      val);
			if (ret < 0)
				return dev_err_probe(dev, ret,
						     "Invalid %s: %uuA\n",
						     ad4170_i_out_val_dt_props[i],
						     val);
		}
		current_src |= FIELD_PREP(AD4170_CURRENT_SRC_I_OUT_VAL_MSK, val);

		/*
		 * The excitation currents can be added by outputting them on
		 * the same pin so don't prevent a pin from being assigned for
		 * current output multiple times.
		 */
		if (pin >= AD4170_CURRENT_IOUT_GPIO0) {
			gpio_number = pin - AD4170_CURRENT_IOUT_GPIO0;
			st->gpio_fn[gpio_number] = AD4170_GPIO_AC_EXCITATION;
		} else if (val > AD4170_I_OUT_0UA) {
			st->pins_fn[pin] = AD4170_PIN_CURRENT_OUT;
		}

		ret = regmap_write(st->regmap, AD4170_CURRENT_SRC_REG(i),
				   current_src);
		if (ret < 0)
			return ret;

		current_src = 0; /* Clear temporary variable. */
	}
	return 0;
}

static void ad4170_disable_supplies(void *data)
{
	struct ad4170_state *st = data;

	regulator_bulk_disable(ARRAY_SIZE(st->supplies), st->supplies);
}

static struct ad4170_state *clk_hw_to_ad4170(struct clk_hw *hw)
{
	return container_of(hw, struct ad4170_state, int_clk_hw);
}

static unsigned long ad4170_sel_clk(struct ad4170_state *st,
				    unsigned int clk_sel)
{
	st->clock_ctrl &= ~AD4170_CLOCK_CTRL_CLOCKSEL_MSK;
	st->clock_ctrl |= FIELD_PREP(AD4170_CLOCK_CTRL_CLOCKSEL_MSK, clk_sel);
	return regmap_write(st->regmap, AD4170_CLOCK_CTRL_REG, st->clock_ctrl);
}

static unsigned long ad4170_clk_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	return AD4170_INT_FREQ_16MHZ;
}

static int ad4170_clk_output_is_enabled(struct clk_hw *hw)
{
	struct ad4170_state *st = clk_hw_to_ad4170(hw);
	u32 clk_sel;

	clk_sel = FIELD_GET(AD4170_CLOCK_CTRL_CLOCKSEL_MSK, st->clock_ctrl);
	return clk_sel == AD4170_INTERNAL_OSC_OUTPUT;
}

static int ad4170_clk_output_prepare(struct clk_hw *hw)
{
	struct ad4170_state *st = clk_hw_to_ad4170(hw);

	return ad4170_sel_clk(st, AD4170_INTERNAL_OSC_OUTPUT);
}

static void ad4170_clk_output_unprepare(struct clk_hw *hw)
{
	struct ad4170_state *st = clk_hw_to_ad4170(hw);

	ad4170_sel_clk(st, AD4170_INTERNAL_OSC);
}

static const struct clk_ops ad4170_int_clk_ops = {
	.recalc_rate = ad4170_clk_recalc_rate,
	.is_enabled = ad4170_clk_output_is_enabled,
	.prepare = ad4170_clk_output_prepare,
	.unprepare = ad4170_clk_output_unprepare,
};

static int ad4170_register_clk_provider(struct iio_dev *indio_dev)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct device *dev = indio_dev->dev.parent;
	struct fwnode_handle *fwnode = dev_fwnode(dev);
	struct clk_init_data init = {};
	int ret;

	if (!IS_ENABLED(CONFIG_COMMON_CLK))
		return 0;

	init.name = fwnode_get_name(fwnode);
	init.ops = &ad4170_int_clk_ops;

	st->int_clk_hw.init = &init;
	ret = devm_clk_hw_register(dev, &st->int_clk_hw);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_simple_get,
					   &st->int_clk_hw);
}

static int ad4170_clock_select(struct iio_dev *indio_dev)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct device *dev = &st->spi->dev;
	int ret;

	st->mclk_hz = AD4170_INT_FREQ_16MHZ;
	ret = device_property_match_property_string(dev, "clock-names",
						    ad4170_clk_sel,
						    ARRAY_SIZE(ad4170_clk_sel));
	if (ret < 0) {
		/* Use internal clock reference */
		st->clock_ctrl |= FIELD_PREP(AD4170_CLOCK_CTRL_CLOCKSEL_MSK,
					     AD4170_INTERNAL_OSC_OUTPUT);
		return ad4170_register_clk_provider(indio_dev);
	}

	/* Use external clock reference */
	st->ext_clk = devm_clk_get(dev, ad4170_clk_sel[ret]);
	if (IS_ERR(st->ext_clk)) {
		if (PTR_ERR(st->ext_clk) == -EPROBE_DEFER)
			return -EPROBE_DEFER;

		return dev_err_probe(dev, PTR_ERR(st->ext_clk),
				     "Failed to get external clock\n");
	}
	st->clock_ctrl |= FIELD_PREP(AD4170_CLOCK_CTRL_CLOCKSEL_MSK,
				     AD4170_EXTERNAL_OSC + ret);

	ret = clk_prepare_enable(st->ext_clk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to enable external clock\n");

	ret = devm_add_action_or_reset(dev, ad4170_clk_disable_unprepare,
				       st->ext_clk);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to add clock unwind action\n");

	st->mclk_hz = clk_get_rate(st->ext_clk);
	if (st->mclk_hz < AD4170_EXT_FREQ_MHZ_MIN ||
	    st->mclk_hz > AD4170_EXT_FREQ_MHZ_MAX) {
		return dev_err_probe(dev, -EINVAL,
				     "Invalid external clock frequency %u\n",
				     st->mclk_hz);
	}
	return 0;
}

static int ad4170_parse_pw_switch(struct ad4170_state *st, struct device *dev)
{
	bool pdsw0, pdsw1;

	pdsw0 = device_property_read_bool(dev, "adi,gpio0-power-down-switch");
	pdsw1 = device_property_read_bool(dev, "adi,gpio1-power-down-switch");

	/*
	 * Error if a GPIO is assigned to be both excitation current output and
	 * power switch.
	 */
	if (pdsw0) {
		if (st->gpio_fn[0] != AD4170_GPIO_UNASIGNED)
			return dev_err_probe(&st->spi->dev, -EINVAL,
					     "GPIO 0 already used with fn %u\n",
					     st->gpio_fn[0]);

		st->gpio_fn[0] = AD4170_GPIO_PW_DOW_SWITCH;
	}
	if (pdsw1) {
		if (st->gpio_fn[1] != AD4170_GPIO_UNASIGNED)
			return dev_err_probe(&st->spi->dev, -EINVAL,
					     "GPIO 1 already used with fn %u\n",
					     st->gpio_fn[1]);

		st->gpio_fn[1] = AD4170_GPIO_PW_DOW_SWITCH;
	}
	return regmap_update_bits(st->regmap, AD4170_POWER_DOWN_SW_REG,
				 AD4170_POWER_DOWN_SW_PDSW0_MSK |
				 AD4170_POWER_DOWN_SW_PDSW1_MSK,
				 pdsw0 | pdsw1);
}

static int ad4170_parse_vbias(struct ad4170_state *st, struct device *dev)
{
	u32 vbias_pins[AD4170_MAX_ANALOG_PINS];
	unsigned int i, val;
	u32 num_vbias_pins;
	int ret;

	ret = device_property_count_u32(dev, "adi,vbias-pins");
	if (ret > 0) {
		if (ret > AD4170_MAX_ANALOG_PINS)
			return dev_err_probe(dev, -EINVAL,
					     "Too many vbias pins %u\n", ret);

		num_vbias_pins = ret;
		ret = device_property_read_u32_array(dev, "adi,vbias-pins",
						     vbias_pins,
						     num_vbias_pins);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to read vbias pins\n");
	}
	for (i = 0; i < num_vbias_pins; i++) {
		if (st->pins_fn[vbias_pins[i]] != AD4170_PIN_UNASIGNED)
			return dev_err_probe(&st->spi->dev, -EINVAL,
					     "AIN%d already used with fn %u\n",
					     vbias_pins[i], st->pins_fn[i]);

		val |= BIT(vbias_pins[i]);
		st->pins_fn[vbias_pins[i]] = AD4170_PIN_VBIAS;
	}
	return regmap_write(st->regmap, AD4170_V_BIAS_REG, val);
}

static int ad4170_parse_firmware(struct iio_dev *indio_dev)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct device *dev = &st->spi->dev;
	int ret, i;
	u8 aux;

	ret = ad4170_clock_select(indio_dev);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to setup device clock\n");

	ret = regmap_write(st->regmap, AD4170_CLOCK_CTRL_REG, st->clock_ctrl);
	if (ret < 0)
		return ret;

	ret = ad4170_parse_pin_muxing(indio_dev);
	if (ret < 0)
		return ret;

	for (i = 0; i < AD4170_NUM_ANALOG_PINS; i++)
		st->pins_fn[i] = AD4170_PIN_UNASIGNED;

	for (i = 0; i < AD4170_NUM_GPIO_PINS; i++)
		st->gpio_fn[i] = AD4170_GPIO_UNASIGNED;

	ret = ad4170_parse_exc_current(indio_dev);
	if (ret)
		return ret;
// write to misc reg must be delaied because misc register contains settings
// that are channel specific.

	ret = ad4170_parse_pw_switch(st, dev);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "Failed to config power down switches\n");

	ret = ad4170_parse_vbias(st, dev);
	if (ret < 0)
		return ret;

	ret = ad4170_parse_channels(indio_dev);
	if (ret)
		return ret;

	aux = AD4170_MISC_CHOP_IEXC_OFF;
	ret = device_property_read_u8(dev, "adi,chop-iexc", &aux);
	if (!ret) {
		ret = ad4170_find_table_index(ad4170_iexc_chop_tbl, aux);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "Invalid adi,chop-iexc config: %u\n",
					      aux);
	}

	/* Set excitation current chop config to first channel setup config */
	st->setups[indio_dev->channels[0].address].misc |=
		FIELD_PREP(AD4170_MISC_CHOP_IEXC_MSK, aux);
	return 0;
}

static int ad4170_initial_config(struct iio_dev *indio_dev)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	struct ad4170_chan_info *chan_info;
	struct iio_chan_spec const *chan;
	struct ad4170_setup *setup;
	unsigned int val;
	unsigned int i;
	int ret;

	ad4170_fill_sps_tbl(st);

	/* Put ADC in IDLE mode */
	ret = ad4170_set_mode(st, AD4170_ADC_CTRL_MODE_IDLE);
	if (ret)
		return ret;

	/* Setup channels. */
	for (i = 0; i < indio_dev->num_channels; i++) {
		chan = &indio_dev->channels[i];
		chan_info = &st->chans_info[chan->address];
		setup = &st->setups[chan_info->setup_num];

		ret = regmap_update_bits(st->regmap,
					 AD4170_CHAN_SETUP_REG(chan->address),
					 AD4170_CHANNEL_SETUP_SETUP_MSK,
					 FIELD_PREP(AD4170_CHANNEL_SETUP_SETUP_MSK,
						    chan_info->setup_num));
		if (ret)
			return ret;

		setup->gain = AD4170_GAIN_REG_DEFAULT;
		ret = ad4170_write_setup(st, chan_info->setup_num, setup);
		if (ret)
			return ret;

		val = FIELD_PREP(AD4170_CHANNEL_MAPN_AINP_MSK, chan->channel) |
		      FIELD_PREP(AD4170_CHANNEL_MAPN_AINM_MSK, chan->channel2);

		ret = regmap_write(st->regmap, AD4170_CHAN_MAP_REG(i), val);
		if (ret < 0)
			return ret;

		ret = ad4170_set_channel_freq(st, chan->address,
					      AD4170_MAX_SAMP_RATE, 0);
		if (ret)
			return ret;

		ret = ad4170_fill_scale_tbl(indio_dev, i);
		if (ret)
			return ret;
	}

	ret = regmap_write(st->regmap, AD4170_CHANNEL_EN_REG, 0);
	if (ret < 0)
		return ret;

	/*
	 * Configure channels to share the same data output register, i.e. data
	 * can be read from the same register address regardless of channel
	 * number.
	 */
	ret = regmap_update_bits(st->regmap, AD4170_ADC_CTRL_REG,
				 AD4170_ADC_CTRL_MULTI_DATA_REG_SEL_MSK,
				 AD4170_ADC_CTRL_MULTI_DATA_REG_SEL_MSK);
	if (ret)
		return ret;

	return 0;
}

static const struct iio_trigger_ops ad4170_trigger_ops = {
	.validate_device = iio_trigger_validate_own_device,
};

static irqreturn_t ad4170_interrupt(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = dev_id;
	struct ad4170_state *st = iio_priv(indio_dev);

	if (iio_buffer_enabled(indio_dev))
		iio_trigger_poll(st->trig);
	else
		complete(&st->completion);

	return IRQ_HANDLED;
};

static void ad4170_prepare_message(struct ad4170_state *st)
{
	/*
	 * Continuous data register read is enabled on buffer postenable so
	 * no instruction phase is needed meaning we don't need to send the
	 * register address to read data. Transfer only needs the read buffer.
	 */
	st->xfer.rx_buf = &st->reg_read_rx_buf;
	st->xfer.bits_per_word = ad4170_channel_template.scan_type.storagebits;
	st->xfer.len = BITS_TO_BYTES(ad4170_channel_template.scan_type.storagebits);

	spi_message_init_with_transfers(&st->msg, &st->xfer, 1);
}

static int ad4170_buffer_postenable(struct iio_dev *indio_dev)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	int ret;

	ret = ad4170_set_mode(st, AD4170_ADC_CTRL_MODE_CONT);
	if (ret < 0)
		return ret;

	ret = regmap_update_bits(st->regmap, AD4170_ADC_CTRL_REG,
				 AD4170_REG_CTRL_CONT_READ_MSK,
				 FIELD_PREP(AD4170_REG_CTRL_CONT_READ_MSK,
					    AD4170_ADC_CTRL_CONT_READ_ENABLE));

	if (ret < 0)
		return ret;

	return spi_optimize_message(st->spi, &st->msg);
}

static int ad4170_buffer_predisable(struct iio_dev *indio_dev)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	int ret, i;

	spi_unoptimize_message(&st->msg);

	for (i = 0; i < indio_dev->num_channels; i++) {
		ret = ad4170_set_channel_enable(st, i, false);
		if (ret)
			return ret;
	}

	ret = regmap_update_bits(st->regmap, AD4170_ADC_CTRL_REG,
				 AD4170_REG_CTRL_CONT_READ_MSK,
				 FIELD_PREP(AD4170_REG_CTRL_CONT_READ_MSK,
					    AD4170_ADC_CTRL_CONT_READ_DISABLE));
	if (ret < 0)
		return ret;

	return ad4170_set_mode(st, AD4170_ADC_CTRL_MODE_IDLE);
}

static const struct iio_buffer_setup_ops ad4170_buffer_ops = {
	.postenable = ad4170_buffer_postenable,
	.predisable = ad4170_buffer_predisable,
};

static irqreturn_t ad4170_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ad4170_state *st = iio_priv(indio_dev);
	int ret, i = 0;
	int scan_index;

	for_each_set_bit(scan_index, indio_dev->active_scan_mask,
			 indio_dev->masklength) {
		/* Read register data */
		ret = regmap_read(st->regmap, AD4170_DATA_24b_REG, &st->data[i]);
		if (ret)
			goto out;
		i++;
	}

	iio_push_to_buffers_with_timestamp(indio_dev, &st->data,
					   iio_get_time_ns(indio_dev));
out:
	iio_trigger_notify_done(indio_dev->trig);
	return IRQ_HANDLED;
}

static int ad4170_triggered_buffer_setup(struct iio_dev *indio_dev)
{
	struct ad4170_state *st = iio_priv(indio_dev);
	int ret;

	indio_dev->modes |= INDIO_BUFFER_TRIGGERED;

	st->trig = devm_iio_trigger_alloc(indio_dev->dev.parent, "%s-trig%d",
					  indio_dev->name,
					  iio_device_id(indio_dev));
	if (!st->trig)
		return -ENOMEM;

	st->trig->ops = &ad4170_trigger_ops;
	st->trig->dev.parent = indio_dev->dev.parent;

	iio_trigger_set_drvdata(st->trig, indio_dev);
	ret = devm_iio_trigger_register(indio_dev->dev.parent, st->trig);
	if (ret)
		return ret;

	indio_dev->trig = iio_trigger_get(st->trig);

	ret = request_irq(st->spi->irq,
			  &ad4170_interrupt,
			  IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			  indio_dev->name, indio_dev);
	if (ret)
		return ret;

	return devm_iio_triggered_buffer_setup(indio_dev->dev.parent, indio_dev,
					       &iio_pollfunc_store_time,
					       &ad4170_trigger_handler,
					       &ad4170_buffer_ops);
}

static int ad4170_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev;
	struct ad4170_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	devm_mutex_init(dev, &st->lock);

	indio_dev->name = AD4170_NAME;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ad4170_info;

	st->spi = spi;
	st->regmap = devm_regmap_init(dev, NULL, st, &ad4170_regmap_config);

	st->supplies[AD4170_AVDD_SUP].supply = "avdd";
	st->supplies[AD4170_AVSS_SUP].supply = "avss";
	st->supplies[AD4170_IOVDD_SUP].supply = "iovdd";
	st->supplies[AD4170_REFIN1P_SUP].supply = "refin1p";
	st->supplies[AD4170_REFIN1N_SUP].supply = "refin1n";
	st->supplies[AD4170_REFIN2P_SUP].supply = "refin2p";
	st->supplies[AD4170_REFIN2N_SUP].supply = "refin2n";

	/*
	 * If a regulator is not available, it will be set to a dummy regulator.
	 * Each channel reference is checked with regulator_get_voltage() before
	 * setting attributes so if any channel uses a dummy supply the driver
	 * probe will fail.
	 */
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(st->supplies),
				      st->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get supplies\n");

	ret = regulator_bulk_enable(ARRAY_SIZE(st->supplies), st->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable supplies\n");

	ret = devm_add_action_or_reset(dev, ad4170_disable_supplies, st);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to add supplies disable action\n");

	ret = ad4170_soft_reset(st);
	if (ret)
		return ret;

	ret = ad4170_parse_firmware(indio_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to parse firmware\n");

	ret = ad4170_initial_config(indio_dev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to setup device\n");

	init_completion(&st->completion);

	ret = ad4170_triggered_buffer_setup(indio_dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to setup read buffer\n");

	ad4170_prepare_message(st);

	return devm_iio_device_register(dev, indio_dev);
}

static const struct of_device_id ad4170_of_match[] = {
	{
		.compatible = "adi,ad4170",
	},
	{ }
};
MODULE_DEVICE_TABLE(of, ad4170_of_match);

static struct spi_driver ad4170_driver = {
	.driver = {
		.name = AD4170_NAME,
		.of_match_table = ad4170_of_match,
	},
	.probe = ad4170_probe,
};
module_spi_driver(ad4170_driver);

MODULE_AUTHOR("Ana-Maria Cusco <ana-maria.cusco@analog.com>");
MODULE_AUTHOR("Marcelo Schmitt <marcelo.schmitt@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD4170 SPI driver");
MODULE_LICENSE("GPL");
