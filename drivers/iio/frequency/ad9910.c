// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD9910 SPI DDS (Direct Digital Synthesizer) driver
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/log2.h>
#include <linux/math64.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/spi/spi.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/units.h>
#include <linux/unaligned.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

/* Register addresses */
#define AD9910_REG_CFR1			0x00
#define AD9910_REG_CFR2			0x01
#define AD9910_REG_CFR3			0x02
#define AD9910_REG_AUX_DAC		0x03
#define AD9910_REG_IO_UPDATE_RATE	0x04
#define AD9910_REG_FTW			0x07
#define AD9910_REG_POW			0x08
#define AD9910_REG_ASF			0x09
#define AD9910_REG_MULTICHIP_SYNC	0x0A
#define AD9910_REG_DRG_LIMIT		0x0B
#define AD9910_REG_DRG_STEP		0x0C
#define AD9910_REG_DRG_RATE		0x0D
#define AD9910_REG_PROFILE0		0x0E
#define AD9910_REG_PROFILE1		0x0F
#define AD9910_REG_PROFILE2		0x10
#define AD9910_REG_PROFILE3		0x11
#define AD9910_REG_PROFILE4		0x12
#define AD9910_REG_PROFILE5		0x13
#define AD9910_REG_PROFILE6		0x14
#define AD9910_REG_PROFILE7		0x15
#define AD9910_REG_RAM			0x16

#define AD9910_REG_NUM_CACHED		0x16

#define AD9910_REG_PROFILE(x)		(AD9910_REG_PROFILE0 + (x))
#define AD9910_REG_HIGH32_FLAG		0x100
#define AD9910_REG_HIGH32_MSK		GENMASK_ULL(63, 32)
#define AD9910_REG_LOW32_MSK		GENMASK_ULL(31, 0)

/* CFR1 bit definitions */
#define AD9910_CFR1_RAM_ENABLE_MSK		BIT(31)
#define AD9910_CFR1_RAM_PLAYBACK_DEST_MSK	GENMASK(30, 29)
#define AD9910_CFR1_OSK_MANUAL_EXT_CTL_MSK	BIT(23)
#define AD9910_CFR1_INV_SINC_EN_MSK		BIT(22)
#define AD9910_CFR1_INT_PROFILE_CTL_MSK		GENMASK(20, 17)
#define AD9910_CFR1_SELECT_SINE_MSK		BIT(16)
#define AD9910_CFR1_LOAD_LRR_IO_UPDATE_MSK	BIT(15)
#define AD9910_CFR1_AUTOCLR_DIG_RAMP_ACCUM_MSK	BIT(14)
#define AD9910_CFR1_AUTOCLR_PHASE_ACCUM_MSK	BIT(13)
#define AD9910_CFR1_CLEAR_DIG_RAMP_ACCUM_MSK	BIT(12)
#define AD9910_CFR1_CLEAR_PHASE_ACCUM_MSK	BIT(11)
#define AD9910_CFR1_LOAD_ARR_IO_UPDATE_MSK	BIT(10)
#define AD9910_CFR1_OSK_ENABLE_MSK		BIT(9)
#define AD9910_CFR1_SELECT_AUTO_OSK_MSK		BIT(8)
#define AD9910_CFR1_DIGITAL_POWER_DOWN_MSK	BIT(7)
#define AD9910_CFR1_DAC_POWER_DOWN_MSK		BIT(6)
#define AD9910_CFR1_REFCLK_INPUT_POWER_DOWN_MSK	BIT(5)
#define AD9910_CFR1_AUX_DAC_POWER_DOWN_MSK	BIT(4)
#define AD9910_CFR1_SOFT_POWER_DOWN_MSK		GENMASK(7, 4)
#define AD9910_CFR1_EXT_POWER_DOWN_CTL_MSK	BIT(3)
#define AD9910_CFR1_SDIO_INPUT_ONLY_MSK		BIT(1)
#define AD9910_CFR1_LSB_FIRST_MSK		BIT(0)

/* CFR2 bit definitions */
#define AD9910_CFR2_AMP_SCALE_SINGLE_TONE_MSK	BIT(24)
#define AD9910_CFR2_INTERNAL_IO_UPDATE_MSK	BIT(23)
#define AD9910_CFR2_SYNC_CLK_EN_MSK		BIT(22)
#define AD9910_CFR2_DRG_DEST_MSK		GENMASK(21, 20)
#define AD9910_CFR2_DRG_ENABLE_MSK		BIT(19)
#define AD9910_CFR2_DRG_NO_DWELL_HIGH_MSK	BIT(18)
#define AD9910_CFR2_DRG_NO_DWELL_LOW_MSK	BIT(17)
#define AD9910_CFR2_DRG_NO_DWELL_MSK		GENMASK(18, 17)
#define AD9910_CFR2_READ_EFFECTIVE_FTW_MSK	BIT(16)
#define AD9910_CFR2_IO_UPDATE_RATE_CTL_MSK	GENMASK(15, 14)
#define AD9910_CFR2_PDCLK_ENABLE_MSK		BIT(11)
#define AD9910_CFR2_PDCLK_INVERT_MSK		BIT(10)
#define AD9910_CFR2_TXENABLE_INVERT_MSK		BIT(9)
#define AD9910_CFR2_MATCHED_LATENCY_EN_MSK	BIT(7)
#define AD9910_CFR2_DATA_ASM_HOLD_LAST_MSK	BIT(6)
#define AD9910_CFR2_SYNC_TIMING_VAL_DISABLE_MSK	BIT(5)
#define AD9910_CFR2_PARALLEL_DATA_PORT_EN_MSK	BIT(4)
#define AD9910_CFR2_FM_GAIN_MSK			GENMASK(3, 0)

/* CFR3 bit definitions */
#define AD9910_CFR3_OPEN_MSK			0x08070000
#define AD9910_CFR3_DRV0_MSK			GENMASK(29, 28)
#define AD9910_CFR3_VCO_SEL_MSK			GENMASK(26, 24)
#define AD9910_CFR3_ICP_MSK			GENMASK(21, 19)
#define AD9910_CFR3_REFCLK_DIV_BYPASS_MSK	BIT(15)
#define AD9910_CFR3_REFCLK_DIV_RESETB_MSK	BIT(14)
#define AD9910_CFR3_PFD_RESET_MSK		BIT(10)
#define AD9910_CFR3_PLL_EN_MSK			BIT(8)
#define AD9910_CFR3_N_MSK			GENMASK(7, 1)

/* Auxiliary DAC Control Register Bits */
#define AD9910_AUX_DAC_FSC_MSK			GENMASK(7, 0)

/* POW Register Bits */
#define AD9910_POW_PP_LSB_MSK			GENMASK(7, 0)

/* ASF Register Bits */
#define AD9910_ASF_RAMP_RATE_MSK		GENMASK(31, 16)
#define AD9910_ASF_SCALE_FACTOR_MSK		GENMASK(15, 2)
#define AD9910_ASF_SCALE_FACTOR_PP_LSB_MSK	GENMASK(7, 2)
#define AD9910_ASF_STEP_SIZE_MSK		GENMASK(1, 0)

/* Multichip Sync Register Bits */
#define AD9910_MC_SYNC_VALIDATION_DELAY_MSK	GENMASK(31, 28)
#define AD9910_MC_SYNC_RECEIVER_ENABLE_MSK	BIT(27)
#define AD9910_MC_SYNC_GENERATOR_ENABLE_MSK	BIT(26)
#define AD9910_MC_SYNC_GENERATOR_POLARITY_MSK	BIT(25)
#define AD9910_MC_SYNC_STATE_PRESET_MSK		GENMASK(23, 18)
#define AD9910_MC_SYNC_OUTPUT_DELAY_MSK		GENMASK(15, 11)
#define AD9910_MC_SYNC_INPUT_DELAY_MSK		GENMASK(7, 3)

/* Profile Register Format (Single Tone Mode) */
#define AD9910_PROFILE_ST_ASF_MSK		GENMASK_ULL(61, 48)
#define AD9910_PROFILE_ST_POW_MSK		GENMASK_ULL(47, 32)
#define AD9910_PROFILE_ST_FTW_MSK		AD9910_REG_LOW32_MSK

/* Device constants */
#define AD9910_PI_NANORAD		3141592653UL

#define AD9910_MAX_SYSCLK_HZ		(1000 * HZ_PER_MHZ)
#define AD9910_MAX_PHASE_MICRORAD	(AD9910_PI_NANORAD / 500)

#define AD9910_ASF_MAX			(BIT(14) - 1)
#define AD9910_ASF_PP_LSB_MAX		(BIT(6) - 1)
#define AD9910_POW_MAX			(BIT(16) - 1)
#define AD9910_POW_PP_LSB_MAX		(BIT(8) - 1)
#define AD9910_NUM_PROFILES		8

/* PLL constants */
#define AD9910_PLL_MIN_N		12
#define AD9910_PLL_MAX_N		127

#define AD9910_PLL_IN_MIN_FREQ_HZ	(3200 * HZ_PER_KHZ)
#define AD9910_PLL_IN_MAX_FREQ_HZ	(60 * HZ_PER_MHZ)

#define AD9910_PLL_OUT_MIN_FREQ_HZ	(420 * HZ_PER_MHZ)
#define AD9910_PLL_OUT_MAX_FREQ_HZ	(1000 * HZ_PER_MHZ)

#define AD9910_VCO0_RANGE_AUTO_MAX_HZ	(465 * HZ_PER_MHZ)
#define AD9910_VCO1_RANGE_AUTO_MAX_HZ	(545 * HZ_PER_MHZ)
#define AD9910_VCO2_RANGE_AUTO_MAX_HZ	(650 * HZ_PER_MHZ)
#define AD9910_VCO3_RANGE_AUTO_MAX_HZ	(790 * HZ_PER_MHZ)
#define AD9910_VCO4_RANGE_AUTO_MAX_HZ	(885 * HZ_PER_MHZ)
#define AD9910_VCO_RANGE_NUM		6

#define AD9910_REFCLK_OUT_DRV_DISABLED	0

#define AD9910_ICP_MIN_uA		212
#define AD9910_ICP_MAX_uA		387
#define AD9910_ICP_STEP_uA		25

#define AD9910_DAC_IOUT_MAX_uA		31590
#define AD9910_DAC_IOUT_DEFAULT_uA	20070
#define AD9910_DAC_IOUT_MIN_uA		8640

#define AD9910_REFDIV2_MIN_FREQ_HZ	(120 * HZ_PER_MHZ)
#define AD9910_REFDIV2_MAX_FREQ_HZ	(1900 * HZ_PER_MHZ)

#define AD9910_SPI_DATA_IDX		1
#define AD9910_SPI_DATA_LEN_MAX		sizeof(__be64)
#define AD9910_SPI_MESSAGE_LEN_MAX	(AD9910_SPI_DATA_IDX + AD9910_SPI_DATA_LEN_MAX)
#define AD9910_SPI_READ			BIT(7)
#define AD9910_SPI_ADDR_MASK		GENMASK(4, 0)

/**
 * enum ad9910_channel - AD9910 channel identifiers in priority order
 */
enum ad9910_channel {
	AD9910_CHANNEL_SINGLE_TONE,
	AD9910_CHANNEL_PARALLEL_PORT,
};

enum {
	AD9910_PROFILE,
	AD9910_POWERDOWN,
	AD9910_PP_FREQ_SCALE,
	AD9910_PP_FREQ_OFFSET,
	AD9910_PP_PHASE_OFFSET,
	AD9910_PP_AMP_OFFSET,
};

struct ad9910_data {
	u32 sysclk_freq_hz;
	u32 dac_output_current;

	/* PLL configuration */
	u16 pll_charge_pump_current;
	u8 pll_multiplier;
	u8 pll_vco_range;

	bool ref_div2_en;
	u8 refclk_out_drv;

	/* Feature flags */
	bool inverse_sinc_enable;
	bool sine_output_enable;
	bool sync_clk_enable;
	bool pdclk_enable;
	bool pdclk_invert;
	bool tx_enable_invert;
};

struct ad9910_state {
	struct spi_device *spi;
	struct clk *refclk;

	struct gpio_desc *gpio_pwdown;
	struct gpio_desc *gpio_update;
	struct gpio_descs *gpio_profile;

	/* cached registers */
	union {
		u64 val64;
		u32 val32;
		u16 val16;
	} reg[AD9910_REG_NUM_CACHED];

	/*
	 * Lock for accessing device registers and state variables.
	 */
	struct mutex lock;

	struct ad9910_data data;
	u8 profile;

	/*
	 * DMA (thus cache coherency maintenance) requires the transfer
	 * buffers to live in their own cache lines.
	 */
	u8 buf[AD9910_SPI_MESSAGE_LEN_MAX] __aligned(IIO_DMA_MINALIGN);
};

static const char * const ad9910_power_supplies[] = {
	"dvdd-io33", "avdd33", "dvdd18", "avdd18",
};

static const char * const ad9910_refclk_out_drv0[] = {
	"disabled", "low", "medium", "high",
};

/**
 * ad9910_rational_scale() - Perform scaling of input given a reference.
 * @input: The input value to be scaled.
 * @scale: The numerator of the scaling factor.
 * @reference: The denominator of the scaling factor.
 *
 * Closest rounding with mul_u64_add_u64_div_u64
 *
 * Return: The scaled value.
 */
#define ad9910_rational_scale(input, scale, reference) ({	\
	u64 _tmp = (reference);					\
	mul_u64_add_u64_div_u64(input, scale, _tmp >> 1, _tmp);	\
})

static int ad9910_io_update(struct ad9910_state *st)
{
	if (st->gpio_update) {
		gpiod_set_value_cansleep(st->gpio_update, 1);
		udelay(1);
		gpiod_set_value_cansleep(st->gpio_update, 0);
	}

	return 0;
}

static inline int ad9910_spi_read(struct ad9910_state *st, u8 reg, size_t len)
{
	st->buf[0] = AD9910_SPI_READ | (reg & AD9910_SPI_ADDR_MASK);
	return spi_write_then_read(st->spi, &st->buf[0], 1,
				   &st->buf[AD9910_SPI_DATA_IDX], len);
}

static inline int ad9910_spi_write(struct ad9910_state *st, u8 reg, size_t len,
				   bool update)
{
	int ret;

	st->buf[0] = reg & AD9910_SPI_ADDR_MASK;
	ret = spi_write(st->spi, st->buf, AD9910_SPI_DATA_IDX + len);
	if (!ret && update)
		return ad9910_io_update(st);

	return ret;
}

#define AD9910_REG_READ_FN(nb)						\
static inline int ad9910_reg##nb##_read(struct ad9910_state *st,	\
					u8 reg, u##nb * data)		\
{									\
	int ret;							\
									\
	ret = ad9910_spi_read(st, reg, sizeof(*data));			\
	if (ret)							\
		return ret;						\
									\
	*data = get_unaligned_be##nb(&st->buf[AD9910_SPI_DATA_IDX]);	\
	return ret;							\
}

AD9910_REG_READ_FN(16)
AD9910_REG_READ_FN(32)
AD9910_REG_READ_FN(64)

#define AD9910_REG_WRITE_FN(nb)						\
static inline int ad9910_reg##nb##_write(struct ad9910_state *st,	\
					 u8 reg, u##nb data,		\
					 bool update)			\
{									\
	int ret;							\
									\
	put_unaligned_be##nb(data, &st->buf[AD9910_SPI_DATA_IDX]);	\
	ret = ad9910_spi_write(st, reg, sizeof(data), update);		\
	if (ret)							\
		return ret;						\
									\
	st->reg[reg].val##nb = data;					\
	return ret;							\
}

AD9910_REG_WRITE_FN(16)
AD9910_REG_WRITE_FN(32)
AD9910_REG_WRITE_FN(64)

#define AD9910_REG_UPDATE_FN(nb)					\
static int ad9910_reg##nb##_update(struct ad9910_state *st,		\
				   u8 reg, u##nb mask,			\
				   u##nb data, bool update)		\
{									\
	u##nb reg_val = (st->reg[reg].val##nb & ~mask) | (data & mask);	\
									\
	if (reg_val == st->reg[reg].val##nb && !update)			\
		return 0;						\
									\
	return ad9910_reg##nb##_write(st, reg, reg_val, update);	\
}

AD9910_REG_UPDATE_FN(16)
AD9910_REG_UPDATE_FN(32)
AD9910_REG_UPDATE_FN(64)

static int ad9910_profile_set(struct ad9910_state *st, u8 profile)
{
	DECLARE_BITMAP(values, BITS_PER_TYPE(profile));

	if (profile >= AD9910_NUM_PROFILES)
		return -EINVAL;

	st->profile = profile;
	values[0] = profile;
	gpiod_multi_set_value_cansleep(st->gpio_profile, values);
	return 0;
}

static int ad9910_powerdown_set(struct ad9910_state *st, bool enable)
{
	return gpiod_set_value_cansleep(st->gpio_pwdown, enable);
}

static ssize_t ad9910_ext_info_read(struct iio_dev *indio_dev,
				    uintptr_t private,
				    const struct iio_chan_spec *chan,
				    char *buf)
{
	struct ad9910_state *st = iio_priv(indio_dev);
	int val;

	guard(mutex)(&st->lock);

	switch (private) {
	case AD9910_PROFILE:
		val = st->profile;
		break;
	case AD9910_POWERDOWN:
		val = !!FIELD_GET(AD9910_CFR1_SOFT_POWER_DOWN_MSK,
				  st->reg[AD9910_REG_CFR1].val32);
		break;
	case AD9910_PP_FREQ_SCALE:
		val = BIT(FIELD_GET(AD9910_CFR2_FM_GAIN_MSK,
				    st->reg[AD9910_REG_CFR2].val32));
		break;
	default:
		return -EINVAL;
	}

	return iio_format_value(buf, IIO_VAL_INT, 1, &val);
}

static ssize_t ad9910_ext_info_write(struct iio_dev *indio_dev,
				     uintptr_t private,
				     const struct iio_chan_spec *chan,
				     const char *buf, size_t len)
{
	struct ad9910_state *st = iio_priv(indio_dev);

	u32 val32;
	int ret;

	ret = kstrtou32(buf, 10, &val32);
	if (ret)
		return ret;

	guard(mutex)(&st->lock);

	switch (private) {
	case AD9910_PROFILE:
		if (val32 >= AD9910_NUM_PROFILES)
			return -EINVAL;
		ret = ad9910_profile_set(st, val32);
		break;
	case AD9910_POWERDOWN:
		val32 = val32 ? AD9910_CFR1_SOFT_POWER_DOWN_MSK : 0;
		ret = ad9910_reg32_update(st, AD9910_REG_CFR1,
					  AD9910_CFR1_SOFT_POWER_DOWN_MSK,
					  val32, true);
		break;
	case AD9910_PP_FREQ_SCALE:
		if (val32 > BIT(15) || !is_power_of_2(val32))
			return -EINVAL;

		val32 = FIELD_PREP(AD9910_CFR2_FM_GAIN_MSK, ilog2(val32));
		ret = ad9910_reg32_update(st, AD9910_REG_CFR2,
					  AD9910_CFR2_FM_GAIN_MSK,
					  val32, true);
		break;
	default:
		return -EINVAL;
	}

	return ret ?: len;
}

static ssize_t ad9910_pp_attrs_read(struct iio_dev *indio_dev,
				    uintptr_t private,
				    const struct iio_chan_spec *chan,
				    char *buf)
{
	struct ad9910_state *st = iio_priv(indio_dev);
	int vals[2];
	u32 tmp32;
	u64 tmp64;

	guard(mutex)(&st->lock);

	switch (private) {
	case AD9910_PP_FREQ_OFFSET:
		tmp64 = (u64)st->reg[AD9910_REG_FTW].val32 * st->data.sysclk_freq_hz;
		vals[0] = upper_32_bits(tmp64);
		vals[1] = upper_32_bits((u64)lower_32_bits(tmp64) * MICRO);
		break;
	case AD9910_PP_PHASE_OFFSET:
		tmp32 = FIELD_GET(AD9910_POW_PP_LSB_MSK,
				  st->reg[AD9910_REG_POW].val16);
		tmp32 = (tmp32 * AD9910_MAX_PHASE_MICRORAD) >> 16;
		vals[0] = tmp32 / MICRO;
		vals[1] = tmp32 % MICRO;
		break;
	case AD9910_PP_AMP_OFFSET:
		tmp32 = FIELD_GET(AD9910_ASF_SCALE_FACTOR_PP_LSB_MSK,
				  st->reg[AD9910_REG_ASF].val32);
		vals[0] = 0;
		vals[1] = (u64)tmp32 * MICRO >> 14;
		break;
	default:
		return -EINVAL;
	}

	return iio_format_value(buf, IIO_VAL_INT_PLUS_MICRO, ARRAY_SIZE(vals), vals);
}

static ssize_t ad9910_pp_attrs_write(struct iio_dev *indio_dev,
				     uintptr_t private,
				     const struct iio_chan_spec *chan,
				     const char *buf, size_t len)
{
	struct ad9910_state *st = iio_priv(indio_dev);
	int val, val2;
	u32 tmp32;
	int ret;

	ret = iio_str_to_fixpoint(buf, MICRO / 10, &val, &val2);
	if (ret)
		return ret;

	guard(mutex)(&st->lock);

	switch (private) {
	case AD9910_PP_FREQ_OFFSET:
		if (val < 0 || val >= st->data.sysclk_freq_hz / 2)
			return -EINVAL;

		tmp32 = ad9910_rational_scale((u64)val * MICRO + val2, BIT_ULL(32),
					      (u64)MICRO * st->data.sysclk_freq_hz);
		ret = ad9910_reg32_write(st, AD9910_REG_FTW, tmp32, true);
		break;
	case AD9910_PP_PHASE_OFFSET:
		if (val != 0 || val2 < 0 || val2 >= (AD9910_MAX_PHASE_MICRORAD >> 8))
			return -EINVAL;

		tmp32 = DIV_ROUND_CLOSEST((u32)val2 << 16, AD9910_MAX_PHASE_MICRORAD);
		tmp32 = min(tmp32, AD9910_POW_PP_LSB_MAX);
		tmp32 = FIELD_PREP(AD9910_POW_PP_LSB_MSK, tmp32);
		ret = ad9910_reg16_update(st, AD9910_REG_POW,
					  AD9910_POW_PP_LSB_MSK,
					  tmp32, true);
		break;
	case AD9910_PP_AMP_OFFSET:
		if (val != 0 || val2 < 0 || val2 >= (MICRO >> 8))
			return -EINVAL;

		tmp32 = DIV_ROUND_CLOSEST((u32)val2 << 14, MICRO);
		tmp32 = min(tmp32, AD9910_ASF_PP_LSB_MAX);
		tmp32 = FIELD_PREP(AD9910_ASF_SCALE_FACTOR_PP_LSB_MSK, tmp32);
		ret = ad9910_reg32_update(st, AD9910_REG_ASF,
					  AD9910_ASF_SCALE_FACTOR_PP_LSB_MSK,
					  tmp32, true);
		break;
	default:
		return -EINVAL;
	}

	return ret ?: len;
}

#define AD9910_EXT_INFO_TMPL(_name, _ident, _shared, _fn_desc) { \
	.name = _name, \
	.read = ad9910_ ## _fn_desc ## _read, \
	.write = ad9910_ ## _fn_desc ## _write, \
	.private = _ident, \
	.shared = _shared, \
}

#define AD9910_EXT_INFO(_name, _ident, _shared) \
	AD9910_EXT_INFO_TMPL(_name, _ident, _shared, ext_info)

#define AD9910_PP_EXT_INFO(_name, _ident) \
	AD9910_EXT_INFO_TMPL(_name, _ident, IIO_SEPARATE, pp_attrs)

static const struct iio_chan_spec_ext_info ad9910_shared_ext_info[] = {
	AD9910_EXT_INFO("profile", AD9910_PROFILE, IIO_SHARED_BY_TYPE),
	AD9910_EXT_INFO("powerdown", AD9910_POWERDOWN, IIO_SHARED_BY_TYPE),
	{ },
};

static const struct iio_chan_spec_ext_info ad9910_pp_ext_info[] = {
	AD9910_EXT_INFO("frequency_scale", AD9910_PP_FREQ_SCALE, IIO_SEPARATE),
	AD9910_PP_EXT_INFO("frequency_offset", AD9910_PP_FREQ_OFFSET),
	AD9910_PP_EXT_INFO("phase_offset", AD9910_PP_PHASE_OFFSET),
	AD9910_PP_EXT_INFO("scale_offset", AD9910_PP_AMP_OFFSET),
	{ },
};

static const struct iio_chan_spec ad9910_channels[] = {
	[AD9910_CHANNEL_SINGLE_TONE] = {
		.type = IIO_ALTVOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = AD9910_CHANNEL_SINGLE_TONE,
		.scan_index = -1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_FREQUENCY) |
				      BIT(IIO_CHAN_INFO_PHASE) |
				      BIT(IIO_CHAN_INFO_SCALE),
		.ext_info = ad9910_shared_ext_info,
	},
	[AD9910_CHANNEL_PARALLEL_PORT] = {
		.type = IIO_ALTVOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = AD9910_CHANNEL_PARALLEL_PORT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_ENABLE),
		.ext_info = ad9910_pp_ext_info,
	},
};

static int ad9910_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long info)
{
	struct ad9910_state *st = iio_priv(indio_dev);
	u64 tmp64;
	u32 tmp32;

	guard(mutex)(&st->lock);

	switch (info) {
	case IIO_CHAN_INFO_ENABLE:
		switch (chan->channel) {
		case AD9910_CHANNEL_PARALLEL_PORT:
			*val = FIELD_GET(AD9910_CFR2_PARALLEL_DATA_PORT_EN_MSK,
					 st->reg[AD9910_REG_CFR2].val32);
			break;
		default:
			return -EINVAL;
		}
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_FREQUENCY:
		tmp32 = FIELD_GET(AD9910_PROFILE_ST_FTW_MSK,
				  st->reg[AD9910_REG_PROFILE(st->profile)].val64);
		tmp64 = (u64)tmp32 * st->data.sysclk_freq_hz;
		*val = upper_32_bits(tmp64);
		*val2 = upper_32_bits((u64)lower_32_bits(tmp64) * MICRO);
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_PHASE:
		tmp32 = FIELD_GET(AD9910_PROFILE_ST_POW_MSK,
				  st->reg[AD9910_REG_PROFILE(st->profile)].val64);
		tmp32 = ((u64)tmp32 * AD9910_MAX_PHASE_MICRORAD) >> 16;
		*val = tmp32 / MICRO;
		*val2 = tmp32 % MICRO;
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_SCALE:
		tmp32 = FIELD_GET(AD9910_PROFILE_ST_ASF_MSK,
				  st->reg[AD9910_REG_PROFILE(st->profile)].val64);
		*val = 0;
		*val2 = (u64)tmp32 * MICRO >> 14;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int ad9910_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val, int val2, long info)
{
	struct ad9910_state *st = iio_priv(indio_dev);
	u64 tmp64;
	u32 tmp32;
	u16 tmp16;

	guard(mutex)(&st->lock);

	switch (info) {
	case IIO_CHAN_INFO_ENABLE:
		val = val ? 1 : 0;
		switch (chan->channel) {
		case AD9910_CHANNEL_PARALLEL_PORT:
			tmp32 = FIELD_PREP(AD9910_CFR2_PARALLEL_DATA_PORT_EN_MSK, val);
			return ad9910_reg32_update(st, AD9910_REG_CFR2,
						   AD9910_CFR2_PARALLEL_DATA_PORT_EN_MSK,
						   tmp32, true);
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_FREQUENCY:
		if (val < 0 || val >= st->data.sysclk_freq_hz / 2)
			return -EINVAL;

		tmp32 = ad9910_rational_scale((u64)val * MICRO + val2, BIT_ULL(32),
					      (u64)MICRO * st->data.sysclk_freq_hz);
		return ad9910_reg64_update(st, AD9910_REG_PROFILE(st->profile),
					   AD9910_PROFILE_ST_FTW_MSK,
					   FIELD_PREP(AD9910_PROFILE_ST_FTW_MSK, tmp32),
					   true);
	case IIO_CHAN_INFO_PHASE:
		tmp64 = (u64)val * MICRO + val2;
		if (val < 0 || val2 < 0 || tmp64 >= AD9910_MAX_PHASE_MICRORAD)
			return -EINVAL;

		tmp32 = DIV_U64_ROUND_CLOSEST(tmp64 << 16, AD9910_MAX_PHASE_MICRORAD);
		tmp16 = min(tmp32, AD9910_POW_MAX);
		return ad9910_reg64_update(st, AD9910_REG_PROFILE(st->profile),
					   AD9910_PROFILE_ST_POW_MSK,
					   FIELD_PREP(AD9910_PROFILE_ST_POW_MSK, tmp16),
					   true);
	case IIO_CHAN_INFO_SCALE:
		if (val < 0 || val > 1 || (val == 1 && val2 > 0))
			return -EINVAL;

		tmp64 = ((u64)val * MICRO + val2) << 14;
		tmp64 = DIV_U64_ROUND_CLOSEST(tmp64, MICRO);
		tmp16 = min(tmp64, AD9910_ASF_MAX);
		return ad9910_reg64_update(st, AD9910_REG_PROFILE(st->profile),
					   AD9910_PROFILE_ST_ASF_MSK,
					   FIELD_PREP(AD9910_PROFILE_ST_ASF_MSK, tmp16),
					   true);
	default:
		return -EINVAL;
	}
}

static int ad9910_write_raw_get_fmt(struct iio_dev *indio_dev,
				    struct iio_chan_spec const *chan,
				    long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_ENABLE:
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_FREQUENCY:
	case IIO_CHAN_INFO_PHASE:
	case IIO_CHAN_INFO_SCALE:
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static int ad9910_reg_access(struct iio_dev *indio_dev,
			     unsigned int reg,
			     unsigned int writeval,
			     unsigned int *readval)
{
	struct ad9910_state *st = iio_priv(indio_dev);
	int ret;
	u64 tmp64;
	u32 tmp32;
	u16 tmp16;
	bool high32 = !!(reg & AD9910_REG_HIGH32_FLAG);

	reg &= ~AD9910_REG_HIGH32_FLAG;
	if (reg >= AD9910_REG_RAM)
		return -EINVAL;

	guard(mutex)(&st->lock);

	switch (reg) {
	case AD9910_REG_DRG_LIMIT:
	case AD9910_REG_DRG_STEP:
	case AD9910_REG_PROFILE0:
	case AD9910_REG_PROFILE1:
	case AD9910_REG_PROFILE2:
	case AD9910_REG_PROFILE3:
	case AD9910_REG_PROFILE4:
	case AD9910_REG_PROFILE5:
	case AD9910_REG_PROFILE6:
	case AD9910_REG_PROFILE7:
		if (readval) {
			ret = ad9910_reg64_read(st, reg, &tmp64);
			if (ret < 0)
				return ret;

			if (high32)
				*readval = upper_32_bits(tmp64);
			else
				*readval = lower_32_bits(tmp64);
		} else {
			tmp64 = st->reg[reg].val64;
			if (high32)
				FIELD_MODIFY(AD9910_REG_HIGH32_MSK, &tmp64, writeval);
			else
				FIELD_MODIFY(AD9910_REG_LOW32_MSK, &tmp64, writeval);

			return ad9910_reg64_write(st, reg, tmp64, true);
		}
		break;
	case AD9910_REG_POW:
		if (!readval)
			return ad9910_reg16_write(st, reg, writeval, true);

		ret = ad9910_reg16_read(st, reg, &tmp16);
		if (ret < 0)
			return ret;
		*readval = tmp16;
		break;
	default:
		if (!readval)
			return ad9910_reg32_write(st, reg, writeval, true);

		ret = ad9910_reg32_read(st, reg, &tmp32);
		if (ret < 0)
			return ret;
		*readval = tmp32;
		break;
	}

	return ret;
}

static const struct iio_info ad9910_info = {
	.read_raw = ad9910_read_raw,
	.write_raw = ad9910_write_raw,
	.write_raw_get_fmt = ad9910_write_raw_get_fmt,
	.debugfs_reg_access = &ad9910_reg_access,
};

static int ad9910_set_dac_current(struct ad9910_state *st, bool update)
{
	u32 fsc_code;

	/* FSC = (86.4 / Rset) * (1 + CODE/256) where Rset = 10k ohms */
	fsc_code = DIV_ROUND_CLOSEST(st->data.dac_output_current, 90) - 96;
	fsc_code &= 0xFFU;

	return ad9910_reg32_write(st, AD9910_REG_AUX_DAC, fsc_code, update);
}

static int ad9910_cfg_sysclk(struct ad9910_state *st, bool update)
{
	u32 cp_index, cfr3 = AD9910_CFR3_OPEN_MSK;

	cfr3 |= FIELD_PREP(AD9910_CFR3_DRV0_MSK, st->data.refclk_out_drv);
	st->data.sysclk_freq_hz = clk_get_rate(st->refclk);

	if (st->data.pll_multiplier) {
		st->data.sysclk_freq_hz *= st->data.pll_multiplier;
		if (st->data.sysclk_freq_hz < AD9910_PLL_OUT_MIN_FREQ_HZ ||
		    st->data.sysclk_freq_hz > AD9910_PLL_OUT_MAX_FREQ_HZ) {
			dev_err(&st->spi->dev, "invalid vco frequency: %u Hz\n",
				st->data.sysclk_freq_hz);
			return -ERANGE;
		}

		if (st->data.pll_vco_range >= AD9910_VCO_RANGE_NUM) {
			if (st->data.sysclk_freq_hz <= AD9910_VCO0_RANGE_AUTO_MAX_HZ)
				st->data.pll_vco_range = 0;
			else if (st->data.sysclk_freq_hz <= AD9910_VCO1_RANGE_AUTO_MAX_HZ)
				st->data.pll_vco_range = 1;
			else if (st->data.sysclk_freq_hz <= AD9910_VCO2_RANGE_AUTO_MAX_HZ)
				st->data.pll_vco_range = 2;
			else if (st->data.sysclk_freq_hz <= AD9910_VCO3_RANGE_AUTO_MAX_HZ)
				st->data.pll_vco_range = 3;
			else if (st->data.sysclk_freq_hz <= AD9910_VCO4_RANGE_AUTO_MAX_HZ)
				st->data.pll_vco_range = 4;
			else
				st->data.pll_vco_range = 5;
			dev_dbg(&st->spi->dev, "auto-selected VCO range: %u\n",
				st->data.pll_vco_range);
		}

		cp_index = st->data.pll_charge_pump_current - AD9910_ICP_MIN_uA;
		cp_index = DIV_ROUND_CLOSEST(cp_index, AD9910_ICP_STEP_uA);
		cfr3 |= FIELD_PREP(AD9910_CFR3_VCO_SEL_MSK, st->data.pll_vco_range) |
			FIELD_PREP(AD9910_CFR3_ICP_MSK, cp_index) |
			FIELD_PREP(AD9910_CFR3_N_MSK, st->data.pll_multiplier) |
			AD9910_CFR3_PLL_EN_MSK;
	} else {
		cfr3 |= AD9910_CFR3_VCO_SEL_MSK |
			AD9910_CFR3_ICP_MSK |
			AD9910_CFR3_REFCLK_DIV_RESETB_MSK |
			FIELD_PREP(AD9910_CFR3_REFCLK_DIV_BYPASS_MSK, !st->data.ref_div2_en) |
			AD9910_CFR3_PFD_RESET_MSK;
		if (st->data.ref_div2_en)
			st->data.sysclk_freq_hz >>= 1;
	}

	return ad9910_reg32_write(st, AD9910_REG_CFR3, cfr3, update);
}

static int ad9910_parse_fw(struct ad9910_state *st)
{
	struct device *dev = &st->spi->dev;
	u32 tmp;
	int ret;

	ret = device_property_read_u32(dev, "adi,pll-multiplier", &tmp);
	if (!ret) {
		if (tmp < AD9910_PLL_MIN_N || tmp > AD9910_PLL_MAX_N)
			return dev_err_probe(dev, -ERANGE,
					     "invalid PLL multiplier %u\n", tmp);
		st->data.pll_multiplier = tmp;

		tmp = AD9910_VCO_RANGE_NUM;
		ret = device_property_read_u32(dev, "adi,pll-vco-select", &tmp);
		if (!ret && tmp >= AD9910_VCO_RANGE_NUM)
			return dev_err_probe(dev, -ERANGE,
					     "invalid VCO range: %u\n", tmp);
		st->data.pll_vco_range = tmp;

		tmp = AD9910_ICP_MAX_uA;
		device_property_read_u32(dev, "adi,charge-pump-current-microamp", &tmp);
		if (tmp < AD9910_ICP_MIN_uA || tmp > AD9910_ICP_MAX_uA)
			return dev_err_probe(dev, -ERANGE,
					     "invalid charge pump current %u\n", tmp);
		st->data.pll_charge_pump_current = tmp;

		st->data.refclk_out_drv = AD9910_REFCLK_OUT_DRV_DISABLED;
		ret = device_property_match_property_string(dev,
							    "adi,refclk-out-drive-strength",
							    ad9910_refclk_out_drv0,
							    ARRAY_SIZE(ad9910_refclk_out_drv0));
		if (ret >= 0)
			st->data.refclk_out_drv = ret;
	}

	st->data.ref_div2_en = device_property_read_bool(dev, "adi,reference-div2-enable");
	st->data.inverse_sinc_enable = device_property_read_bool(dev, "adi,inverse-sinc-enable");
	st->data.sine_output_enable = device_property_read_bool(dev, "adi,sine-output-enable");
	st->data.sync_clk_enable = !device_property_read_bool(dev, "adi,sync-clk-disable");
	st->data.pdclk_enable = !device_property_read_bool(dev, "adi,pdclk-disable");
	st->data.pdclk_invert = device_property_read_bool(dev, "adi,pdclk-invert");
	st->data.tx_enable_invert = device_property_read_bool(dev, "adi,tx-enable-invert");

	tmp = AD9910_DAC_IOUT_DEFAULT_uA;
	device_property_read_u32(dev, "adi,dac-output-current-microamp", &tmp);
	if (tmp < AD9910_DAC_IOUT_MIN_uA || tmp > AD9910_DAC_IOUT_MAX_uA)
		return dev_err_probe(dev, -ERANGE,
				     "Invalid DAC output current %u uA\n", tmp);
	st->data.dac_output_current = tmp;

	return 0;
}

static int ad9910_setup(struct ad9910_state *st, struct reset_control *dev_rst)
{
	u32 reg32;
	int ret;

	ret = reset_control_deassert(dev_rst);
	if (ret)
		return ret;

	reg32 = AD9910_CFR1_SDIO_INPUT_ONLY_MSK;
	reg32 |= FIELD_PREP(AD9910_CFR1_INV_SINC_EN_MSK, st->data.inverse_sinc_enable) |
		 FIELD_PREP(AD9910_CFR1_SELECT_SINE_MSK, st->data.sine_output_enable);

	ret = ad9910_reg32_write(st, AD9910_REG_CFR1, reg32, false);
	if (ret)
		return ret;

	reg32 = AD9910_CFR2_AMP_SCALE_SINGLE_TONE_MSK;
	reg32 |= AD9910_CFR2_SYNC_TIMING_VAL_DISABLE_MSK |
		 AD9910_CFR2_DRG_NO_DWELL_MSK |
		 AD9910_CFR2_DATA_ASM_HOLD_LAST_MSK |
		 FIELD_PREP(AD9910_CFR2_SYNC_CLK_EN_MSK, st->data.sync_clk_enable) |
		 FIELD_PREP(AD9910_CFR2_PDCLK_ENABLE_MSK, st->data.pdclk_enable) |
		 FIELD_PREP(AD9910_CFR2_PDCLK_INVERT_MSK, st->data.pdclk_invert) |
		 FIELD_PREP(AD9910_CFR2_TXENABLE_INVERT_MSK, st->data.tx_enable_invert);

	ret = ad9910_reg32_write(st, AD9910_REG_CFR2, reg32, false);
	if (ret)
		return ret;

	ret = ad9910_cfg_sysclk(st, false);
	if (ret)
		return ret;

	ret = ad9910_set_dac_current(st, false);
	if (ret)
		return ret;

	return ad9910_io_update(st);
}

static void ad9910_power_down(void *data)
{
	struct ad9910_state *st = data;

	if (!ad9910_powerdown_set(st, true))
		return;

	ad9910_reg32_update(st, AD9910_REG_CFR1,
			    AD9910_CFR1_SOFT_POWER_DOWN_MSK,
			    AD9910_CFR1_SOFT_POWER_DOWN_MSK,
			    true);
}

static int ad9910_probe(struct spi_device *spi)
{
	struct reset_control *dev_rst, *io_rst;
	struct gpio_desc *io_rst_gpio;
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev;
	struct ad9910_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;

	spi_set_drvdata(spi, indio_dev);

	st->refclk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(st->refclk))
		return dev_err_probe(dev, PTR_ERR(st->refclk),
				     "Failed to get reference clock\n");

	ret = devm_regulator_bulk_get_enable(dev,
					     ARRAY_SIZE(ad9910_power_supplies),
					     ad9910_power_supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ret = devm_mutex_init(dev, &st->lock);
	if (ret)
		return ret;

	indio_dev->name = "ad9910";
	indio_dev->info = &ad9910_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ad9910_channels;
	indio_dev->num_channels = ARRAY_SIZE(ad9910_channels);

	dev_rst = devm_reset_control_get_optional_exclusive(dev, NULL);
	if (IS_ERR(dev_rst))
		return dev_err_probe(dev, PTR_ERR(dev_rst),
				     "failed to get device reset control\n");

	ret = reset_control_assert(dev_rst);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to assert device reset control\n");

	/*
	 * The IO RESET pin is not used in this driver, as we assume that all
	 * SPI transfers are complete, but if it is wired up, we need to make
	 * sure it is not floating. We can use either a reset controller or a
	 * GPIO for this.
	 */
	io_rst = devm_reset_control_get_optional_exclusive_deasserted(dev, "io");
	if (IS_ERR(io_rst))
		return dev_err_probe(dev, PTR_ERR(io_rst),
				     "failed to get io reset control\n");

	io_rst_gpio = devm_gpiod_get_index_optional(dev, "reset", 1,
						    GPIOD_OUT_LOW);
	if (IS_ERR(io_rst_gpio))
		return dev_err_probe(dev, PTR_ERR(io_rst_gpio),
				     "failed to get io reset gpio\n");

	st->gpio_pwdown = devm_gpiod_get_optional(dev, "powerdown",
						  GPIOD_OUT_LOW);
	if (IS_ERR(st->gpio_pwdown))
		return dev_err_probe(dev, PTR_ERR(st->gpio_pwdown),
				     "failed to get powerdown gpio\n");

	st->gpio_update = devm_gpiod_get_optional(dev, "update", GPIOD_OUT_LOW);
	if (IS_ERR(st->gpio_update))
		return dev_err_probe(dev, PTR_ERR(st->gpio_update),
				     "failed to get update gpio\n");

	st->gpio_profile = devm_gpiod_get_array_optional(dev, "profile",
							 GPIOD_OUT_LOW);
	if (IS_ERR(st->gpio_profile))
		return dev_err_probe(dev, PTR_ERR(st->gpio_profile),
				     "failed to get profile gpios\n");

	ret = ad9910_parse_fw(st);
	if (ret)
		return ret;

	ret = ad9910_setup(st, dev_rst);
	if (ret)
		return dev_err_probe(dev, ret, "device setup failed\n");

	ret = devm_add_action_or_reset(dev, ad9910_power_down, st);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add power down action\n");

	return devm_iio_device_register(dev, indio_dev);
}

static const struct spi_device_id ad9910_id[] = {
	{"ad9910", 0},
	{}
};
MODULE_DEVICE_TABLE(spi, ad9910_id);

static const struct of_device_id ad9910_of_match[] = {
	{ .compatible = "adi,ad9910" },
	{ }
};
MODULE_DEVICE_TABLE(of, ad9910_of_match);

static struct spi_driver ad9910_driver = {
	.driver = {
		.name = "ad9910",
		.of_match_table = ad9910_of_match,
	},
	.probe = ad9910_probe,
	.id_table = ad9910_id,
};
module_spi_driver(ad9910_driver);

MODULE_AUTHOR("Rodrigo Alencar <rodrigo.alencar@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD9910 DDS driver");
MODULE_LICENSE("GPL");
