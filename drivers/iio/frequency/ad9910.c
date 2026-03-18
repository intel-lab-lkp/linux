// SPDX-License-Identifier: GPL-2.0-only
/*
 * AD9910 SPI DDS (Direct Digital Synthesizer) driver
 *
 * Copyright 2026 Analog Devices Inc.
 */

#include <linux/array_size.h>
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
#define AD9910_REG_HIGH32_FLAG_MSK	BIT(8)

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

/* Digital Ramp Limit Register */
#define AD9910_DRG_LIMIT_UPPER_MSK		GENMASK_ULL(63, 32)
#define AD9910_DRG_LIMIT_LOWER_MSK		GENMASK_ULL(31, 0)

/* Digital Ramp Step Register */
#define AD9910_DRG_STEP_DEC_MSK			GENMASK_ULL(63, 32)
#define AD9910_DRG_STEP_INC_MSK			GENMASK_ULL(31, 0)

/* Digital Ramp Rate Register */
#define AD9910_DRG_RATE_DEC_MSK			GENMASK(31, 16)
#define AD9910_DRG_RATE_INC_MSK			GENMASK(15, 0)

/* Profile Register Format (Single Tone Mode) */
#define AD9910_PROFILE_ST_ASF_MSK		GENMASK_ULL(61, 48)
#define AD9910_PROFILE_ST_POW_MSK		GENMASK_ULL(47, 32)
#define AD9910_PROFILE_ST_FTW_MSK		GENMASK_ULL(31, 0)

/* Device constants */
#define AD9910_PI_NANORAD		3141592653UL

#define AD9910_MAX_SYSCLK_HZ		(1000 * HZ_PER_MHZ)
#define AD9910_MAX_PHASE_MICRORAD	(AD9910_PI_NANORAD / 500)

#define AD9910_ASF_MAX			(BIT(14) - 1)
#define AD9910_ASF_PP_LSB_MAX		(BIT(6) - 1)
#define AD9910_POW_MAX			(BIT(16) - 1)
#define AD9910_POW_PP_LSB_MAX		(BIT(8) - 1)
#define AD9910_STEP_RATE_MAX		(BIT(16) - 1)
#define AD9910_NUM_PROFILES		8

#define AD9910_DRG_DEST_NUM		3

/* PLL constants */
#define AD9910_PLL_MIN_N		12
#define AD9910_PLL_MAX_N		127

#define AD9910_PLL_IN_MIN_FREQ_HZ	(3200 * HZ_PER_KHZ)
#define AD9910_PLL_IN_MAX_FREQ_HZ	(60 * HZ_PER_MHZ)

#define AD9910_PLL_OUT_MIN_FREQ_HZ	(420 * HZ_PER_MHZ)
#define AD9910_PLL_OUT_MAX_FREQ_HZ	(1000 * HZ_PER_MHZ)

#define AD9910_VCO0_RANGE_AUTO_MAX_HZ	(457 * HZ_PER_MHZ)
#define AD9910_VCO1_RANGE_AUTO_MAX_HZ	(530 * HZ_PER_MHZ)
#define AD9910_VCO2_RANGE_AUTO_MAX_HZ	(632 * HZ_PER_MHZ)
#define AD9910_VCO3_RANGE_AUTO_MAX_HZ	(775 * HZ_PER_MHZ)
#define AD9910_VCO4_RANGE_AUTO_MAX_HZ	(897 * HZ_PER_MHZ)
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
#define AD9910_SPI_READ_MSK		BIT(7)
#define AD9910_SPI_ADDR_MSK		GENMASK(4, 0)

/**
 * enum ad9910_channel - AD9910 channel identifiers in priority order
 *
 * @AD9910_CHANNEL_PHY: Physical output channel
 * @AD9910_CHANNEL_SINGLE_TONE: Single tone output channel
 * @AD9910_CHANNEL_PARALLEL_PORT: Parallel port output channel
 * @AD9910_CHANNEL_DRG: Digital Ramp Generator output channel
 * @AD9910_CHANNEL_DRG_RAMP_UP: DRG ramp up channel
 * @AD9910_CHANNEL_DRG_RAMP_DOWN: DRG ramp down channel
 */
enum ad9910_channel {
	AD9910_CHANNEL_PHY = 100,
	AD9910_CHANNEL_SINGLE_TONE = 110,
	AD9910_CHANNEL_PARALLEL_PORT = 120,
	AD9910_CHANNEL_DRG = 130,
	AD9910_CHANNEL_DRG_RAMP_UP = 131,
	AD9910_CHANNEL_DRG_RAMP_DOWN = 132,
};

/**
 * enum ad9910_destination - AD9910 DDS core parameter destination
 *
 * @AD9910_DEST_FREQUENCY: Frequency destination
 * @AD9910_DEST_PHASE: Phase destination
 * @AD9910_DEST_AMPLITUDE: Amplitude destination
 * @AD9910_DEST_POLAR: Polar destination
 */
enum ad9910_destination {
	AD9910_DEST_FREQUENCY,
	AD9910_DEST_PHASE,
	AD9910_DEST_AMPLITUDE,
	AD9910_DEST_POLAR,
};

/**
 * enum ad9910_drg_oper_mode - Digital Ramp Generator Operating Mode
 *
 * @AD9910_DRG_OPER_MODE_BIDIR: Normal Ramp Generation
 * @AD9910_DRG_OPER_MODE_RAMP_DOWN: No-dwell Low only operation
 * @AD9910_DRG_OPER_MODE_RAMP_UP: No-dwell High only operation
 * @AD9910_DRG_OPER_MODE_BIDIR_CONT: Both No-dwell High/Low operation
 */
enum ad9910_drg_oper_mode {
	AD9910_DRG_OPER_MODE_BIDIR,
	AD9910_DRG_OPER_MODE_RAMP_DOWN,
	AD9910_DRG_OPER_MODE_RAMP_UP,
	AD9910_DRG_OPER_MODE_BIDIR_CONT,
};

enum {
	AD9910_CHAN_IDX_PHY,
	AD9910_CHAN_IDX_SINGLE_TONE,
	AD9910_CHAN_IDX_PARALLEL_PORT,
	AD9910_CHAN_IDX_DRG,
	AD9910_CHAN_IDX_DRG_RAMP_UP,
	AD9910_CHAN_IDX_DRG_RAMP_DOWN,
	AD9910_CHAN_IDX_RAM,
	AD9910_CHAN_IDX_OSK,
};

enum {
	AD9910_PROFILE,
	AD9910_POWERDOWN,
	AD9910_PP_FREQ_SCALE,
	AD9910_PP_FREQ_OFFSET,
	AD9910_PP_PHASE_OFFSET,
	AD9910_PP_AMP_OFFSET,
	AD9910_DRG_FREQ_STEP,
	AD9910_DRG_PHASE_STEP,
	AD9910_DRG_AMP_STEP,
};

struct ad9910_data {
	u32 sysclk_freq_hz;
	u32 dac_output_current;

	u16 pll_charge_pump_current;
	u8 refclk_out_drv;
	bool pll_enabled;
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

	/* Lock for accessing device registers and state variables */
	struct mutex lock;

	struct ad9910_data data;
	u8 profile;

	union {
		__be64 be64;
		__be32 be32;
		__be16 be16;
	} rx_buf;
	/*
	 * RAM loading requires a reasonable amount of bytes, at the same time
	 * DMA capable SPI drivers requires the transfer buffers to live in
	 * their own cache lines.
	 */
	u8 tx_buf[AD9910_SPI_MESSAGE_LEN_MAX] __aligned(IIO_DMA_MINALIGN);
};

static const char * const ad9910_power_supplies[] = {
	"dvdd-io33", "avdd33", "dvdd18", "avdd18",
};

static const char * const ad9910_refclk_out_drv0[] = {
	"disabled", "low", "medium", "high",
};

static const char * const ad9910_destination_str[] = {
	[AD9910_DEST_FREQUENCY] = "frequency",
	[AD9910_DEST_PHASE] = "phase",
	[AD9910_DEST_AMPLITUDE] = "amplitude",
	[AD9910_DEST_POLAR] = "polar",
};

static const char * const ad9910_drg_oper_mode_str[] = {
	[AD9910_DRG_OPER_MODE_BIDIR] = "bidirectional",
	[AD9910_DRG_OPER_MODE_RAMP_DOWN] = "ramp_down",
	[AD9910_DRG_OPER_MODE_RAMP_UP] = "ramp_up",
	[AD9910_DRG_OPER_MODE_BIDIR_CONT] = "bidirectional_continuous",
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
	st->tx_buf[0] = AD9910_SPI_READ_MSK |
			FIELD_PREP(AD9910_SPI_ADDR_MSK, reg);
	return spi_write_then_read(st->spi, st->tx_buf, 1, &st->rx_buf, len);
}

static inline int ad9910_spi_write(struct ad9910_state *st, u8 reg, size_t len,
				   bool update)
{
	int ret;

	st->tx_buf[0] = FIELD_PREP(AD9910_SPI_ADDR_MSK, reg);
	ret = spi_write(st->spi, st->tx_buf, AD9910_SPI_DATA_IDX + len);
	if (!ret && update)
		return ad9910_io_update(st);

	return ret;
}

#define AD9910_REG_READ_FN(nb)						\
static int ad9910_reg##nb##_read(struct ad9910_state *st, u8 reg,	\
				 u##nb * data)				\
{									\
	int ret;							\
									\
	ret = ad9910_spi_read(st, reg, sizeof(*data));			\
	if (ret)							\
		return ret;						\
									\
	*data = be##nb##_to_cpu(st->rx_buf.be##nb);			\
	return ret;							\
}

AD9910_REG_READ_FN(16)
AD9910_REG_READ_FN(32)
AD9910_REG_READ_FN(64)

#define AD9910_REG_WRITE_FN(nb)						\
static int ad9910_reg##nb##_write(struct ad9910_state *st, u8 reg,	\
				  u##nb data, bool update)		\
{									\
	int ret;							\
									\
	put_unaligned_be##nb(data, &st->tx_buf[AD9910_SPI_DATA_IDX]);	\
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

static int ad9910_set_dac_current(struct ad9910_state *st, bool update)
{
	u32 fsc_code;

	/* FSC = (86.4 / Rset) * (1 + CODE/256) where Rset = 10k ohms */
	fsc_code = DIV_ROUND_CLOSEST(st->data.dac_output_current, 90) - 96;
	fsc_code &= 0xFFU;

	return ad9910_reg32_write(st, AD9910_REG_AUX_DAC, fsc_code, update);
}

static int ad9910_set_sysclk_freq(struct ad9910_state *st, u32 freq_hz,
				  bool update)
{
	u32 sysclk_freq_hz, refclk_freq_hz = clk_get_rate(st->refclk);
	u32 tmp32, vco_sel;
	int ret;

	if (st->data.pll_enabled) {
		if (refclk_freq_hz < AD9910_PLL_IN_MIN_FREQ_HZ ||
		    refclk_freq_hz > AD9910_PLL_IN_MAX_FREQ_HZ) {
			dev_err(&st->spi->dev,
				"REF_CLK frequency %u Hz is out of PLL input range\n",
				refclk_freq_hz);
			return -ERANGE;
		}

		tmp32 = DIV_ROUND_CLOSEST(freq_hz, refclk_freq_hz);
		tmp32 = clamp(tmp32, AD9910_PLL_MIN_N, AD9910_PLL_MAX_N);
		sysclk_freq_hz = refclk_freq_hz * tmp32;

		if (sysclk_freq_hz < AD9910_PLL_OUT_MIN_FREQ_HZ ||
		    sysclk_freq_hz > AD9910_PLL_OUT_MAX_FREQ_HZ) {
			dev_err(&st->spi->dev,
				"PLL output frequency %u Hz is out of range\n",
				sysclk_freq_hz);
			return -ERANGE;
		}

		if (sysclk_freq_hz <= AD9910_VCO0_RANGE_AUTO_MAX_HZ)
			vco_sel = 0;
		else if (sysclk_freq_hz <= AD9910_VCO1_RANGE_AUTO_MAX_HZ)
			vco_sel = 1;
		else if (sysclk_freq_hz <= AD9910_VCO2_RANGE_AUTO_MAX_HZ)
			vco_sel = 2;
		else if (sysclk_freq_hz <= AD9910_VCO3_RANGE_AUTO_MAX_HZ)
			vco_sel = 3;
		else if (sysclk_freq_hz <= AD9910_VCO4_RANGE_AUTO_MAX_HZ)
			vco_sel = 4;
		else
			vco_sel = 5;

		ret = ad9910_reg32_update(st, AD9910_REG_CFR3,
					  AD9910_CFR3_N_MSK | AD9910_CFR3_VCO_SEL_MSK,
					  FIELD_PREP(AD9910_CFR3_N_MSK, tmp32) |
					  FIELD_PREP(AD9910_CFR3_VCO_SEL_MSK, vco_sel),
					  update);
		if (ret)
			return ret;
	} else {
		tmp32 = DIV_ROUND_CLOSEST(refclk_freq_hz, freq_hz);
		tmp32 = clamp(tmp32, 1, 2);
		sysclk_freq_hz = refclk_freq_hz / tmp32;
		tmp32 = FIELD_PREP(AD9910_CFR3_REFCLK_DIV_BYPASS_MSK, tmp32 % 2);
		ret = ad9910_reg32_update(st, AD9910_REG_CFR3,
					  AD9910_CFR3_REFCLK_DIV_BYPASS_MSK,
					  tmp32, update);
		if (ret)
			return ret;
	}

	st->data.sysclk_freq_hz = sysclk_freq_hz;
	return 0;
}

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

static int ad9910_chan_destination_set(struct iio_dev *indio_dev,
				       const struct iio_chan_spec *chan,
				       unsigned int val)
{
	struct ad9910_state *st = iio_priv(indio_dev);

	guard(mutex)(&st->lock);

	switch (chan->channel) {
	case AD9910_CHANNEL_DRG:
		return ad9910_reg32_update(st, AD9910_REG_CFR2,
					   AD9910_CFR2_DRG_DEST_MSK,
					   FIELD_PREP(AD9910_CFR2_DRG_DEST_MSK, val),
					   true);
	default:
		return -EINVAL;
	}
}

static int ad9910_chan_destination_get(struct iio_dev *indio_dev,
				       const struct iio_chan_spec *chan)
{
	struct ad9910_state *st = iio_priv(indio_dev);

	guard(mutex)(&st->lock);

	switch (chan->channel) {
	case AD9910_CHANNEL_DRG:
		return FIELD_GET(AD9910_CFR2_DRG_DEST_MSK,
				 st->reg[AD9910_REG_CFR2].val32);
	default:
		return -EINVAL;
	}
}

static int ad9910_drg_oper_mode_set(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    unsigned int val)
{
	struct ad9910_state *st = iio_priv(indio_dev);

	guard(mutex)(&st->lock);

	return ad9910_reg32_update(st, AD9910_REG_CFR2,
				   AD9910_CFR2_DRG_NO_DWELL_MSK,
				   FIELD_PREP(AD9910_CFR2_DRG_NO_DWELL_MSK, val),
				   true);
}

static int ad9910_drg_oper_mode_get(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan)
{
	struct ad9910_state *st = iio_priv(indio_dev);

	guard(mutex)(&st->lock);

	return FIELD_GET(AD9910_CFR2_DRG_NO_DWELL_MSK,
			 st->reg[AD9910_REG_CFR2].val32);
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
		if (!in_range(val, 0, st->data.sysclk_freq_hz / 2))
			return -EINVAL;

		tmp32 = ad9910_rational_scale((u64)val * MICRO + val2, BIT_ULL(32),
					      (u64)MICRO * st->data.sysclk_freq_hz);
		ret = ad9910_reg32_write(st, AD9910_REG_FTW, tmp32, true);
		break;
	case AD9910_PP_PHASE_OFFSET:
		if (val)
			return -EINVAL;

		if (!in_range(val2, 0, (AD9910_MAX_PHASE_MICRORAD >> 8)))
			return -EINVAL;

		tmp32 = DIV_ROUND_CLOSEST((u32)val2 << 16, AD9910_MAX_PHASE_MICRORAD);
		tmp32 = min(tmp32, AD9910_POW_PP_LSB_MAX);
		tmp32 = FIELD_PREP(AD9910_POW_PP_LSB_MSK, tmp32);
		ret = ad9910_reg16_update(st, AD9910_REG_POW,
					  AD9910_POW_PP_LSB_MSK,
					  tmp32, true);
		break;
	case AD9910_PP_AMP_OFFSET:
		if (val)
			return -EINVAL;

		if (!in_range(val2, 0, (MICRO >> 8)))
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

static ssize_t ad9910_drg_attrs_read(struct iio_dev *indio_dev,
				     uintptr_t private,
				     const struct iio_chan_spec *chan,
				     char *buf)
{
	struct ad9910_state *st = iio_priv(indio_dev);
	unsigned int type;
	int vals[2];
	u64 tmp64;

	guard(mutex)(&st->lock);

	switch (chan->channel) {
	case AD9910_CHANNEL_DRG_RAMP_UP:
		tmp64 = FIELD_GET(AD9910_DRG_STEP_INC_MSK,
				  st->reg[AD9910_REG_DRG_STEP].val64);
		break;
	case AD9910_CHANNEL_DRG_RAMP_DOWN:
		tmp64 = FIELD_GET(AD9910_DRG_STEP_DEC_MSK,
				  st->reg[AD9910_REG_DRG_STEP].val64);
		break;
	default:
		return -EINVAL;
	}

	switch (private) {
	case AD9910_DRG_FREQ_STEP:
		type = IIO_VAL_INT_PLUS_MICRO;
		tmp64 *= st->data.sysclk_freq_hz;
		vals[0] = upper_32_bits(tmp64);
		vals[1] = upper_32_bits((u64)lower_32_bits(tmp64) * MICRO);
		break;
	case AD9910_DRG_PHASE_STEP:
		type = IIO_VAL_INT_PLUS_NANO;
		tmp64 *= AD9910_PI_NANORAD;
		tmp64 >>= 31;
		vals[0] = div_u64_rem(tmp64, NANO, &vals[1]);
		break;
	case AD9910_DRG_AMP_STEP:
		type = IIO_VAL_INT_PLUS_NANO;
		vals[0] = 0;
		vals[1] = tmp64 * NANO >> 32;
		break;
	default:
		return -EINVAL;
	}

	return iio_format_value(buf, type, ARRAY_SIZE(vals), vals);
}

static ssize_t ad9910_drg_attrs_write(struct iio_dev *indio_dev,
				      uintptr_t private,
				      const struct iio_chan_spec *chan,
				      const char *buf, size_t len)
{
	struct ad9910_state *st = iio_priv(indio_dev);
	int val, val2;
	u64 tmp64;
	int ret;

	guard(mutex)(&st->lock);

	switch (private) {
	case AD9910_DRG_FREQ_STEP:
		ret = iio_str_to_fixpoint(buf, MICRO / 10, &val, &val2);
		if (ret)
			return ret;

		if (!in_range(val, 0, st->data.sysclk_freq_hz / 2))
			return -EINVAL;

		tmp64 = (u64)val * MICRO + val2;
		tmp64 = ad9910_rational_scale(tmp64, BIT_ULL(32),
					      (u64)MICRO * st->data.sysclk_freq_hz);
		break;
	case AD9910_DRG_PHASE_STEP:
		ret = iio_str_to_fixpoint(buf, NANO / 10, &val, &val2);
		if (ret)
			return ret;

		if (val < 0 || val2 < 0)
			return -EINVAL;

		tmp64 = (u64)val * NANO + val2;
		if (tmp64 > 2ULL * AD9910_PI_NANORAD)
			return -EINVAL;

		tmp64 <<= 31;
		tmp64 = DIV_U64_ROUND_CLOSEST(tmp64, AD9910_PI_NANORAD);
		break;
	case AD9910_DRG_AMP_STEP:
		ret = iio_str_to_fixpoint(buf, NANO / 10, &val, &val2);
		if (ret)
			return ret;

		if (val < 0 || val > 1 || (val == 1 && val2 > 0))
			return -EINVAL;

		tmp64 = ((u64)val * NANO + val2) << 32;
		tmp64 = DIV_U64_ROUND_CLOSEST(tmp64, NANO);
		break;
	default:
		return -EINVAL;
	}

	tmp64 = min(tmp64, U32_MAX);

	switch (chan->channel) {
	case AD9910_CHANNEL_DRG_RAMP_UP:
		ret = ad9910_reg64_update(st, AD9910_REG_DRG_STEP,
					  AD9910_DRG_STEP_INC_MSK,
					  FIELD_PREP(AD9910_DRG_STEP_INC_MSK, tmp64),
					  true);
		break;
	case AD9910_CHANNEL_DRG_RAMP_DOWN:
		ret = ad9910_reg64_update(st, AD9910_REG_DRG_STEP,
					  AD9910_DRG_STEP_DEC_MSK,
					  FIELD_PREP(AD9910_DRG_STEP_DEC_MSK, tmp64),
					  true);
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

#define AD9910_DRG_EXT_INFO(_name, _ident) \
	AD9910_EXT_INFO_TMPL(_name, _ident, IIO_SEPARATE, drg_attrs)

static const struct iio_enum ad9910_drg_destination_enum = {
	.items = ad9910_destination_str,
	.num_items = AD9910_DRG_DEST_NUM,
	.set = ad9910_chan_destination_set,
	.get = ad9910_chan_destination_get,
};

static const struct iio_enum ad9910_drg_oper_mode_enum = {
	.items = ad9910_drg_oper_mode_str,
	.num_items = ARRAY_SIZE(ad9910_drg_oper_mode_str),
	.set = ad9910_drg_oper_mode_set,
	.get = ad9910_drg_oper_mode_get,
};

static const struct iio_chan_spec_ext_info ad9910_phy_ext_info[] = {
	AD9910_EXT_INFO("profile", AD9910_PROFILE, IIO_SEPARATE),
	AD9910_EXT_INFO("powerdown", AD9910_POWERDOWN, IIO_SEPARATE),
	{ }
};

static const struct iio_chan_spec_ext_info ad9910_pp_ext_info[] = {
	AD9910_EXT_INFO("frequency_scale", AD9910_PP_FREQ_SCALE, IIO_SEPARATE),
	AD9910_PP_EXT_INFO("frequency_offset", AD9910_PP_FREQ_OFFSET),
	AD9910_PP_EXT_INFO("phase_offset", AD9910_PP_PHASE_OFFSET),
	AD9910_PP_EXT_INFO("scale_offset", AD9910_PP_AMP_OFFSET),
	{ }
};

static const struct iio_chan_spec_ext_info ad9910_drg_ext_info[] = {
	IIO_ENUM("destination", IIO_SEPARATE, &ad9910_drg_destination_enum),
	IIO_ENUM_AVAILABLE("destination", IIO_SEPARATE, &ad9910_drg_destination_enum),
	IIO_ENUM("operating_mode", IIO_SEPARATE, &ad9910_drg_oper_mode_enum),
	IIO_ENUM_AVAILABLE("operating_mode", IIO_SEPARATE, &ad9910_drg_oper_mode_enum),
	{ }
};

static const struct iio_chan_spec_ext_info ad9910_drg_ramp_ext_info[] = {
	AD9910_DRG_EXT_INFO("frequency_step", AD9910_DRG_FREQ_STEP),
	AD9910_DRG_EXT_INFO("phase_step", AD9910_DRG_PHASE_STEP),
	AD9910_DRG_EXT_INFO("scale_step", AD9910_DRG_AMP_STEP),
	{ }
};

static const struct iio_chan_spec ad9910_channels[] = {
	[AD9910_CHAN_IDX_PHY] = {
		.type = IIO_ALTVOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = AD9910_CHANNEL_PHY,
		.address = AD9910_CHAN_IDX_PHY,
		.scan_index = -1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.ext_info = ad9910_phy_ext_info,
	},
	[AD9910_CHAN_IDX_SINGLE_TONE] = {
		.type = IIO_ALTVOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = AD9910_CHANNEL_SINGLE_TONE,
		.address = AD9910_CHAN_IDX_SINGLE_TONE,
		.scan_index = -1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_FREQUENCY) |
				      BIT(IIO_CHAN_INFO_PHASE) |
				      BIT(IIO_CHAN_INFO_SCALE),
	},
	[AD9910_CHAN_IDX_PARALLEL_PORT] = {
		.type = IIO_ALTVOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = AD9910_CHANNEL_PARALLEL_PORT,
		.address = AD9910_CHAN_IDX_PARALLEL_PORT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_ENABLE),
		.ext_info = ad9910_pp_ext_info,
	},
	[AD9910_CHAN_IDX_DRG] = {
		.type = IIO_ALTVOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = AD9910_CHANNEL_DRG,
		.address = AD9910_CHAN_IDX_DRG,
		.scan_index = -1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_ENABLE),
		.ext_info = ad9910_drg_ext_info,
	},
	[AD9910_CHAN_IDX_DRG_RAMP_UP] = {
		.type = IIO_ALTVOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = AD9910_CHANNEL_DRG_RAMP_UP,
		.address = AD9910_CHAN_IDX_DRG_RAMP_UP,
		.scan_index = -1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_FREQUENCY) |
				      BIT(IIO_CHAN_INFO_PHASE) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.ext_info = ad9910_drg_ramp_ext_info,
	},
	[AD9910_CHAN_IDX_DRG_RAMP_DOWN] = {
		.type = IIO_ALTVOLTAGE,
		.indexed = 1,
		.output = 1,
		.channel = AD9910_CHANNEL_DRG_RAMP_DOWN,
		.address = AD9910_CHAN_IDX_DRG_RAMP_DOWN,
		.scan_index = -1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_FREQUENCY) |
				      BIT(IIO_CHAN_INFO_PHASE) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.ext_info = ad9910_drg_ramp_ext_info,
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
		case AD9910_CHANNEL_DRG:
			*val = FIELD_GET(AD9910_CFR2_DRG_ENABLE_MSK,
					 st->reg[AD9910_REG_CFR2].val32);
			break;
		default:
			return -EINVAL;
		}
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_FREQUENCY:
		switch (chan->channel) {
		case AD9910_CHANNEL_SINGLE_TONE:
			tmp32 = FIELD_GET(AD9910_PROFILE_ST_FTW_MSK,
					  st->reg[AD9910_REG_PROFILE(st->profile)].val64);
			break;
		case AD9910_CHANNEL_DRG_RAMP_UP:
			tmp32 = FIELD_GET(AD9910_DRG_LIMIT_UPPER_MSK,
					  st->reg[AD9910_REG_DRG_LIMIT].val64);
			break;
		case AD9910_CHANNEL_DRG_RAMP_DOWN:
			tmp32 = FIELD_GET(AD9910_DRG_LIMIT_LOWER_MSK,
					  st->reg[AD9910_REG_DRG_LIMIT].val64);
			break;
		default:
			return -EINVAL;
		}
		tmp64 = (u64)tmp32 * st->data.sysclk_freq_hz;
		*val = upper_32_bits(tmp64);
		*val2 = upper_32_bits((u64)lower_32_bits(tmp64) * MICRO);
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_PHASE:
		switch (chan->channel) {
		case AD9910_CHANNEL_SINGLE_TONE:
			tmp64 = FIELD_GET(AD9910_PROFILE_ST_POW_MSK,
					  st->reg[AD9910_REG_PROFILE(st->profile)].val64);
			tmp32 = (tmp64 * AD9910_MAX_PHASE_MICRORAD) >> 16;
			*val = tmp32 / MICRO;
			*val2 = tmp32 % MICRO;
			return IIO_VAL_INT_PLUS_MICRO;
		case AD9910_CHANNEL_DRG_RAMP_UP:
			tmp64 = FIELD_GET(AD9910_DRG_LIMIT_UPPER_MSK,
					  st->reg[AD9910_REG_DRG_LIMIT].val64);
			tmp64 = (tmp64 * AD9910_PI_NANORAD) >> 31;
			*val = div_u64_rem(tmp64, NANO, val2);
			return IIO_VAL_INT_PLUS_NANO;
		case AD9910_CHANNEL_DRG_RAMP_DOWN:
			tmp64 = FIELD_GET(AD9910_DRG_LIMIT_LOWER_MSK,
					  st->reg[AD9910_REG_DRG_LIMIT].val64);
			tmp64 = (tmp64 * AD9910_PI_NANORAD) >> 31;
			*val = div_u64_rem(tmp64, NANO, val2);
			return IIO_VAL_INT_PLUS_NANO;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SCALE:
		switch (chan->channel) {
		case AD9910_CHANNEL_SINGLE_TONE:
			tmp64 = FIELD_GET(AD9910_PROFILE_ST_ASF_MSK,
					  st->reg[AD9910_REG_PROFILE(st->profile)].val64);
			*val = 0;
			*val2 = tmp64 * MICRO >> 14;
			return IIO_VAL_INT_PLUS_MICRO;
		case AD9910_CHANNEL_DRG_RAMP_UP:
			tmp64 = FIELD_GET(AD9910_DRG_LIMIT_UPPER_MSK,
					  st->reg[AD9910_REG_DRG_LIMIT].val64);
			*val = 0;
			*val2 = tmp64 * NANO >> 32;
			return IIO_VAL_INT_PLUS_NANO;
		case AD9910_CHANNEL_DRG_RAMP_DOWN:
			tmp64 = FIELD_GET(AD9910_DRG_LIMIT_LOWER_MSK,
					  st->reg[AD9910_REG_DRG_LIMIT].val64);
			*val = 0;
			*val2 = tmp64 * NANO >> 32;
			return IIO_VAL_INT_PLUS_NANO;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SAMP_FREQ:
		switch (chan->channel) {
		case AD9910_CHANNEL_PHY:
			*val = st->data.sysclk_freq_hz;
			return IIO_VAL_INT;
		case AD9910_CHANNEL_DRG_RAMP_UP:
			tmp32 = FIELD_GET(AD9910_DRG_RATE_INC_MSK,
					  st->reg[AD9910_REG_DRG_RATE].val32);
			break;
		case AD9910_CHANNEL_DRG_RAMP_DOWN:
			tmp32 = FIELD_GET(AD9910_DRG_RATE_DEC_MSK,
					  st->reg[AD9910_REG_DRG_RATE].val32);
			break;
		default:
			return -EINVAL;
		}
		if (!tmp32)
			return -ERANGE;
		tmp32 *= 4;
		*val = st->data.sysclk_freq_hz / tmp32;
		*val2 = div_u64((u64)(st->data.sysclk_freq_hz % tmp32) * MICRO, tmp32);
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

	guard(mutex)(&st->lock);

	switch (info) {
	case IIO_CHAN_INFO_ENABLE:
		val = !!val;
		switch (chan->channel) {
		case AD9910_CHANNEL_PARALLEL_PORT:
			tmp32 = FIELD_PREP(AD9910_CFR2_PARALLEL_DATA_PORT_EN_MSK, val);
			return ad9910_reg32_update(st, AD9910_REG_CFR2,
						   AD9910_CFR2_PARALLEL_DATA_PORT_EN_MSK,
						   tmp32, true);
		case AD9910_CHANNEL_DRG:
			tmp32 = FIELD_PREP(AD9910_CFR2_DRG_ENABLE_MSK, val);
			return ad9910_reg32_update(st, AD9910_REG_CFR2,
						   AD9910_CFR2_DRG_ENABLE_MSK,
						   tmp32, true);
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_FREQUENCY:
		if (!in_range(val, 0, st->data.sysclk_freq_hz / 2))
			return -EINVAL;

		tmp64 = ad9910_rational_scale((u64)val * MICRO + val2, BIT_ULL(32),
					      (u64)MICRO * st->data.sysclk_freq_hz);
		tmp64 = min(tmp64, U32_MAX);
		switch (chan->channel) {
		case AD9910_CHANNEL_SINGLE_TONE:
			tmp64 = FIELD_PREP(AD9910_PROFILE_ST_FTW_MSK, tmp64);
			return ad9910_reg64_update(st, AD9910_REG_PROFILE(st->profile),
						   AD9910_PROFILE_ST_FTW_MSK,
						   tmp64, true);
		case AD9910_CHANNEL_DRG_RAMP_UP:
			tmp64 = FIELD_PREP(AD9910_DRG_LIMIT_UPPER_MSK, tmp64);
			return ad9910_reg64_update(st, AD9910_REG_DRG_LIMIT,
						   AD9910_DRG_LIMIT_UPPER_MSK,
						   tmp64, true);
		case AD9910_CHANNEL_DRG_RAMP_DOWN:
			tmp64 = FIELD_PREP(AD9910_DRG_LIMIT_LOWER_MSK, tmp64);
			return ad9910_reg64_update(st, AD9910_REG_DRG_LIMIT,
						   AD9910_DRG_LIMIT_LOWER_MSK,
						   tmp64, true);
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_PHASE:
		if (val < 0 || val2 < 0)
			return -EINVAL;

		switch (chan->channel) {
		case AD9910_CHANNEL_SINGLE_TONE:
			tmp64 = (u64)val * MICRO + val2;
			if (tmp64 >= AD9910_MAX_PHASE_MICRORAD)
				return -EINVAL;

			tmp64 <<= 16;
			tmp64 = DIV_U64_ROUND_CLOSEST(tmp64, AD9910_MAX_PHASE_MICRORAD);
			tmp64 = min(tmp64, AD9910_POW_MAX);
			tmp64 = FIELD_PREP(AD9910_PROFILE_ST_POW_MSK, tmp64);
			return ad9910_reg64_update(st, AD9910_REG_PROFILE(st->profile),
						   AD9910_PROFILE_ST_POW_MSK,
						   tmp64, true);
		case AD9910_CHANNEL_DRG_RAMP_UP:
			tmp64 = (u64)val * NANO + val2;
			if (tmp64 > 2ULL * AD9910_PI_NANORAD)
				return -EINVAL;

			tmp64 <<= 31;
			tmp64 = DIV_U64_ROUND_CLOSEST(tmp64, AD9910_PI_NANORAD);
			tmp64 = min(tmp64, U32_MAX);
			tmp64 = FIELD_PREP(AD9910_DRG_LIMIT_UPPER_MSK, tmp64);
			return ad9910_reg64_update(st, AD9910_REG_DRG_LIMIT,
						   AD9910_DRG_LIMIT_UPPER_MSK,
						   tmp64, true);
		case AD9910_CHANNEL_DRG_RAMP_DOWN:
			tmp64 = (u64)val * NANO + val2;
			if (tmp64 > 2ULL * AD9910_PI_NANORAD)
				return -EINVAL;

			tmp64 <<= 31;
			tmp64 = DIV_U64_ROUND_CLOSEST(tmp64, AD9910_PI_NANORAD);
			tmp64 = min(tmp64, U32_MAX);
			tmp64 = FIELD_PREP(AD9910_DRG_LIMIT_LOWER_MSK, tmp64);
			return ad9910_reg64_update(st, AD9910_REG_DRG_LIMIT,
						   AD9910_DRG_LIMIT_LOWER_MSK,
						   tmp64, true);
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SCALE:
		if (val < 0 || val > 1 || (val == 1 && val2 > 0))
			return -EINVAL;

		switch (chan->channel) {
		case AD9910_CHANNEL_SINGLE_TONE:
			tmp64 = ((u64)val * MICRO + val2) << 14;
			tmp64 = DIV_U64_ROUND_CLOSEST(tmp64, MICRO);
			tmp64 = min(tmp64, AD9910_ASF_MAX);
			tmp64 = FIELD_PREP(AD9910_PROFILE_ST_ASF_MSK, tmp64);
			return ad9910_reg64_update(st, AD9910_REG_PROFILE(st->profile),
						   AD9910_PROFILE_ST_ASF_MSK,
						   tmp64, true);
		case AD9910_CHANNEL_DRG_RAMP_UP:
			tmp64 = ((u64)val * NANO + val2) << 32;
			tmp64 = DIV_U64_ROUND_CLOSEST(tmp64, NANO);
			tmp64 = min(tmp64, U32_MAX);
			tmp64 = FIELD_PREP(AD9910_DRG_LIMIT_UPPER_MSK, tmp64);
			return ad9910_reg64_update(st, AD9910_REG_DRG_LIMIT,
						   AD9910_DRG_LIMIT_UPPER_MSK,
						   tmp64, true);
		case AD9910_CHANNEL_DRG_RAMP_DOWN:
			tmp64 = ((u64)val * NANO + val2) << 32;
			tmp64 = DIV_U64_ROUND_CLOSEST(tmp64, NANO);
			tmp64 = min(tmp64, U32_MAX);
			tmp64 = FIELD_PREP(AD9910_DRG_LIMIT_LOWER_MSK, tmp64);
			return ad9910_reg64_update(st, AD9910_REG_DRG_LIMIT,
						   AD9910_DRG_LIMIT_LOWER_MSK,
						   tmp64, true);
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SAMP_FREQ:
		if (chan->channel == AD9910_CHANNEL_PHY)
			return ad9910_set_sysclk_freq(st, val, true);

		tmp64 = ((u64)val * MICRO + val2) * 4;
		if (!tmp64)
			return -EINVAL;

		tmp64 = DIV64_U64_ROUND_CLOSEST((u64)st->data.sysclk_freq_hz * MICRO, tmp64);
		tmp32 = clamp(tmp64, 1U, AD9910_STEP_RATE_MAX);

		switch (chan->channel) {
		case AD9910_CHANNEL_DRG_RAMP_UP:
			tmp32 = FIELD_PREP(AD9910_DRG_RATE_INC_MSK, tmp32);
			return ad9910_reg32_update(st, AD9910_REG_DRG_RATE,
						   AD9910_DRG_RATE_INC_MSK,
						   tmp32, true);
		case AD9910_CHANNEL_DRG_RAMP_DOWN:
			tmp32 = FIELD_PREP(AD9910_DRG_RATE_DEC_MSK, tmp32);
			return ad9910_reg32_update(st, AD9910_REG_DRG_RATE,
						   AD9910_DRG_RATE_DEC_MSK,
						   tmp32, true);
		default:
			return -EINVAL;
		}
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
		return IIO_VAL_INT_PLUS_MICRO;
	case IIO_CHAN_INFO_PHASE:
	case IIO_CHAN_INFO_SCALE:
		switch (chan->channel) {
		case AD9910_CHANNEL_SINGLE_TONE:
			return IIO_VAL_INT_PLUS_MICRO;
		case AD9910_CHANNEL_DRG_RAMP_UP:
		case AD9910_CHANNEL_DRG_RAMP_DOWN:
			return IIO_VAL_INT_PLUS_NANO;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_SAMP_FREQ:
		if (chan->channel == AD9910_CHANNEL_PHY)
			return IIO_VAL_INT;
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
	bool high32 = FIELD_GET(AD9910_REG_HIGH32_FLAG_MSK, reg);

	/*
	 * HIGH32 flag is a workaround to allow access to upper 32 bits of
	 * 64-bit registers one at a time due to debugfs_reg_access limitations
	 * of only supporting 32-bit values.
	 */
	reg &= ~AD9910_REG_HIGH32_FLAG_MSK;
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
				FIELD_MODIFY(GENMASK_ULL(63, 32), &tmp64, writeval);
			else
				FIELD_MODIFY(GENMASK_ULL(31, 0), &tmp64, writeval);

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

static int ad9910_cfg_sysclk(struct ad9910_state *st, bool update)
{
	u32 tmp32, cfr3 = AD9910_CFR3_OPEN_MSK;

	cfr3 |= AD9910_CFR3_VCO_SEL_MSK |
		FIELD_PREP(AD9910_CFR3_DRV0_MSK, st->data.refclk_out_drv);

	if (st->data.pll_enabled) {
		tmp32 = st->data.pll_charge_pump_current - AD9910_ICP_MIN_uA;
		tmp32 = DIV_ROUND_CLOSEST(tmp32, AD9910_ICP_STEP_uA);
		cfr3 |= FIELD_PREP(AD9910_CFR3_ICP_MSK, tmp32) |
			AD9910_CFR3_PLL_EN_MSK;
	} else {
		cfr3 |= AD9910_CFR3_ICP_MSK |
			AD9910_CFR3_REFCLK_DIV_RESETB_MSK |
			AD9910_CFR3_PFD_RESET_MSK;
	}
	st->reg[AD9910_REG_CFR3].val32 = cfr3;

	return ad9910_set_sysclk_freq(st, AD9910_PLL_OUT_MAX_FREQ_HZ, update);
}

static int ad9910_parse_fw(struct ad9910_state *st)
{
	struct device *dev = &st->spi->dev;
	u32 tmp;
	int ret;

	st->data.pll_enabled = device_property_read_bool(dev, "adi,pll-enable");
	if (st->data.pll_enabled) {
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
	ret = ad9910_reg32_write(st, AD9910_REG_CFR1, reg32, false);
	if (ret)
		return ret;

	reg32 = AD9910_CFR2_AMP_SCALE_SINGLE_TONE_MSK;
	reg32 |= AD9910_CFR2_SYNC_TIMING_VAL_DISABLE_MSK |
		 AD9910_CFR2_DRG_NO_DWELL_MSK |
		 AD9910_CFR2_DATA_ASM_HOLD_LAST_MSK |
		 AD9910_CFR2_SYNC_CLK_EN_MSK |
		 AD9910_CFR2_PDCLK_ENABLE_MSK;
	ret = ad9910_reg32_write(st, AD9910_REG_CFR2, reg32, false);
	if (ret)
		return ret;

	ret = ad9910_cfg_sysclk(st, false);
	if (ret)
		return ret;

	ret = ad9910_set_dac_current(st, false);
	if (ret)
		return ret;

	/* configure step rate with default values */
	reg32 = FIELD_PREP(AD9910_DRG_RATE_DEC_MSK, 1) |
		FIELD_PREP(AD9910_DRG_RATE_INC_MSK, 1);
	ret = ad9910_reg32_write(st, AD9910_REG_DRG_RATE, reg32, false);
	if (ret)
		return ret;

	return ad9910_io_update(st);
}

static void ad9910_release(void *data)
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
	struct reset_control *dev_rst;
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

	st->refclk = devm_clk_get_enabled(dev, "ref_clk");
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
	io_rst_gpio = devm_gpiod_get_optional(dev, "io-reset", GPIOD_OUT_LOW);
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

	ret = devm_iio_device_register(dev, indio_dev);
	if (ret)
		return ret;

	return devm_add_action_or_reset(dev, ad9910_release, st);
}

static const struct spi_device_id ad9910_id[] = {
	{ "ad9910" },
	{ }
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
