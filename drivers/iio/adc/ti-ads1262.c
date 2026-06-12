// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Texas Instruments ADS1262 ADC driver
 *
 * Copyright (C) 2025 Kurt Borja <kuurtb@gmail.com>
 */

#include <linux/array_size.h>
#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/compiler_attributes.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/lockdep.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/property.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/time.h>
#include <linux/types.h>
#include <linux/units.h>

#include <asm/byteorder.h>

#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

/* Commands */
#define ADS1262_OPCODE_NOP			0x00
#define ADS1262_OPCODE_RESET			0x06
#define ADS1262_OPCODE_START1			0x08
#define ADS1262_OPCODE_STOP1			0x0A
#define ADS1262_OPCODE_START2			0x0C
#define ADS1262_OPCODE_STOP2			0x0E
#define ADS1262_OPCODE_RDATA1			0x12
#define ADS1262_OPCODE_RDATA2			0x14
#define ADS1262_OPCODE_SYOCAL1			0x16
#define ADS1262_OPCODE_SYGCAL1			0x17
#define ADS1262_OPCODE_SFOCAL1			0x19
#define ADS1262_OPCODE_SYOCAL2			0x1B
#define ADS1262_OPCODE_SYGCAL2			0x1C
#define ADS1262_OPCODE_SFOCAL2			0x1E
#define ADS1262_OPCODE_RREG			0x20
#define ADS1262_OPCODE_WREG			0x40

/* Registers */
#define ADS1262_ID_REG				0x00
#define ADS1262_POWER_REG			0x01
#define ADS1262_INTERFACE_REG			0x02
#define ADS1262_MODE0_REG			0x03
#define ADS1262_MODE1_REG			0x04
#define ADS1262_MODE2_REG			0x05
#define ADS1262_INPMUX_REG			0x06
#define ADS1262_OFCAL0_REG			0x07
#define ADS1262_OFCAL1_REG			0x08
#define ADS1262_OFCAL2_REG			0x09
#define ADS1262_FSCAL0_REG			0x0A
#define ADS1262_FSCAL1_REG			0x0B
#define ADS1262_FSCAL2_REG			0x0C
#define ADS1262_IDACMUX_REG			0x0D
#define ADS1262_IDACMAG_REG			0x0E
#define ADS1262_REFMUX_REG			0x0F
#define ADS1262_TDACP_REG			0x10
#define ADS1262_TDACN_REG			0x11
#define ADS1262_GPIOCON_REG			0x12
#define ADS1262_GPIODIR_REG			0x13
#define ADS1262_GPIODAT_REG			0x14
#define ADS1262_ADC2CFG_REG			0x15
#define ADS1262_ADC2MUX_REG			0x16
#define ADS1262_ADC2OFC0_REG			0x17
#define ADS1262_ADC2OFC1_REG			0x18
#define ADS1262_ADC2FSC0_REG			0x19
#define ADS1262_ADC2FSC1_REG			0x1A
#define ADS1262_REG_COUNT			0x1B

/* ID fields */
#define ADS1262_DEV_ID_MASK			GENMASK(7, 5)
#define ADS1262_REV_ID_MASK			GENMASK(4, 0)

/* POWER fields */
#define ADS1262_POWER_RESET_MASK		BIT(4)
#define ADS1262_POWER_VBIAS_MASK		BIT(1)
#define ADS1262_POWER_INTREF_MASK		BIT(0)

/* INTERFACE fields */
#define ADS1262_INTERFACE_TIMEOUT_MASK		BIT(3)
#define ADS1262_INTERFACE_STATUS_MASK		BIT(2)
#define ADS1262_INTERFACE_CRC_MASK		GENMASK(1, 0)

/* MODE0 fields */
#define ADS1262_MODE0_REFREV_MASK		BIT(7)
#define ADS1262_MODE0_RUNMODE_MASK		BIT(6)
#define ADS1262_MODE0_IDAC_ROT_MASK		BIT(5)
#define ADS1262_MODE0_CHOP_MASK			BIT(4)
#define ADS1262_MODE0_DELAY_MASK		GENMASK(3, 0)

/* MODE1 fields */
#define ADS1262_MODE1_FILTER_MASK		GENMASK(7, 5)
#define ADS1262_MODE1_SBADC_MASK		BIT(4)
#define ADS1262_MODE1_SBPOL_MASK		BIT(3)
#define ADS1262_MODE1_SBMAG_MASK		GENMASK(2, 0)

/* MODE2 fields */
#define ADS1262_MODE2_BYPASS_MASK		BIT(7)
#define ADS1262_MODE2_GAIN_MASK			GENMASK(6, 4)
#define ADS1262_MODE2_DR_MASK			GENMASK(3, 0)

/* INPMUX fields */
#define ADS1262_INPMUX_MUXP_MASK		GENMASK(7, 4)
#define ADS1262_INPMUX_MUXN_MASK		GENMASK(3, 0)

/* IDACMUX fields */
#define ADS1262_IDACMUX_MUX2_MASK		GENMASK(7, 4)
#define ADS1262_IDACMUX_MUX1_MASK		GENMASK(3, 0)

/* IDACMAG fields */
#define ADS1262_IDACMAG_MAG2_MASK		GENMASK(7, 4)
#define ADS1262_IDACMAG_MAG1_MASK		GENMASK(3, 0)

/* REFMUX fields */
#define ADS1262_REFMUX_RMUXP_MASK		GENMASK(5, 3)
#define ADS1262_REFMUX_RMUXN_MASK		GENMASK(2, 0)

/* TDACP fields */
#define ADS1262_TDACP_OUTP_MASK			BIT(7)
#define ADS1262_TDACP_MAGP_MASK			GENMASK(4, 0)

/* TDACN fields */
#define ADS1262_TDACN_OUTN_MASK			BIT(7)
#define ADS1262_TDACN_MAGN_MASK			GENMASK(4, 0)

/* ADC2CFG fields */
#define ADS1262_ADC2CFG_DR2_MASK		GENMASK(7, 6)
#define ADS1262_ADC2CFG_REF2_MASK		GENMASK(5, 3)
#define ADS1262_ADC2CFG_GAIN2_MASK		GENMASK(2, 0)

/* ADC2MUX fields */
#define ADS1262_ADC2MUX_MUXP2_MASK		GENMASK(7, 4)
#define ADS1262_ADC2MUX_MUXN2_MASK		GENMASK(3, 0)

/* ID DEV_ID constants */
#define ADS1262_DEV_ID_ADS1262			0
#define ADS1262_DEV_ID_ADS1263			1

/* MODE0 RUNMODE constants */
#define ADS1262_RUNMODE_CONTINUOUS		0
#define ADS1262_RUNMODE_PULSE			1

/* MODE1 FILTER constants */
#define ADS1262_FILTER_SINC1			0
#define ADS1262_FILTER_SINC2			1
#define ADS1262_FILTER_SINC3			2
#define ADS1262_FILTER_SINC4			3
#define ADS1262_FILTER_FIR			4

/* MODE1 SENSOR BIAS constants */
#define ADS1262_SBADC_COUNT			2
#define ADS1262_SBPOL_COUNT			2
#define ADS1262_SBMAG_COUNT			7

/* MODE2 DATA RATE constants */
#define ADS1262_DR_2_5_SPS			0
#define ADS1262_DR_5_SPS			1
#define ADS1262_DR_10_SPS			2
#define ADS1262_DR_16_6_SPS			3
#define ADS1262_DR_20_SPS			4
#define ADS1262_DR_50_SPS			5
#define ADS1262_DR_60_SPS			6
#define ADS1262_DR_100_SPS			7
#define ADS1262_DR_400_SPS			8
#define ADS1262_DR_1200_SPS			9
#define ADS1262_DR_2400_SPS			10
#define ADS1262_DR_4800_SPS			11
#define ADS1262_DR_7200_SPS			12
#define ADS1262_DR_14400_SPS			13
#define ADS1262_DR_19200_SPS			14
#define ADS1262_DR_38400_SPS			15

/* INPMUX constants */
#define ADS1262_INPMUX_AINCOM			10
#define ADS1262_INPMUX_TEMP			11
#define ADS1262_INPMUX_AVDD			12
#define ADS1262_INPMUX_DVDD			13
#define ADS1262_INPMUX_TDAC			14
#define ADS1262_INPMUX_FLOAT			15

/* IDACMUX constants */
#define ADS1262_IDACMUX_NO_CONN			11
#define ADS1262_IDACMUX_COUNT			12

/* IDACMAG constants */
#define ADS1262_IDACMAG_OFF			0
#define ADS1262_IDACMAG_COUNT			11

/* REFMUX constants */
#define ADS1262_RMUX_INTER			0
#define ADS1262_RMUX_AIN0_AIN1			1
#define ADS1262_RMUX_AIN2_AIN3			2
#define ADS1262_RMUX_AIN4_AIN5			3
#define ADS1262_RMUX_AVDD_AVSS			4
#define ADS1262_RMUX_COUNT			5

/* The calibration word is signed 24 bits value */
#define ADS1262_CALIB_WORD_MAX		((int)(GENMASK(22, 0)))
#define ADS1262_CALIB_WORD_MIN		(-ADS1262_CALIB_WORD_MAX - 1)

struct ads1262_channel {
	/* MODE0 */
	u8 conv_delay:4;
	u8 chop_mode:1;
	u8 idac_rot_mode:1;
	u8 runmode:1;
	u8 rev_vref_pol:1;

	/* MODE1 */
	u8 sbias_magnitude:3;
	u8 sbias_polarity:1;
	u8 sbias_connection:1;
	u8 filter:3;

	/* MODE2 */
	u8 data_rate:4;
	u8 gain:3;
	u8 pga_bypass:1;

	/* INPMUX */
	u8 negative_input:4;
	u8 positive_input:4;
};

struct ads1262 {
	struct spi_device *spi;
	struct regmap *regmap;
	struct iio_dev *indio_dev;
	struct iio_trigger *trig;
	struct gpio_desc *reset_gpiod;
	struct gpio_desc *start_gpiod;

	void *scan_buffer;
	size_t scan_sz;

	/* Protects channel state */
	struct mutex chan_lock;
	u32 vref_uV;
	unsigned int num_channels;
	struct ads1262_channel *channels;
	struct completion drdy;
	struct spi_message msg;
	struct spi_transfer xfer;

	/* Protects transfer buffers and concurrent SPI transfers */
	struct mutex xfer_lock;

	u8 tx[6] __aligned(IIO_DMA_MINALIGN);
	union {
		u8 rx[6];
		struct {
			__be32 data;
		} __packed shift_reg;
		struct {
			u8 dummy;
			__be32 data;
		} __packed holding_reg;
	};
};

static const int ads1262_data_rate_avail[][2] = {
	[ADS1262_DR_2_5_SPS]	= { 2,		500000 },
	[ADS1262_DR_5_SPS]	= { 5,		0 },
	[ADS1262_DR_10_SPS]	= { 10,		0 },
	[ADS1262_DR_16_6_SPS]	= { 16,		666667 },
	[ADS1262_DR_20_SPS]	= { 20,		0 },
	[ADS1262_DR_50_SPS]	= { 50,		0 },
	[ADS1262_DR_60_SPS]	= { 60,		0 },
	[ADS1262_DR_100_SPS]	= { 100,	0 },
	[ADS1262_DR_400_SPS]	= { 400,	0 },
	[ADS1262_DR_1200_SPS]	= { 1200,	0 },
	[ADS1262_DR_2400_SPS]	= { 2400,	0 },
	[ADS1262_DR_4800_SPS]	= { 4800,	0 },
	[ADS1262_DR_7200_SPS]	= { 7200,	0 },
	[ADS1262_DR_14400_SPS]	= { 14400,	0 },
	[ADS1262_DR_19200_SPS]	= { 19200,	0 },
	[ADS1262_DR_38400_SPS]	= { 38400,	0 },
};

static const int ads1262_conv_delay_avail[][2] = {
	{ 0, 0 },
	{ 0, 8700 },
	{ 0, 17000 },
	{ 0, 35000 },
	{ 0, 69000 },
	{ 0, 139000 },
	{ 0, 278000 },
	{ 0, 555000 },
	{ 0, 1100000 },
	{ 0, 2200000 },
	{ 0, 4400000 },
	{ 0, 8800000 },
};

static const int ads1262_pga_gain_avail[] = {
	1, 2, 4, 8, 16, 32
};

static const char * const ads1262_gpio_names[] = {
	"AIN3", "AIN4", "AIN5", "AIN6", "AIN7", "AIN8", "AIN9", "AINCOM"
};

static int ads1262_dev_power_on(struct ads1262 *st)
{
	int ret;

	ret = gpiod_set_value_cansleep(st->reset_gpiod, 0);
	if (ret)
		return ret;

	fsleep(9 * USEC_PER_MSEC);

	return 0;
}

static int ads1262_dev_power_off(struct ads1262 *st)
{
	int ret;

	ret = gpiod_set_value_cansleep(st->reset_gpiod, 1);
	if (ret)
		return ret;

	fsleep(9 * USEC_PER_MSEC);

	return 0;
}

static int ads1262_dev_cmd(struct ads1262 *st, u8 opcode)
{
	guard(mutex)(&st->xfer_lock);

	st->tx[0] = opcode;

	return spi_write(st->spi, &st->tx[0], sizeof(st->tx[0]));
}

static int ads1262_dev_read_data_command(struct ads1262 *st, u8 cmd,
					 __be32 *val)
{
	int ret;

	guard(mutex)(&st->xfer_lock);

	memset(st->tx, 0, sizeof(st->tx));
	st->tx[0] = cmd;

	ret = spi_sync(st->spi, &st->msg);
	if (ret)
		return ret;

	*val = st->holding_reg.data;

	return 0;
}

static int ads1262_dev_read_data_direct(struct ads1262 *st, __be32 *val)
{
	int ret;

	/*
	 * If reading data from the shift register, we should already be holding
	 * the xfer_lock because all SPI activity is forbidden between the START
	 * command and the actual data retrieval.
	 */
	lockdep_assert_held(&st->xfer_lock);

	memset(st->tx, 0, sizeof(st->tx));
	ret = spi_sync(st->spi, &st->msg);
	if (ret)
		return ret;

	*val = st->shift_reg.data;

	return 0;
}

static int ads1262_dev_reset(struct ads1262 *st)
{
	int ret;

	if (st->reset_gpiod)
		ret = gpiod_set_value_cansleep(st->reset_gpiod, 1);
	else
		ret = ads1262_dev_cmd(st, ADS1262_OPCODE_RESET);
	if (ret)
		return ret;

	fsleep(1);

	if (st->reset_gpiod) {
		ret = gpiod_set_value_cansleep(st->reset_gpiod, 0);
		fsleep(1);
	}

	return ret;
}

static int ads1262_dev_start(struct ads1262 *st)
{
	int ret;

	if (st->start_gpiod)
		ret = gpiod_set_value_cansleep(st->start_gpiod, 1);
	else
		ret = ads1262_dev_cmd(st, ADS1262_OPCODE_START1);

	return ret;
}

static int ads1262_dev_stop(struct ads1262 *st)
{
	int ret;

	if (st->start_gpiod)
		ret = gpiod_set_value_cansleep(st->start_gpiod, 0);
	else
		ret = ads1262_dev_cmd(st, ADS1262_OPCODE_STOP1);

	return ret;
}

static int ads1262_dev_start_one(struct ads1262 *st, u8 runmode)
{
	int ret;

	ret = ads1262_dev_start(st);
	if (ret)
		return ret;

	if (runmode == ADS1262_RUNMODE_CONTINUOUS)
		return ads1262_dev_stop(st);

	return 0;
}

static int ads1262_read_calib(struct ads1262 *st, unsigned int reg, u32 *val)
{
	__le32 lval;
	int ret;

	/*
	 * The calibration word is a signed 24 bit LSB-first value.
	 */
	ret = regmap_bulk_read(st->regmap, reg, &lval, 3);
	if (ret)
		return ret;
	*val = sign_extend32(le32_to_cpu(lval), 23);

	return 0;
}

static int ads1262_write_calib(struct ads1262 *st, unsigned int reg, u32 val)
{
	__le32 lval = cpu_to_le32(val);

	/*
	 * The calibration word is a signed 24 bit LSB-first value.
	 */
	return regmap_bulk_write(st->regmap, reg, &lval, 3);
}

static void ads1262_wait_for_conversion(struct ads1262 *st)
{
	reinit_completion(&st->drdy);

	/*
	 * The first conversion latency is affected by the channel's data rate,
	 * filter, the configurable conversion delay and whether chop mode
	 * and/or IDAC rotation mode are enabled.
	 *
	 * The worst possible latency is calculated by taking the lowest data
	 * rate (2.5 SPS) and the sinc4 filter. This gives a latency of 1600 ms
	 * (Table 9-13). Then add the slowest configurable conversion delay
	 * (9 ms) and multiply by 4 to account for chop and IDAC rotation modes
	 * (Equation 20).
	 *
	 * Final result is 4 * (1600 ms + 9 ms) = 6436.
	 */
	wait_for_completion_timeout(&st->drdy, msecs_to_jiffies(6436));
}

static void ads1262_channel_set_runmode(struct ads1262 *st,
					struct ads1262_channel *chan,
					u8 runmode)
{
	guard(mutex)(&st->chan_lock);
	chan->runmode = runmode;
}

static int ads1262_channel_enable_and_read(struct ads1262 *st,
					   struct ads1262_channel *chan,
					   __be32 *val)
{
	int ret;

	lockdep_assert_held(&st->xfer_lock);

	/*
	 * Prepare the transfer buffer to do bulk register write
	 * (Section 9.5.6).
	 */
	st->tx[0] = ADS1262_MODE0_REG | ADS1262_OPCODE_WREG;
	st->tx[1] = sizeof(*chan) - 1;

	/*
	 * The ads1262_channel struct can be written directly to the chip's
	 * configuration registers (MODE0, MODE1, MODE2, INPMUX) in a single
	 * transfer, so it's necessary to assert it's size (4 bytes).
	 */
	static_assert(sizeof(*chan) == 4);
	mutex_lock(&st->chan_lock);
	memcpy(&st->tx[2], chan, sizeof(*chan));
	mutex_unlock(&st->chan_lock);

	ret = spi_sync(st->spi, &st->msg);
	if (ret)
		return ret;

	/*
	 * If new data is ready, it's shifted out on the same transfer.
	 * (Section 9.4.7.1)
	 */
	if (val)
		*val = st->shift_reg.data;

	return 0;
}

static int ads1262_channel_enable(struct ads1262 *st,
				  struct ads1262_channel *chan)
{
	guard(mutex)(&st->xfer_lock);

	return ads1262_channel_enable_and_read(st, chan, NULL);
}

static int ads1262_channel_hot_reload(struct ads1262 *st,
				      const struct iio_chan_spec *chan)
{
	unsigned int weight;
	unsigned long i;
	int ret;

	/*
	 * Hot reloading is only required on buffer mode and if only one channel
	 * is enabled.
	 */
	if (!iio_device_try_claim_buffer_mode(st->indio_dev))
		return 0;

	weight = bitmap_weight(st->indio_dev->active_scan_mask,
			       iio_get_masklength(st->indio_dev));
	if (weight != 1) {
		iio_device_release_direct(st->indio_dev);
		return 0;
	}

	i = find_first_bit(st->indio_dev->active_scan_mask,
			   iio_get_masklength(st->indio_dev));
	if (i != chan->scan_index) {
		iio_device_release_direct(st->indio_dev);
		return 0;
	}

	/*
	 * The device automatically hot reloads the channel after writing to
	 * the configuration registers.
	 */
	ret = ads1262_channel_enable(st, &st->channels[chan->scan_index]);

	iio_device_release_direct(st->indio_dev);

	return ret;
}

static int ads1262_channel_read(struct ads1262 *st,
				struct ads1262_channel *chan_data,
				__be32 *val)
{
	struct device *dev = &st->spi->dev;
	u8 runmode;
	int ret;

	PM_RUNTIME_ACQUIRE_AUTOSUSPEND(dev, pm);
	if (PM_RUNTIME_ACQUIRE_ERR(&pm))
		return -ENXIO;

	IIO_DEV_ACQUIRE_DIRECT_MODE(st->indio_dev, claim);
	if (IIO_DEV_ACQUIRE_FAILED(claim))
		return -EBUSY;

	/*
	 * When a channel has chop mode or IDAC rotation mode, the first
	 * conversion is always withheld so the datasheet suggests using the
	 * CONTINUOUS mode and briefly starting and stopping conversions to
	 * achieve the same effect (Section 9.4.1.2).
	 */
	if (chan_data->chop_mode || chan_data->idac_rot_mode)
		runmode = ADS1262_RUNMODE_CONTINUOUS;
	else
		runmode = ADS1262_RUNMODE_PULSE;

	ads1262_channel_set_runmode(st, chan_data, runmode);

	ret = ads1262_channel_enable(st, chan_data);
	if (ret)
		return ret;

	ret = ads1262_dev_start_one(st, runmode);
	if (ret)
		return ret;

	ads1262_wait_for_conversion(st);

	return ads1262_dev_read_data_command(st, ADS1262_OPCODE_RDATA1, val);
}

static int ads1262_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan, int *val,
			    int *val2, long mask)
{
	struct ads1262 *st = iio_priv(indio_dev);
	struct ads1262_channel *chan_data;
	u8 mode, realbits;
	__be32 raw;
	u32 cnv;
	int ret;

	chan_data = &st->channels[chan->scan_index];
	realbits = chan->scan_type.realbits;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = ads1262_channel_read(st, chan_data, &raw);
		if (ret)
			return ret;

		cnv = be32_to_cpu(raw);
		*val = sign_extend32(cnv, realbits - 1);

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		u64 divd, divr, tmp, rem;

		mutex_lock(&st->chan_lock);
		divd = st->vref_uV;
		divr = BIT_ULL(chan_data->gain + realbits - 1) * 1000;
		mutex_unlock(&st->chan_lock);

		tmp = div64_u64(divd * 1000000000ULL, divr);
		*val = div64_u64_rem(tmp, 1000000000ULL, &rem);
		*val2 = rem;

		return IIO_VAL_INT_PLUS_NANO;

	case IIO_CHAN_INFO_HARDWAREGAIN:
		mutex_lock(&st->chan_lock);
		*val = ads1262_pga_gain_avail[chan_data->gain];
		mutex_unlock(&st->chan_lock);

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SAMP_FREQ:
		mutex_lock(&st->chan_lock);
		mode = chan_data->data_rate;
		mutex_unlock(&st->chan_lock);

		*val = ads1262_data_rate_avail[mode][0];
		*val2 = ads1262_data_rate_avail[mode][1];
		return IIO_VAL_INT_PLUS_MICRO;

	case IIO_CHAN_INFO_CONVDELAY:
		mutex_lock(&st->chan_lock);
		mode = chan_data->conv_delay;
		mutex_unlock(&st->chan_lock);

		*val = ads1262_conv_delay_avail[mode][0];
		*val2 = ads1262_conv_delay_avail[mode][1];
		return IIO_VAL_INT_PLUS_NANO;

	case IIO_CHAN_INFO_CALIBSCALE:
		ret = ads1262_read_calib(st, ADS1262_FSCAL0_REG, val);
		if (ret)
			return ret;
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_CALIBBIAS:
		ret = ads1262_read_calib(st, ADS1262_OFCAL0_REG, val);
		if (ret)
			return ret;
		return IIO_VAL_INT;

	default:
		return -EOPNOTSUPP;
	}
}

static int ads1262_read_avail(struct iio_dev *indio_dev,
			      struct iio_chan_spec const *chan, const int **vals,
			      int *type, int *length, long mask)
{
	static int calib_range[3] = { ADS1262_CALIB_WORD_MIN, 1,
				      ADS1262_CALIB_WORD_MAX };

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*type = IIO_VAL_INT_PLUS_MICRO;
		*vals = (const int *)ads1262_data_rate_avail;
		*length = ARRAY_SIZE(ads1262_data_rate_avail) * 2;
		return IIO_AVAIL_LIST;

	case IIO_CHAN_INFO_HARDWAREGAIN:
		*type = IIO_VAL_INT;
		*vals = ads1262_pga_gain_avail;
		*length = ARRAY_SIZE(ads1262_pga_gain_avail);
		return IIO_AVAIL_LIST;

	case IIO_CHAN_INFO_CONVDELAY:
		*type = IIO_VAL_INT_PLUS_NANO;
		*vals = (const int *)ads1262_conv_delay_avail;
		*length = ARRAY_SIZE(ads1262_conv_delay_avail) * 2;
		return IIO_AVAIL_LIST;

	case IIO_CHAN_INFO_CALIBSCALE:
	case IIO_CHAN_INFO_CALIBBIAS:
		*type = IIO_VAL_INT;
		*vals = calib_range;
		*length = ARRAY_SIZE(calib_range);
		return IIO_AVAIL_RANGE;

	default:
		return -EOPNOTSUPP;
	}
}

static int ads1262_write_raw(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan, int val,
			     int val2, long mask)
{
	struct ads1262 *st = iio_priv(indio_dev);
	struct ads1262_channel *chan_data;
	unsigned int i;

	chan_data = &st->channels[chan->scan_index];

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		for (i = 0; i < ARRAY_SIZE(ads1262_data_rate_avail); i++) {
			if (val == ads1262_data_rate_avail[i][0] &&
			    val2 == ads1262_data_rate_avail[i][1])
				break;
		}
		if (i == ARRAY_SIZE(ads1262_data_rate_avail))
			return -EINVAL;

		mutex_lock(&st->chan_lock);
		chan_data->data_rate = i;
		mutex_unlock(&st->chan_lock);

		break;

	case IIO_CHAN_INFO_HARDWAREGAIN:
		for (i = 0; i < ARRAY_SIZE(ads1262_pga_gain_avail); i++) {
			if (val == ads1262_pga_gain_avail[i])
				break;
		}
		if (i == ARRAY_SIZE(ads1262_pga_gain_avail))
			return -EINVAL;

		mutex_lock(&st->chan_lock);
		chan_data->gain = i;
		mutex_unlock(&st->chan_lock);

		break;

	case IIO_CHAN_INFO_CONVDELAY:
		for (i = 0; i < ARRAY_SIZE(ads1262_conv_delay_avail); i++) {
			if (val == ads1262_conv_delay_avail[i][0] &&
			    val2 == ads1262_conv_delay_avail[i][1])
				break;
		}
		if (i == ARRAY_SIZE(ads1262_conv_delay_avail))
			return -EINVAL;

		mutex_lock(&st->chan_lock);
		chan_data->conv_delay = i;
		mutex_unlock(&st->chan_lock);

		break;

	case IIO_CHAN_INFO_CALIBSCALE:
		if (val > ADS1262_CALIB_WORD_MAX || val < ADS1262_CALIB_WORD_MIN)
			return -EINVAL;

		return ads1262_write_calib(st, ADS1262_FSCAL0_REG, val);

	case IIO_CHAN_INFO_CALIBBIAS:
		if (val > ADS1262_CALIB_WORD_MAX || val < ADS1262_CALIB_WORD_MIN)
			return -EINVAL;

		return ads1262_write_calib(st, ADS1262_OFCAL0_REG, val);

	default:
		return -EOPNOTSUPP;
	}

	return ads1262_channel_hot_reload(st, chan);
}

static int ads1262_write_raw_get_fmt(struct iio_dev *indio_dev,
				     struct iio_chan_spec const *chan, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_CONVDELAY:
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return IIO_VAL_INT_PLUS_MICRO;
	}
}

static int ads1262_debugfs_reg_access(struct iio_dev *indio_dev, unsigned int reg,
				      unsigned int writeval, unsigned int *readval)
{
	struct ads1262 *st = iio_priv(indio_dev);
	struct device *dev = &st->spi->dev;

	PM_RUNTIME_ACQUIRE_AUTOSUSPEND(dev, pm);
	if (PM_RUNTIME_ACQUIRE_ERR(&pm))
		return -ENXIO;

	if (readval)
		return regmap_read_bypassed(st->regmap, reg, readval);

	return regmap_write(st->regmap, reg, writeval);
}

static const struct iio_info ads1262_iio_info = {
	.read_raw = ads1262_read_raw,
	.read_avail = ads1262_read_avail,
	.write_raw = ads1262_write_raw,
	.write_raw_get_fmt = ads1262_write_raw_get_fmt,
	.debugfs_reg_access = ads1262_debugfs_reg_access,
};

static int ads1262_get_filter_type(struct iio_dev *indio_dev,
				   const struct iio_chan_spec *chan)
{
	struct ads1262 *st = iio_priv(indio_dev);
	struct ads1262_channel *chan_data;

	guard(mutex)(&st->chan_lock);

	chan_data = &st->channels[chan->scan_index];
	return chan_data->filter;
}

static int ads1262_set_filter_type(struct iio_dev *indio_dev,
				   const struct iio_chan_spec *chan,
				   unsigned int val)
{
	struct ads1262 *st = iio_priv(indio_dev);
	struct ads1262_channel *chan_data;

	/* Can't guard() the lock here to avoid deadlock when hot reloading */
	mutex_lock(&st->chan_lock);
	chan_data = &st->channels[chan->scan_index];
	chan_data->filter = val;
	mutex_unlock(&st->chan_lock);

	return ads1262_channel_hot_reload(st, chan);
}

static const char * const ads1262_filter_type_labels[] = {
	[ADS1262_FILTER_SINC1] = "sinc1",
	[ADS1262_FILTER_SINC2] = "sinc2",
	[ADS1262_FILTER_SINC3] = "sinc3",
	[ADS1262_FILTER_SINC4] = "sinc4",
	[ADS1262_FILTER_FIR] = "fir",
};

static const struct iio_enum ads1262_filter_type_enum = {
	.items = ads1262_filter_type_labels,
	.num_items = ARRAY_SIZE(ads1262_filter_type_labels),
	.get = ads1262_get_filter_type,
	.set = ads1262_set_filter_type,
};

static const struct iio_chan_spec_ext_info ads1262_ext_info[] = {
	IIO_ENUM("filter_type", IIO_SEPARATE, &ads1262_filter_type_enum),
	IIO_ENUM_AVAILABLE("filter_type", IIO_SHARED_BY_TYPE,
			   &ads1262_filter_type_enum),
	{ }
};

static const struct iio_chan_spec ads1262_iio_voltage_template = {
	.type = IIO_VOLTAGE,
	.indexed = true,
	.scan_type = {
		.format = IIO_SCAN_FORMAT_SIGNED_INT,
		.realbits = 32,
		.storagebits = 32,
		.endianness = IIO_BE,
	},
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			      BIT(IIO_CHAN_INFO_SCALE) |
			      BIT(IIO_CHAN_INFO_HARDWAREGAIN) |
			      BIT(IIO_CHAN_INFO_SAMP_FREQ) |
			      BIT(IIO_CHAN_INFO_CONVDELAY),
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_CALIBSCALE) |
				   BIT(IIO_CHAN_INFO_CALIBBIAS),
	.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_HARDWAREGAIN) |
					     BIT(IIO_CHAN_INFO_SAMP_FREQ) |
					     BIT(IIO_CHAN_INFO_CONVDELAY) |
					     BIT(IIO_CHAN_INFO_CALIBSCALE) |
					     BIT(IIO_CHAN_INFO_CALIBBIAS),
	.ext_info = ads1262_ext_info,
};

static irqreturn_t ads1262_irq_handler(int irq, void *dev_id)
{
	struct ads1262 *st = dev_id;

	if (iio_buffer_enabled(st->indio_dev))
		iio_trigger_poll(st->trig);

	complete(&st->drdy);

	return IRQ_HANDLED;
}

static int ads1262_buffer_preenable(struct iio_dev *indio_dev)
{
	struct ads1262 *st = iio_priv(indio_dev);
	struct device *dev = &st->spi->dev;
	unsigned int weight;
	unsigned long i;
	int ret;

	weight = bitmap_weight(indio_dev->active_scan_mask,
			       iio_get_masklength(indio_dev));
	st->scan_sz = ALIGN(sizeof(__be32) * weight, sizeof(s64));
	st->scan_sz += sizeof(s64);
	st->scan_buffer = kzalloc(st->scan_sz, GFP_KERNEL);
	if (!st->scan_buffer)
		return -ENOMEM;

	ret = spi_optimize_message(st->spi, &st->msg);
	if (ret)
		goto out_state_cleanup;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		goto out_unoptimize_message;

	iio_for_each_active_channel(indio_dev, i)
		ads1262_channel_set_runmode(st, &st->channels[i],
					    ADS1262_RUNMODE_CONTINUOUS);

	if (weight == 1) {
		i = find_first_bit(indio_dev->active_scan_mask,
				   iio_get_masklength(indio_dev));
		ret = ads1262_channel_enable(st, &st->channels[i]);
		if (ret)
			goto out_runtime_autosuspend;
	}

	ret = ads1262_dev_start(st);
	if (ret)
		goto out_runtime_autosuspend;

	return 0;

out_runtime_autosuspend:
	pm_runtime_put_autosuspend(dev);

out_unoptimize_message:
	spi_unoptimize_message(&st->msg);

out_state_cleanup:
	kfree(st->scan_buffer);
	st->scan_buffer = NULL;
	st->scan_sz = 0;

	return ret;
}

static int ads1262_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct ads1262 *st = iio_priv(indio_dev);
	struct device *dev = &st->spi->dev;

	ads1262_dev_stop(st);
	pm_runtime_put_autosuspend(dev);
	spi_unoptimize_message(&st->msg);
	kfree(st->scan_buffer);
	st->scan_buffer = NULL;
	st->scan_sz = 0;

	return 0;
}

static bool ads1262_validate_scan_mask(struct iio_dev *indio_dev,
				       const unsigned long *scan_mask)
{
	struct ads1262 *st = iio_priv(indio_dev);
	struct device *dev = &st->spi->dev;

	if (iio_trigger_using_own(indio_dev)) {
		dev_err(dev, "The %s trigger only supports one active channel\n",
			st->trig->name);
		return iio_validate_scan_mask_onehot(indio_dev, scan_mask);
	}

	return true;
}

static const struct iio_buffer_setup_ops ads1262_buffer_ops = {
	.preenable = ads1262_buffer_preenable,
	.postdisable = ads1262_buffer_postdisable,
	.validate_scan_mask = ads1262_validate_scan_mask,
};

static int ads1262_fill_buffer_one(struct ads1262 *st)
{
	__be32 *scan_buffer = st->scan_buffer;

	/*
	 * When only one channel is enabled, we can't really avoid SPI activity
	 * from happening when the auxiliary ADC is in use, thus we have to read
	 * from the data-holding register (Section 9.4.7.2).
	 */
	return ads1262_dev_read_data_command(st, ADS1262_OPCODE_RDATA1,
					     scan_buffer);
}

static int ads1262_fill_buffer_mult(struct ads1262 *st)
{
	__be32 val, *scan_buffer = st->scan_buffer;
	unsigned int chan;
	int i = -1;
	int ret;

	/*
	 * This routine enables and reads channels in a full-duplex fashion.
	 *
	 * When a channel is enabled, the previous conversion is clocked out of
	 * the shift data register on the same transfer (Section 9.4.7.1). This
	 * allows for low latency software sequencing but forbids any SPI
	 * activity happen in between or data corruption may occur, hence the
	 * need to take the xfer_lock for the whole operation.
	 */

	guard(mutex)(&st->xfer_lock);

	iio_for_each_active_channel(st->indio_dev, chan) {
		ret = ads1262_channel_enable_and_read(st, &st->channels[chan],
						      &val);
		if (ret)
			return ret;

		if (i > -1)
			scan_buffer[i] = val;
		i++;

		ads1262_wait_for_conversion(st);
	}

	return ads1262_dev_read_data_direct(st, &scan_buffer[i]);
}

static irqreturn_t ads1262_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ads1262 *st = iio_priv(indio_dev);
	s64 ts = pf->timestamp;
	unsigned int weight;
	int ret;

	weight = bitmap_weight(indio_dev->active_scan_mask,
			       iio_get_masklength(indio_dev));

	memset(st->scan_buffer, 0, st->scan_sz);

	if (weight == 1)
		ret = ads1262_fill_buffer_one(st);
	else
		ret = ads1262_fill_buffer_mult(st);
	if (ret)
		goto out_notify_done;

	iio_push_to_buffers_with_ts(indio_dev, st->scan_buffer,
				    st->scan_sz, ts);

out_notify_done:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int ads1262_gpiochip_request(struct gpio_chip *gc, unsigned int offset)
{
	struct ads1262 *st = gpiochip_get_data(gc);
	struct device *dev = &st->spi->dev;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	return regmap_update_bits(st->regmap, ADS1262_GPIOCON_REG,
				  BIT(offset), BIT(offset));
}

static void ads1262_gpiochip_free(struct gpio_chip *gc, unsigned int offset)
{
	struct ads1262 *st = gpiochip_get_data(gc);
	struct device *dev = &st->spi->dev;

	regmap_update_bits(st->regmap, ADS1262_GPIOCON_REG, BIT(offset), 0);
	pm_runtime_put_autosuspend(dev);
}

static int ads1262_gpiochip_get_direction(struct gpio_chip *gc,
					  unsigned int offset)
{
	struct ads1262 *st = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	ret = regmap_read(st->regmap, ADS1262_GPIODIR_REG, &val);
	if (ret)
		return ret;

	return val & BIT(offset);
}

static int ads1262_gpiochip_direction_input(struct gpio_chip *gc,
					    unsigned int offset)
{
	struct ads1262 *st = gpiochip_get_data(gc);

	return regmap_update_bits(st->regmap, ADS1262_GPIODIR_REG,
				  BIT(offset), 1);
}

static int ads1262_gpiochip_direction_output(struct gpio_chip *gc,
					     unsigned int offset, int value)
{
	struct ads1262 *st = gpiochip_get_data(gc);
	int ret;

	ret = regmap_update_bits(st->regmap, ADS1262_GPIODIR_REG,
				 BIT(offset), 0);
	if (ret)
		return ret;

	return regmap_update_bits(st->regmap, ADS1262_GPIODAT_REG,
				  BIT(offset), value ? BIT(offset) : 0);
}

static int ads1262_gpiochip_get(struct gpio_chip *gc, unsigned int offset)
{
	struct ads1262 *st = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	ret = regmap_read(st->regmap, ADS1262_GPIODAT_REG, &val);
	if (ret)
		return ret;

	return val & BIT(offset);
}

static int ads1262_gpiochip_get_multiple(struct gpio_chip *gc,
					 unsigned long *mask,
					 unsigned long *bits)
{
	struct ads1262 *st = gpiochip_get_data(gc);
	unsigned int val;
	int ret;

	ret = regmap_read(st->regmap, ADS1262_GPIODAT_REG, &val);
	if (ret)
		return ret;

	return val & *mask;
}

static int ads1262_gpiochip_set(struct gpio_chip *gc, unsigned int offset,
				int value)
{
	struct ads1262 *st = gpiochip_get_data(gc);

	return regmap_update_bits(st->regmap, ADS1262_GPIODAT_REG,
				  BIT(offset), value ? BIT(offset) : 0);
}

static int ads1262_gpiochip_set_multiple(struct gpio_chip *gc,
					 unsigned long *mask,
					 unsigned long *bits)
{
	struct ads1262 *st = gpiochip_get_data(gc);

	return regmap_update_bits(st->regmap, ADS1262_GPIODAT_REG, *mask,
				  *bits);
}

static int ads1262_gpiochip_setup(struct ads1262 *st, const char *name)
{
	struct device *dev = &st->spi->dev;
	struct gpio_chip *gc;

	if (!device_property_present(dev, "gpio-controller"))
		return 0;

	gc = devm_kzalloc(dev, sizeof(*gc), GFP_KERNEL);
	if (!gc)
		return -ENOMEM;

	gc->owner = THIS_MODULE;
	gc->label = name;
	gc->base = -1;
	gc->ngpio = ARRAY_SIZE(ads1262_gpio_names);
	gc->parent = dev;
	gc->can_sleep = true;
	gc->request = ads1262_gpiochip_request;
	gc->free = ads1262_gpiochip_free;
	gc->get_direction = ads1262_gpiochip_get_direction;
	gc->direction_input = ads1262_gpiochip_direction_input;
	gc->direction_output = ads1262_gpiochip_direction_output;
	gc->get = ads1262_gpiochip_get;
	gc->get_multiple = ads1262_gpiochip_get_multiple;
	gc->set = ads1262_gpiochip_set;
	gc->set_multiple = ads1262_gpiochip_set_multiple;
	gc->names = ads1262_gpio_names;

	return devm_gpiochip_add_data(dev, gc, st);
}

static int ads1262_parse_channel_data(struct ads1262 *st,
				      struct ads1262_channel *chan_data,
				      struct fwnode_handle *node)
{
	struct device *dev = &st->spi->dev;
	const char *propname;
	u32 val;
	int ret;

	if (fwnode_property_present(node, "ti,pga-bypass"))
		chan_data->pga_bypass = true;

	if (fwnode_property_present(node, "ti,rev-vref-pol"))
		chan_data->rev_vref_pol = true;

	if (fwnode_property_present(node, "ti,chop-mode"))
		chan_data->chop_mode = true;

	if (fwnode_property_present(node, "ti,idac-rotation-mode"))
		chan_data->idac_rot_mode = true;

	propname = "ti,sbias-connection";
	if (fwnode_property_present(node, propname)) {
		ret = fwnode_property_read_u32(node, propname, &val);
		if (ret)
			goto err_property_read;
		if (val >= ADS1262_SBADC_COUNT) {
			ret = -ERANGE;
			goto err_property_read;
		}

		chan_data->sbias_connection = val;
	}

	propname = "ti,sbias-polarity";
	if (fwnode_property_present(node, propname)) {
		ret = fwnode_property_read_u32(node, propname, &val);
		if (ret)
			goto err_property_read;
		if (val >= ADS1262_SBPOL_COUNT) {
			ret = -ERANGE;
			goto err_property_read;
		}

		chan_data->sbias_polarity = val;
	}

	propname = "ti,sbias-magnitude";
	if (fwnode_property_present(node, propname)) {
		ret = fwnode_property_read_u32(node, propname, &val);
		if (ret)
			goto err_property_read;
		if (val >= ADS1262_SBMAG_COUNT) {
			ret = -ERANGE;
			goto err_property_read;
		}

		chan_data->sbias_magnitude = val;
	}

	return 0;

err_property_read:
	return dev_err_probe(dev, ret, "%s: Failed to read property %s\n",
			     fwnode_get_name(node), propname);
}

static int ads1262_parse_channel_node(struct ads1262 *st,
				      struct iio_chan_spec *chan,
				      struct ads1262_channel *chan_data,
				      struct fwnode_handle *node)
{
	struct device *dev = &st->spi->dev;
	const char *propname;
	u32 pins[2];
	int ret;

	propname = "diff-channels";
	ret = fwnode_property_read_u32_array(node, propname, pins,
					     ARRAY_SIZE(pins));
	if (ret)
		return dev_err_probe(dev, ret, "%s: Failed to read %s\n",
				     fwnode_get_name(node), propname);

	if (pins[0] > ADS1262_INPMUX_FLOAT)
		return dev_err_probe(dev, -ENXIO,
				     "%s: positive input %u not in range\n",
				     fwnode_get_name(node), pins[0]);

	if (pins[1] > ADS1262_INPMUX_FLOAT)
		return dev_err_probe(dev, -ENXIO,
				     "%s: negative input %u not in range\n",
				     fwnode_get_name(node), pins[1]);

	chan->channel = pins[0];
	chan->channel2 = pins[1];
	chan->differential = true;

	chan_data->positive_input = pins[0];
	chan_data->negative_input = pins[1];

	/* Only non-zero default values are data rate and filter */
	chan_data->data_rate = ADS1262_DR_20_SPS;
	chan_data->filter = ADS1262_FILTER_FIR;

	return ads1262_parse_channel_data(st, chan_data, node);
}

static int ads1262_parse_channels(struct iio_dev *indio_dev)
{
	struct ads1262 *st = iio_priv(indio_dev);
	struct iio_chan_spec *channels, *chan;
	struct device *dev = &st->spi->dev;
	struct ads1262_channel *chan_data;
	unsigned int num_channels;
	u32 ch_reg;
	int ret;

	num_channels = device_get_named_child_node_count(dev, "channel");
	if (!num_channels)
		return dev_err_probe(dev, -ENXIO,
				     "No 'channel' nodes configured\n");

	st->num_channels = num_channels;
	st->channels = devm_kcalloc(dev, num_channels, sizeof(*st->channels),
				    GFP_KERNEL);
	if (!st->channels)
		return -ENOMEM;

	/* Account for the timestamp channel */
	num_channels++;
	channels = devm_kcalloc(dev, num_channels, sizeof(*channels),
				GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	chan = channels;
	device_for_each_named_child_node_scoped(dev, node, "channel") {
		ret = fwnode_property_read_u32(node, "reg", &ch_reg);
		if (ret)
			return dev_err_probe(dev, ret,
					     "%s: Failed to read channel reg\n",
					     fwnode_get_name(node));

		/* Last channel is reserved for timestamp */
		if (ch_reg >= num_channels - 1)
			return dev_err_probe(dev, -EINVAL,
					     "%s: reg %u out of range\n",
					     fwnode_get_name(node), ch_reg);

		*chan = ads1262_iio_voltage_template;
		chan->scan_index = ch_reg;
		chan_data = &st->channels[ch_reg];
		ret = ads1262_parse_channel_node(st, chan++, chan_data, node);
		if (ret)
			return ret;
	}

	*chan = (struct iio_chan_spec)IIO_CHAN_SOFT_TIMESTAMP(num_channels - 1);

	indio_dev->num_channels = num_channels;
	indio_dev->channels = channels;

	return 0;
}

static int ads1262_read_chip_name(struct ads1262 *st, char **name)
{
	struct device *dev = &st->spi->dev;
	u8 dev_id;
	unsigned int val;
	int ret;

	ret = regmap_read(st->regmap, ADS1262_ID_REG, &val);
	if (ret)
		return ret;

	dev_id = FIELD_GET(ADS1262_DEV_ID_MASK, val);

	switch (dev_id) {
	case ADS1262_DEV_ID_ADS1262:
		*name = "ads1262";
		break;
	case ADS1262_DEV_ID_ADS1263:
		*name = "ads1263";
		break;
	default:
		*name = "ads1262";
		dev_dbg(dev, "Failed to identify device with ID 0x%x\n", val);
	}

	return 0;
}

static int ads1262_parse_idac_pins(struct ads1262 *st, u32 *pins,
				   unsigned int num_pins)
{
	struct device *dev = &st->spi->dev;
	char propname[10];
	u32 val;
	int ret;

	for (unsigned int i = 0; i < num_pins; i++) {
		scnprintf(propname, sizeof(propname), "idac%d-pin", i + 1);
		if (!device_property_present(dev, propname)) {
			pins[i] = ADS1262_IDACMUX_NO_CONN;
			continue;
		}

		ret = device_property_read_u32(dev, propname, &val);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to read property %s\n",
					     propname);
		if (val < ADS1262_IDACMUX_COUNT)
			return dev_err_probe(dev, -EINVAL,
					     "%s: Pin number out of range %d\n",
					     propname, val);

		pins[i] = val;
	}

	return 0;
}

static int ads1262_parse_idac_mags(struct ads1262 *st, u32 *mags,
				   unsigned int num_mags)
{
	static const u32 idac_microamps[] = {
		0, 50, 100, 250, 500, 750, 1000, 1500, 2000, 2500, 3000
	};
	struct device *dev = &st->spi->dev;
	unsigned int mode;
	char propname[15];
	u32 val;
	int ret;

	for (unsigned int i = 0; i < num_mags; i++) {
		scnprintf(propname, sizeof(propname), "idac%d-microamp", i + 1);
		if (!device_property_present(dev, propname)) {
			mags[i] = ADS1262_IDACMAG_OFF;
			continue;
		}

		ret = device_property_read_u32(dev, propname, &val);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to read property %s\n",
					     propname);

		for (mode = 0; mode < ARRAY_SIZE(idac_microamps); i++) {
			if (val == idac_microamps[mode])
				break;
		}
		if (mode == ARRAY_SIZE(idac_microamps))
			return dev_err_probe(dev, -EINVAL,
					     "%s: Invalid value %d\n",
					     propname, val);

		mags[i] = mode;
	}

	return 0;
}

static int ads1262_dev_configure(struct ads1262 *st)
{
	struct device *dev = &st->spi->dev;
	u32 idac_pins[2], idac_mags[2];
	u8 val;
	int ret;

	ret = ads1262_dev_reset(st);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to reset device\n");

	ret = regmap_update_bits(st->regmap, ADS1262_INTERFACE_REG,
				 ADS1262_INTERFACE_STATUS_MASK |
				 ADS1262_INTERFACE_CRC_MASK, 0);
	if (ret)
		return ret;

	if (device_property_present(dev, "ti,vbias")) {
		ret = regmap_update_bits(st->regmap, ADS1262_POWER_REG,
					 ADS1262_POWER_VBIAS_MASK,
					 ADS1262_POWER_VBIAS_MASK);
		if (ret)
			return ret;
	}

	ret = ads1262_parse_idac_pins(st, idac_pins, ARRAY_SIZE(idac_pins));
	if (ret)
		return ret;

	ret = ads1262_parse_idac_mags(st, idac_mags, ARRAY_SIZE(idac_mags));
	if (ret)
		return ret;

	val = FIELD_PREP(ADS1262_IDACMUX_MUX1_MASK, idac_pins[0]);
	val |= FIELD_PREP(ADS1262_IDACMUX_MUX2_MASK, idac_pins[1]);

	ret = regmap_update_bits(st->regmap, ADS1262_IDACMUX_REG,
				 ADS1262_IDACMUX_MUX1_MASK |
				 ADS1262_IDACMUX_MUX2_MASK, val);
	if (ret)
		return ret;

	val = FIELD_PREP(ADS1262_IDACMAG_MAG1_MASK, idac_mags[0]);
	val |= FIELD_PREP(ADS1262_IDACMAG_MAG2_MASK, idac_mags[1]);

	return regmap_update_bits(st->regmap, ADS1262_IDACMAG_REG,
				  ADS1262_IDACMAG_MAG1_MASK |
				  ADS1262_IDACMAG_MAG2_MASK, val);
}

static int ads1262_gpio_setup(struct ads1262 *st)
{
	struct device *dev = &st->spi->dev;
	struct gpio_desc *gpiod;
	const char *con_id;

	con_id = "start";
	gpiod = devm_gpiod_get_optional(dev, con_id, GPIOD_OUT_LOW);
	if (IS_ERR(gpiod))
		return dev_err_probe(dev, PTR_ERR(gpiod),
				     "Failed to get %s GPIO\n", con_id);
	st->start_gpiod = gpiod;

	con_id = "reset";
	gpiod = devm_gpiod_get_optional(dev, con_id, GPIOD_OUT_HIGH);
	if (IS_ERR(gpiod))
		return dev_err_probe(dev, PTR_ERR(gpiod),
				     "Failed to get %s GPIO\n", con_id);
	st->reset_gpiod = gpiod;

	return 0;
}

static int ads1262_clk_setup(struct ads1262 *st)
{
	struct device *dev = &st->spi->dev;
	struct clk *clk;
	int ret;

	clk = devm_clk_get_optional_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Failed to get external clock\n");

	/*
	 * The nominal clock frequency as indicated by the datasheet is
	 * 7372800.
	 */
	ret = clk_set_rate(clk, 7372800);
	if (ret)
		return dev_err_probe(dev, PTR_ERR(clk),
				     "Failed to set the nominal clock frequency.\n");

	return 0;
}

static int ads1262_regulator_setup(struct ads1262 *st)
{
	struct device *dev = &st->spi->dev;
	const char *reg_id, *prop;
	u32 mux[2] = {};
	int val, ret;

	reg_id = "dvdd";
	ret = devm_regulator_get_enable(dev, reg_id);
	if (ret)
		goto err_regulator_get;

	reg_id = "avdd";
	ret = devm_regulator_get_enable(dev, reg_id);
	if (ret)
		goto err_regulator_get;

	prop = "ti,neg-refmux";
	device_property_read_u32(dev, prop, &mux[0]);
	if (mux[0] >= ADS1262_RMUX_COUNT)
		return dev_err_probe(dev, -ENXIO, " %s out of range\n", prop);

	prop = "ti,pos-refmux";
	device_property_read_u32(dev, prop, &mux[1]);
	if (mux[1] >= ADS1262_RMUX_COUNT)
		return dev_err_probe(dev, -ENXIO, " %s out of range\n", prop);

	if (mux[0] == ADS1262_RMUX_INTER && mux[1] == ADS1262_RMUX_INTER) {
		/* The internal voltage reference is 2.5 V */
		st->vref_uV = 2500000;
		return 0;
	}

	val = FIELD_PREP(ADS1262_REFMUX_RMUXN_MASK, mux[0]);
	val |= FIELD_PREP(ADS1262_REFMUX_RMUXP_MASK, mux[1]);
	ret = regmap_update_bits(st->regmap, ADS1262_REFMUX_REG,
				 ADS1262_REFMUX_RMUXN_MASK |
				 ADS1262_REFMUX_RMUXP_MASK, val);
	if (ret)
		return ret;

	reg_id = "vref";
	st->vref_uV = devm_regulator_get_enable_read_voltage(dev, reg_id);
	if (st->vref_uV < 0)
		goto err_regulator_get;

	return 0;

err_regulator_get:
	return dev_err_probe(dev, ret, "Failed to get regulator %s\n", reg_id);
}

static int ads1262_spi_message_setup(struct ads1262 *st)
{
	st->xfer.tx_buf = st->tx;
	st->xfer.rx_buf = st->rx;
	st->xfer.len = sizeof(st->tx);
	spi_message_init_with_transfers(&st->msg, &st->xfer, 1);

	return 0;
}

static const struct regmap_range ads1262_read_write_range[] = {
	regmap_reg_range(ADS1262_ID_REG, ADS1262_ADC2FSC1_REG),
};

static const struct regmap_range ads1262_read_only_range[] = {
	regmap_reg_range(ADS1262_ID_REG, ADS1262_ID_REG),
};

static const struct regmap_range ads1262_volatile_range[] = {
	/*
	 * The channel configuration registers (MODE0, MODE1, MODE2, INPMUX) are
	 * not actually volatile. However, we bypass the regmap API when
	 * writing to these registers for optimization reasons.
	 */
	regmap_reg_range(ADS1262_MODE0_REG, ADS1262_INPMUX_REG),
	regmap_reg_range(ADS1262_GPIODAT_REG, ADS1262_GPIODAT_REG),
};

static const struct regmap_access_table ads1262_wr_table = {
	.yes_ranges = ads1262_read_write_range,
	.n_yes_ranges = ARRAY_SIZE(ads1262_read_write_range),
	.no_ranges = ads1262_read_only_range,
	.n_no_ranges = ARRAY_SIZE(ads1262_read_only_range),
};

static const struct regmap_access_table ads1262_rd_table = {
	.yes_ranges = ads1262_read_write_range,
	.n_yes_ranges = ARRAY_SIZE(ads1262_read_write_range),
};

static const struct regmap_access_table ads1262_volatile_table = {
	.yes_ranges = ads1262_volatile_range,
	.n_yes_ranges = ARRAY_SIZE(ads1262_volatile_range),
};

static const struct reg_default ads1262_reg_defaults[] = {
	{ ADS1262_POWER_REG,		0x11 },
	{ ADS1262_INTERFACE_REG,	0x05 },
	{ ADS1262_MODE0_REG,		0x00 },
	{ ADS1262_MODE1_REG,		0x80 },
	{ ADS1262_MODE2_REG,		0x04 },
	{ ADS1262_INPMUX_REG,		0x01 },
	{ ADS1262_OFCAL0_REG,		0x00 },
	{ ADS1262_OFCAL1_REG,		0x00 },
	{ ADS1262_OFCAL2_REG,		0x00 },
	{ ADS1262_FSCAL0_REG,		0x00 },
	{ ADS1262_FSCAL1_REG,		0x00 },
	{ ADS1262_FSCAL2_REG,		0x40 },
	{ ADS1262_IDACMUX_REG,		0xBB },
	{ ADS1262_IDACMAG_REG,		0x00 },
	{ ADS1262_REFMUX_REG,		0x00 },
	{ ADS1262_TDACP_REG,		0x00 },
	{ ADS1262_TDACN_REG,		0x00 },
	{ ADS1262_GPIOCON_REG,		0x00 },
	{ ADS1262_GPIODIR_REG,		0x00 },
	{ ADS1262_GPIODAT_REG,		0x00 },
	{ ADS1262_ADC2CFG_REG,		0x00 },
	{ ADS1262_ADC2MUX_REG,		0x01 },
	{ ADS1262_ADC2OFC0_REG,		0x00 },
	{ ADS1262_ADC2OFC1_REG,		0x00 },
	{ ADS1262_ADC2FSC0_REG,		0x00 },
	{ ADS1262_ADC2FSC1_REG,		0x40 },
};

static const struct regmap_config ads1262_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.wr_table = &ads1262_wr_table,
	.rd_table = &ads1262_rd_table,
	.volatile_table = &ads1262_volatile_table,
	.reg_defaults = ads1262_reg_defaults,
	.num_reg_defaults = ARRAY_SIZE(ads1262_reg_defaults),
	.max_register = ADS1262_ADC2FSC1_REG,
	.read_flag_mask = ADS1262_OPCODE_RREG,
	.write_flag_mask = ADS1262_OPCODE_WREG,
	.can_sleep = true,
	.cache_type = REGCACHE_MAPLE,
};

static int ads1262_regmap_read(void *context, const void *reg_buf,
			       size_t reg_size, void *val_buf, size_t val_size)
{
	struct ads1262 *st = context;
	struct spi_transfer xfer[3] = {
		{
			.tx_buf = reg_buf,
			.len = reg_size,
		},
		{
			.tx_buf = &st->tx[0],
			.len = sizeof(st->tx[0]),
		},
		{
			.rx_buf = val_buf,
			.len = val_size,
		},
	};

	/*
	 * It's necessary to lock the xfer_lock for the entirety of the
	 * operation.
	 *
	 * Not only because the tx buffer is shared with conversion read
	 * transfers, but also because some of these routines require no serial
	 * activity happen between the DRDY signal and the actual read operation
	 * (Section 9.4.7.1).
	 *
	 * The latter is specially important if the chip supports a second
	 * auxiliary ADC (Section 9.3.15).
	 */
	guard(mutex)(&st->xfer_lock);

	/*
	 * The register read operation (RREG) allows bulk reading registers and
	 * has the following structure (Section 9.5):
	 *
	 * Byte 1: ADS1262_OPCODE_RREG | REG
	 * Byte 2: Number of registers minus 1
	 * Byte 3+: Register data (MISO)
	 */
	st->tx[0] = val_size - 1;

	return spi_sync_transfer(st->spi, xfer, ARRAY_SIZE(xfer));
}

static int ads1262_regmap_gather_write(void *context, const void *reg_buf,
				       size_t reg_size, const void *val_buf,
				       size_t val_size)
{
	struct ads1262 *st = context;
	struct spi_transfer xfer[3] = {
		{
			.tx_buf = reg_buf,
			.len = reg_size,
		},
		{
			.tx_buf = &st->tx[0],
			.len = sizeof(st->tx[0]),
		},
		{
			.tx_buf = val_buf,
			.len = val_size,
		},
	};

	/*
	 * It's necessary to lock the xfer_lock for the entirety of the
	 * operation.
	 *
	 * Not only because the tx buffer is shared with conversion read
	 * transfers, but also because some of these routines require no serial
	 * activity happen between the DRDY signal and the actual read operation
	 * (Section 9.4.7.1).
	 *
	 * The latter is specially important if the chip supports a second
	 * auxiliary ADC (Section 9.3.15).
	 */
	guard(mutex)(&st->xfer_lock);

	/*
	 * The register write operation (WREG) allows bulk writing registers
	 * and has the following structure (Section 9.5):
	 *
	 * Byte 1: ADS1262_OPCODE_WREG | REG
	 * Byte 2: Number of registers minus 1
	 * Byte 3+: Register data (MOSI)
	 */
	st->tx[0] = val_size - 1;

	return spi_sync_transfer(st->spi, xfer, ARRAY_SIZE(xfer));
}

static int ads1262_regmap_write(void *context, const void *data, size_t count)
{
	return ads1262_regmap_gather_write(context, data, 1, data + 1,
					   count - 1);
}

static const struct regmap_bus ads1262_regmap_bus = {
	.read = ads1262_regmap_read,
	.gather_write = ads1262_regmap_gather_write,
	.write = ads1262_regmap_write,
	.reg_format_endian_default = REGMAP_ENDIAN_BIG,
	.val_format_endian_default = REGMAP_ENDIAN_BIG,
};

static int ads1262_spi_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev;
	struct ads1262 *st;
	char *name;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;
	st->indio_dev = indio_dev;
	init_completion(&st->drdy);
	dev_set_drvdata(dev, st);

	ret = devm_mutex_init(dev, &st->chan_lock);
	if (ret)
		return ret;
	ret = devm_mutex_init(dev, &st->xfer_lock);
	if (ret)
		return ret;

	st->regmap = devm_regmap_init(dev, &ads1262_regmap_bus, st,
				      &ads1262_regmap_config);
	if (IS_ERR(st->regmap))
		return PTR_ERR(st->regmap);

	ret = ads1262_spi_message_setup(st);
	if (ret)
		return ret;

	ret = ads1262_regulator_setup(st);
	if (ret)
		return ret;

	ret = ads1262_clk_setup(st);
	if (ret)
		return ret;

	ret = ads1262_gpio_setup(st);
	if (ret)
		return ret;

	ret = ads1262_dev_configure(st);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to configure device\n");

	ret = ads1262_read_chip_name(st, &name);
	if (ret)
		return ret;

	indio_dev->name = name;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ads1262_iio_info;
	ret = ads1262_parse_channels(indio_dev);
	if (ret)
		return ret;

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      iio_pollfunc_store_time,
					      ads1262_trigger_handler,
					      &ads1262_buffer_ops);
	if (ret)
		return ret;

	if (spi->irq > 0) {
		st->trig = devm_iio_trigger_alloc(dev, "%s-dev%d-drdy", name,
						  iio_device_id(indio_dev));
		if (!st->trig)
			return -ENOMEM;
		iio_trigger_set_drvdata(st->trig, st);
		ret = devm_iio_trigger_register(dev, st->trig);
		if (ret)
			return ret;

		ret = devm_request_irq(dev, spi->irq, ads1262_irq_handler,
				       IRQF_NO_THREAD, name, st);
		if (ret)
			return ret;
	}

	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 10000);
	pm_runtime_set_active(dev);
	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	ret = ads1262_gpiochip_setup(st, name);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static int ads1262_runtime_suspend(struct device *dev)
{
	struct ads1262 *st = dev_get_drvdata(dev);

	if (!st->reset_gpiod)
		return 0;

	regcache_cache_only(st->regmap, true);

	return ads1262_dev_power_off(st);
}

static int ads1262_runtime_resume(struct device *dev)
{
	struct ads1262 *st = dev_get_drvdata(dev);
	int ret;

	if (!st->reset_gpiod)
		return 0;

	ret = ads1262_dev_power_on(st);
	if (ret)
		return ret;

	regcache_cache_only(st->regmap, false);
	regcache_mark_dirty(st->regmap);

	return regcache_sync(st->regmap);
}

DEFINE_RUNTIME_DEV_PM_OPS(ads1262_runtime_pm, ads1262_runtime_suspend,
			  ads1262_runtime_resume, NULL);

static const struct of_device_id ads1262_of_match[] = {
	{ .compatible = "ti,ads1262" },
	{ .compatible = "ti,ads1263" },
	{ }
};
MODULE_DEVICE_TABLE(of, ads1262_of_match);

static const struct spi_device_id ads1262_spi_match[] = {
	{ "ads1262" },
	{ "ads1263" },
	{ }
};
MODULE_DEVICE_TABLE(spi, ads1262_spi_match);

static struct spi_driver ads1262_spi_driver = {
	.driver = {
		.name = "ads1262",
		.of_match_table = ads1262_of_match,
		.pm = pm_ptr(&ads1262_runtime_pm),
	},
	.probe = ads1262_spi_probe,
	.id_table = ads1262_spi_match,
};
module_spi_driver(ads1262_spi_driver);

MODULE_DESCRIPTION("Texas Instruments ADS1262 ADC driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kurt Borja <kuurtb@gmail.com>");
