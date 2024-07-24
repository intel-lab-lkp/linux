// SPDX-License-Identifier: GPL-2.0+
/*
 * AD7770, AD7771, AD7779 ADC
 *
 * Copyright 2023-2024 Analog Devices Inc.
 */

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/crc8.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <linux/string.h>
#include <linux/units.h>

#include <asm/unaligned.h>

#include <linux/iio/iio.h>
#include <linux/iio/buffer.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

#define AD7779_SPI_READ_CMD			BIT(7)

#define AD7779_DISABLE_SD			BIT(7)

#define AD7779_REG_CH_DISABLE			0x08
#define AD7779_REG_CH_SYNC_OFFSET(ch)		(0x09 + (ch))
#define AD7779_REG_CH_CONFIG(ch)		(0x00 + (ch))
#define AD7779_REG_GENERAL_USER_CONFIG_1	0x11
#define AD7779_REG_GENERAL_USER_CONFIG_2	0x12
#define AD7779_REG_GENERAL_USER_CONFIG_3	0x13
#define AD7779_REG_DOUT_FORMAT			0x14
#define AD7779_REG_ADC_MUX_CONFIG		0x15
#define AD7779_REG_GPIO_CONFIG			0x17
#define AD7779_REG_BUFFER_CONFIG_1		0x19
#define AD7779_REG_GLOBAL_MUX_CONFIG		0x16
#define AD7779_REG_BUFFER_CONFIG_2		0x1A
#define AD7779_REG_GPIO_DATA			0x18
#define AD7779_REG_CH_OFFSET_UPPER_BYTE(ch)	(0x1C + (ch) * 6)
#define AD7779_REG_CH_OFFSET_LOWER_BYTE(ch)	(0x1E + (ch) * 6)
#define AD7779_REG_CH_GAIN_UPPER_BYTE(ch)	(0x1F + (ch) * 6)
#define AD7779_REG_CH_OFFSET_MID_BYTE(ch)	(0x1D + (ch) * 6)
#define AD7779_REG_CH_GAIN_MID_BYTE(ch)		(0x20 + (ch) * 6)
#define AD7779_REG_CH_ERR_REG(ch)		(0x4C + (ch))
#define AD7779_REG_CH0_1_SAT_ERR		0x54
#define AD7779_REG_CH_GAIN_LOWER_BYTE(ch)	(0x21 + (ch) * 6)
#define AD7779_REG_CH2_3_SAT_ERR		0x55
#define AD7779_REG_CH4_5_SAT_ERR		0x56
#define AD7779_REG_CH6_7_SAT_ERR		0x57
#define AD7779_REG_CHX_ERR_REG_EN		0x58
#define AD7779_REG_GEN_ERR_REG_1		0x59
#define AD7779_REG_GEN_ERR_REG_1_EN		0x5A
#define AD7779_REG_GEN_ERR_REG_2		0x5B
#define AD7779_REG_GEN_ERR_REG_2_EN		0x5C
#define AD7779_REG_STATUS_REG_1			0x5D
#define AD7779_REG_STATUS_REG_2			0x5E
#define AD7779_REG_STATUS_REG_3			0x5F
#define AD7779_REG_SRC_N_MSB			0x60
#define AD7779_REG_SRC_N_LSB			0x61
#define AD7779_REG_SRC_IF_MSB			0x62
#define AD7779_REG_SRC_IF_LSB			0x63
#define AD7779_REG_SRC_UPDATE			0x64

#define AD7779_FILTER_MSK			BIT(6)
#define AD7779_MOD_POWERMODE_MSK		BIT(6)
#define AD7779_MOD_PDB_REFOUT_MSK		BIT(4)
#define AD7779_MOD_SPI_EN_MSK			BIT(4)

/* AD7779_REG_DOUT_FORMAT */
#define AD7779_DOUT_FORMAT_MSK			GENMASK(7, 6)
#define AD7779_DOUT_HEADER_FORMAT		BIT(5)
#define AD7779_DCLK_CLK_DIV_MSK			GENMASK(3, 1)

#define AD7779_REFMUX_CTRL_MSK			GENMASK(7, 6)
#define AD7779_SPI_CRC_EN_MSK			BIT(0)

#define AD7779_MAXCLK_LOWPOWER			4096000
#define AD7779_NUM_CHANNELS			8
#define AD7779_RESET_BUF_SIZE			8

#define AD7779_LOWPOWER_DIV			512
#define AD7779_HIGHPOWER_DIV			2048

#define AD7779_SINC3_MAXFREQ			(16 * HZ_PER_KHZ)
#define AD7779_SINC5_MAXFREQ			(128 * HZ_PER_KHZ)

#define AD7779_DEFAULT_SAMPLING_FREQ		(8 * HZ_PER_KHZ)
#define AD7779_DEFAULT_SAMPLING_2LINE		(4 * HZ_PER_KHZ)
#define AD7779_DEFAULT_SAMPLING_1LINE		(2 * HZ_PER_KHZ)

#define AD7779_SPIMODE_MAX_SAMP_FREQ		(16 * HZ_PER_KHZ)

#define GAIN_REL				0x555555
#define AD7779_FREQ_MSB_MSK			GENMASK(15, 8)
#define AD7779_FREQ_LSB_MSK			GENMASK(7, 0)
#define AD7779_UPPER				GENMASK(23, 16)
#define AD7779_MID				GENMASK(15, 8)
#define AD7779_LOWER				GENMASK(7, 0)

#define AD7779_REG_MSK		GENMASK(6, 0)

#define AD7779_CRC8_POLY			0x07
DECLARE_CRC8_TABLE(ad7779_crc8_table);

enum ad7779_filter {
	AD7779_SINC3,
	AD7779_SINC5,
};

enum ad7779_variant {
	ad7770,
	ad7771,
	AD7779,
};

enum ad7779_power_mode {
	AD7779_LOW_POWER,
	AD7779_HIGH_POWER,
};

struct ad7779_chip_info {
	const char *name;
	struct iio_chan_spec const *channels;
};

struct ad7779_state {
	struct spi_device *spi;
	const struct ad7779_chip_info *chip_info;
	struct clk *mclk;
	struct iio_trigger *trig;
	struct completion completion;
	unsigned int sampling_freq;
	enum ad7779_power_mode power_mode;
	enum ad7779_filter filter_enabled;
	/*
	 * DMA (thus cache coherency maintenance) requires the
	 * transfer buffers to live in their own cache lines.
	 */
	u8			reg_rx_buf[3] __aligned(IIO_DMA_MINALIGN);
	u8			reg_tx_buf[3];
	u32			spidata_rx[8];
	u32			spidata_tx[8];
	u8			reset_buf[8];
};

static const char * const ad7779_filter_type[] = {
	[AD7779_SINC3] = "sinc3",
	[AD7779_SINC5] = "sinc5",
};

static int ad7779_spi_read(struct ad7779_state *st, u8 reg, u8 *rbuf)
{
	int ret;
	int length = 3;
	u8 crc_buf[2];
	u8 exp_crc = 0;
	struct spi_transfer reg_read_tr[] = {
		{
			.tx_buf = st->reg_tx_buf,
			.rx_buf = st->reg_rx_buf,
		},
	};

	if (reg == AD7779_REG_GEN_ERR_REG_1_EN)
		length = 2;
	reg_read_tr[0].len = length;

	st->reg_tx_buf[0] = AD7779_SPI_READ_CMD | FIELD_GET(AD7779_REG_MSK, reg);
	st->reg_tx_buf[1] = 0;
	st->reg_tx_buf[2] = crc8(ad7779_crc8_table, st->reg_tx_buf, 2, 0);

	ret = spi_sync_transfer(st->spi, reg_read_tr, ARRAY_SIZE(reg_read_tr));
	if (ret)
		return ret;

	crc_buf[0] = AD7779_SPI_READ_CMD | FIELD_GET(AD7779_REG_MSK, reg);
	crc_buf[1] = st->reg_rx_buf[1];
	exp_crc = crc8(ad7779_crc8_table, crc_buf, 2, 0);
	if (reg != AD7779_REG_GEN_ERR_REG_1_EN && exp_crc != st->reg_rx_buf[2]) {
		dev_err(&st->spi->dev, "Bad CRC %x, expected %x",
			st->reg_rx_buf[2], exp_crc);
		return -EINVAL;
	}
	*rbuf = st->reg_rx_buf[1];

	return 0;
}

static int ad7779_spi_write(struct ad7779_state *st, u8 reg, u8 val)
{
	int length = 3;
	struct spi_transfer reg_write_tr[] = {
		{
			.tx_buf = st->reg_tx_buf,
		},
	};

	if (reg == AD7779_REG_GEN_ERR_REG_1_EN)
		length = 2;
	reg_write_tr[0].len = length;

	st->reg_tx_buf[0] = FIELD_GET(AD7779_REG_MSK, reg);
	st->reg_tx_buf[1] = val;
	st->reg_tx_buf[2] = crc8(ad7779_crc8_table, st->reg_tx_buf, 2, 0);

	return spi_sync_transfer(st->spi, reg_write_tr, ARRAY_SIZE(reg_write_tr));
}

static int ad7779_spi_write_mask(struct ad7779_state *st, u8 reg, u8 mask,
				 u8 val)
{
	int ret;
	u8 regval, data;

	ret = ad7779_spi_read(st, reg, &data);
	if (ret)
		return ret;

	regval = data;
	regval &= ~mask;
	regval |= val;

	if (regval == data)
		return 0;

	return ad7779_spi_write(st, reg, regval);
}

static int ad7779_reg_access(struct iio_dev *indio_dev,
			     unsigned int reg,
			     unsigned int writeval,
			     unsigned int *readval)
{
	struct ad7779_state *st = iio_priv(indio_dev);

	if (readval)
		return ad7779_spi_read(st, reg, (u8 *)readval);

	return ad7779_spi_write(st, reg, writeval);
}

static int ad7779_set_sampling_frequency(struct ad7779_state *st,
					 unsigned int sampling_freq)
{
	int ret;
	unsigned int dec;
	unsigned int div;
	unsigned int decimal;
	int temp;
	unsigned int kfreq;

	if (st->filter_enabled == AD7779_SINC3 &&
	    sampling_freq > AD7779_SINC3_MAXFREQ)
		return -EINVAL;

	if (st->filter_enabled == AD7779_SINC5 &&
	    sampling_freq > AD7779_SINC5_MAXFREQ)
		return -EINVAL;

	if (sampling_freq > AD7779_SPIMODE_MAX_SAMP_FREQ)
		return -EINVAL;

	if (st->power_mode == AD7779_LOW_POWER)
		div = AD7779_LOWPOWER_DIV;
	else
		div = AD7779_HIGHPOWER_DIV;

	kfreq = sampling_freq / KILO;
	dec = div / kfreq;

	ret = ad7779_spi_write(st, AD7779_REG_SRC_N_MSB,
						FIELD_GET(AD7779_FREQ_MSB_MSK, dec));
	if (ret)
		return ret;
	ret = ad7779_spi_write(st, AD7779_REG_SRC_N_LSB,
						FIELD_GET(AD7779_FREQ_LSB_MSK, dec));
	if (ret)
		return ret;

	if (div % kfreq) {
		temp = (div * KILO) / kfreq;
		decimal = ((temp -  dec * KILO) << 16) / KILO;
		ret = ad7779_spi_write(st, AD7779_REG_SRC_N_MSB,
						FIELD_GET(AD7779_FREQ_MSB_MSK, decimal));
		if (ret)
			return ret;
		ret = ad7779_spi_write(st, AD7779_REG_SRC_N_LSB,
						FIELD_GET(AD7779_FREQ_LSB_MSK, decimal));
		if (ret)
			return ret;
	} else {
		ret = ad7779_spi_write(st, AD7779_REG_SRC_N_MSB,
						   FIELD_GET(AD7779_FREQ_MSB_MSK, 0x0));
		if (ret)
			return ret;
		ret = ad7779_spi_write(st, AD7779_REG_SRC_N_LSB,
							FIELD_GET(AD7779_FREQ_LSB_MSK, 0x0));
		if (ret)
			return ret;
	}
	ret = ad7779_spi_write(st, AD7779_REG_SRC_UPDATE, 0x1);
	if (ret)
		return ret;

	/* SRC update settling time */
	fsleep(15);
	ret = ad7779_spi_write(st, AD7779_REG_SRC_UPDATE, 0x0);
	if (ret)
		return ret;

	/* SRC update settling time */
	fsleep(15);

	st->sampling_freq = sampling_freq;

	return 0;
}

static int ad7779_get_filter(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan)
{
	struct ad7779_state *st = iio_priv(indio_dev);
	u8 temp;
	int ret;

	ret = ad7779_spi_read(st, AD7779_REG_GENERAL_USER_CONFIG_2, &temp);
	if (ret)
		return ret;

	return FIELD_GET(AD7779_FILTER_MSK, temp);
}

static int ad7779_set_filter(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     unsigned int mode)
{
	struct ad7779_state *st = iio_priv(indio_dev);
	int ret;

	ret = ad7779_spi_write_mask(st,
				    AD7779_REG_GENERAL_USER_CONFIG_2,
				    AD7779_FILTER_MSK,
				    FIELD_PREP(AD7779_FILTER_MSK, mode));
	if (ret < 0)
		return ret;

	ret = ad7779_set_sampling_frequency(st, st->sampling_freq);
	if (ret < 0)
		return ret;

	st->filter_enabled = mode;

	return 0;
}

static int ad7779_get_calibscale(struct ad7779_state *st, int channel)
{
	int ret;
	u8 calibscale[3];
	// u8 low, mid, high;
	ret = ad7779_spi_read(st, AD7779_REG_CH_GAIN_LOWER_BYTE(channel), &calibscale[0]);
	if (ret)
		return ret;

	ret = ad7779_spi_read(st, AD7779_REG_CH_GAIN_MID_BYTE(channel), &calibscale[1]);
	if (ret)
		return ret;

	ret = ad7779_spi_read(st, AD7779_REG_CH_GAIN_UPPER_BYTE(channel), &calibscale[2]);
	if (ret)
		return ret;

	// return FIELD_PREP(AD7779_UPPER, high) | FIELD_PREP(AD7779_MID, mid) |
	//        FIELD_PREP(AD7779_LOWER, low);
	return get_unaligned_be24(calibscale);
}

static int ad7779_set_calibscale(struct ad7779_state *st, int channel, int val)
{
	int ret;
	unsigned int gain;
	unsigned long long tmp;
	u8 gain_bytes[3];

	tmp = val * 5592405LL;
	gain = DIV_ROUND_CLOSEST_ULL(tmp, MEGA);
	put_unaligned_be24(gain, gain_bytes);
	ret = ad7779_spi_write(st,
			       AD7779_REG_CH_GAIN_UPPER_BYTE(channel),
			       gain_bytes[0]);
	if (ret)
		return ret;

	ret = ad7779_spi_write(st,
			       AD7779_REG_CH_GAIN_MID_BYTE(channel),
			       gain_bytes[1]);
	if (ret)
		return ret;

	return ad7779_spi_write(st,
				AD7779_REG_CH_GAIN_LOWER_BYTE(channel),
				gain_bytes[2]);
}

static int ad7779_get_calibbias(struct ad7779_state *st, int channel)
{
	int ret;
	u8 calibbias[3];
	u8 low, mid, high;

	ret = ad7779_spi_read(st, AD7779_REG_CH_OFFSET_LOWER_BYTE(channel),
			      &calibbias[0]);
	if (ret)
		return ret;

	ret = ad7779_spi_read(st, AD7779_REG_CH_OFFSET_MID_BYTE(channel),
				  &calibbias[1]);
	if (ret)
		return ret;

	ret = ad7779_spi_read(st,
			      AD7779_REG_CH_OFFSET_UPPER_BYTE(channel),
			      &calibbias[2]);
	if (ret)
		return ret;

	return get_unaligned_be24(calibbias);
}

static int ad7779_set_calibbias(struct ad7779_state *st, int channel, int val)
{
	int ret;
	u8 calibbias[3];
	u8 msb, mid, lsb;

	msb = FIELD_GET(AD7779_UPPER, val);
	mid = FIELD_GET(AD7779_MID, val);
	lsb = FIELD_GET(AD7779_LOWER, val);
	put_unaligned_be24(val, calibbias);
	ret = ad7779_spi_write(st,
			       AD7779_REG_CH_OFFSET_UPPER_BYTE(channel),
			       calibbias[0]);
	if (ret)
		return ret;

	ret = ad7779_spi_write(st,
			       AD7779_REG_CH_OFFSET_MID_BYTE(channel),
			       calibbias[1]);
	if (ret)
		return ret;

	return ad7779_spi_write(st,
				AD7779_REG_CH_OFFSET_LOWER_BYTE(channel),
				calibbias[2]);
}

static int ad7779_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val,
			   int *val2,
			   long mask)
{
	struct ad7779_state *st = iio_priv(indio_dev);
	int ret;

	iio_device_claim_direct_scoped(return -EBUSY, indio_dev) {
		switch (mask) {
		case IIO_CHAN_INFO_CALIBSCALE:
			*val = ad7779_get_calibscale(st, chan->channel);
			iio_device_release_direct_mode(indio_dev);
			if (*val < 0)
				return -EINVAL;
			*val2 = GAIN_REL;
			return IIO_VAL_FRACTIONAL;
		case IIO_CHAN_INFO_CALIBBIAS:
			*val = ad7779_get_calibbias(st, chan->channel);
			iio_device_release_direct_mode(indio_dev);
			if (*val < 0)
				return -EINVAL;
			return IIO_VAL_INT;
		case IIO_CHAN_INFO_SAMP_FREQ:
			*val = st->sampling_freq;
			iio_device_release_direct_mode(indio_dev);
			if (*val < 0)
				return -EINVAL;
			return IIO_VAL_INT;
		}
		return -EINVAL;
	}
	unreachable();
}

static int ad7779_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val,
			    int val2,
			    long mask)
{
	struct ad7779_state *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_CALIBSCALE:
		return ad7779_set_calibscale(st, chan->channel, val2);
	case IIO_CHAN_INFO_CALIBBIAS:
		return ad7779_set_calibbias(st, chan->channel, val);
	case IIO_CHAN_INFO_SAMP_FREQ:
		return ad7779_set_sampling_frequency(st, val);
	default:
		return -EINVAL;
	}
}

static int ad7779_buffer_preenable(struct iio_dev *indio_dev)
{
	int ret;
	struct ad7779_state *st = iio_priv(indio_dev);

	ret = ad7779_spi_write_mask(st,
				    AD7779_REG_GENERAL_USER_CONFIG_3,
				    AD7779_MOD_SPI_EN_MSK,
				    FIELD_PREP(AD7779_MOD_SPI_EN_MSK, 1));
	if (ret)
		return ret;
 
	/* DRDY output cannot be disabled at device level
	 * therefore we mask the irq at host end.
	 */
	enable_irq(st->spi->irq);

	return 0;
}

static int ad7779_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct ad7779_state *st = iio_priv(indio_dev);

	disable_irq(st->spi->irq);

	return ad7779_spi_write(st, AD7779_REG_GENERAL_USER_CONFIG_3,
			       AD7779_DISABLE_SD);
}

static irqreturn_t ad7779_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ad7779_state *st = iio_priv(indio_dev);
	int ret;
	int bit;
	int k = 0;
	/* Each channel shifts out HEADER + 24 bits of data
	 * therefore 8 * u32 for the data and 64 bits for
	 * the timestamp
	 */
	u32 tmp[10]; 

	struct spi_transfer sd_readback_tr[] = {
		{
			.rx_buf = st->spidata_rx,
			.tx_buf = st->spidata_tx,
			.len = 32,
		}
	};

	if (!iio_buffer_enabled(indio_dev)){
		iio_trigger_notify_done(indio_dev->trig);
		return IRQ_HANDLED;
	}

	st->spidata_tx[0] = AD7779_SPI_READ_CMD;
	ret = spi_sync_transfer(st->spi, sd_readback_tr,
				ARRAY_SIZE(sd_readback_tr));
	if (ret) {
		dev_err(&st->spi->dev,
			"spi transfer error in irq handler");
		iio_trigger_notify_done(indio_dev->trig);
		return IRQ_HANDLED;
	}

	for_each_set_bit(bit, indio_dev->active_scan_mask, AD7779_NUM_CHANNELS - 1)
		tmp[k++] = st->spidata_rx[bit];

	iio_push_to_buffers_with_timestamp(indio_dev, &tmp[0], pf->timestamp);

	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int ad7779_reset(struct iio_dev *indio_dev, struct gpio_desc *reset_gpio)
{
	struct ad7779_state *st = iio_priv(indio_dev);
	int ret;
	struct spi_transfer reg_read_tr[] = {
		{
			.tx_buf = st->reset_buf,
			.len = 8,
		},
	};

	memset(st->reset_buf, 0xff, sizeof(st->reset_buf));

	if (reset_gpio) {
		/* Delay for reset to occur is 225 microseconds*/
		gpiod_set_value(reset_gpio, 1);
		fsleep(230);
		return 0;
	}

	ret = spi_sync_transfer(st->spi, reg_read_tr,
				ARRAY_SIZE(reg_read_tr));
	if (ret)
		return ret;

	/* Delay for reset to occur is 225 microseconds*/
	fsleep(230);

	return 0;
}

static const struct iio_info ad7779_info = {
	.read_raw = ad7779_read_raw,
	.write_raw = ad7779_write_raw,
	.debugfs_reg_access = &ad7779_reg_access,
};

static const struct iio_enum ad7779_filter_enum = {
	.items = ad7779_filter_type,
	.num_items = ARRAY_SIZE(ad7779_filter_type),
	.get = ad7779_get_filter,
	.set = ad7779_set_filter,
};

static const struct iio_chan_spec_ext_info ad7779_ext_filter[] = {
	IIO_ENUM("filter_type", IIO_SHARED_BY_ALL, &ad7779_filter_enum),
	IIO_ENUM_AVAILABLE("filter_type", IIO_SHARED_BY_ALL,
				  &ad7779_filter_enum),
	{ }
};

#define AD777x_CHAN_S(index, _ext_info)					       \
	{								       \
		.type = IIO_VOLTAGE,					       \
		.info_mask_separate = BIT(IIO_CHAN_INFO_CALIBSCALE)  |	       \
				      BIT(IIO_CHAN_INFO_CALIBBIAS),	       \
		.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ),       \
		.address = (index),					       \
		.indexed = 1,						       \
		.channel = (index),					       \
		.scan_index = (index),					       \
		.ext_info = (_ext_info),				       \
		.scan_type = {						       \
			.sign = 's',					       \
			.realbits = 24,					       \
			.storagebits = 32,				       \
			.endianness = IIO_BE,				   \
		},							       		   \
	}

#define AD777x_CHAN_NO_FILTER_S(index)					       \
	AD777x_CHAN_S(index, NULL)

#define AD777x_CHAN_FILTER_S(index)					       \
	AD777x_CHAN_S(index, ad7779_ext_filter)

static const struct iio_chan_spec ad7779_channels[] = {
	AD777x_CHAN_NO_FILTER_S(0),
	AD777x_CHAN_NO_FILTER_S(1),
	AD777x_CHAN_NO_FILTER_S(2),
	AD777x_CHAN_NO_FILTER_S(3),
	AD777x_CHAN_NO_FILTER_S(4),
	AD777x_CHAN_NO_FILTER_S(5),
	AD777x_CHAN_NO_FILTER_S(6),
	AD777x_CHAN_NO_FILTER_S(7),
	IIO_CHAN_SOFT_TIMESTAMP(8),
};

static const struct iio_chan_spec ad7779_channels_filter[] = {
	AD777x_CHAN_FILTER_S(0),
	AD777x_CHAN_FILTER_S(1),
	AD777x_CHAN_FILTER_S(2),
	AD777x_CHAN_FILTER_S(3),
	AD777x_CHAN_FILTER_S(4),
	AD777x_CHAN_FILTER_S(5),
	AD777x_CHAN_FILTER_S(6),
	AD777x_CHAN_FILTER_S(7),
	IIO_CHAN_SOFT_TIMESTAMP(8),
};

static const struct iio_buffer_setup_ops ad7779_buffer_setup_ops = {
	.preenable = ad7779_buffer_preenable,
	.postdisable = ad7779_buffer_postdisable,
};

static const struct iio_trigger_ops ad7779_trigger_ops = {
	.validate_device = iio_trigger_validate_own_device,
};

static int ad7779_powerup(struct ad7779_state *st, struct gpio_desc *start_gpio)
{
	int ret;
	
	ret = ad7779_spi_write_mask(st, AD7779_REG_GEN_ERR_REG_1_EN,
				    AD7779_SPI_CRC_EN_MSK,
				    FIELD_PREP(AD7779_SPI_CRC_EN_MSK, 1));
	if (ret)
		return ret;

	ret = ad7779_spi_write_mask(st, AD7779_REG_GENERAL_USER_CONFIG_1,
				    AD7779_MOD_POWERMODE_MSK,
				    FIELD_PREP(AD7779_MOD_POWERMODE_MSK, 1));
	if (ret)
		return ret;

	ret = ad7779_spi_write_mask(st, AD7779_REG_GENERAL_USER_CONFIG_1,
				    AD7779_MOD_PDB_REFOUT_MSK,
				    FIELD_PREP(AD7779_MOD_PDB_REFOUT_MSK, 1));
	if (ret)
		return ret;

	ret = ad7779_spi_write_mask(st, AD7779_REG_DOUT_FORMAT,
				    AD7779_DCLK_CLK_DIV_MSK,
				    FIELD_PREP(AD7779_DCLK_CLK_DIV_MSK, 1));
	if (ret)
		return ret;

	ret = ad7779_spi_write_mask(st, AD7779_REG_ADC_MUX_CONFIG,
				    AD7779_REFMUX_CTRL_MSK,
				    FIELD_PREP(AD7779_REFMUX_CTRL_MSK, 1));
	if (ret)
		return ret;


	st->power_mode = AD7779_HIGH_POWER;
	ret = ad7779_set_sampling_frequency(st, AD7779_DEFAULT_SAMPLING_FREQ);
	if (ret)
		return ret;

	gpiod_set_value(start_gpio, 0);
	/* Start setup time */
	fsleep(15);
	gpiod_set_value(start_gpio, 1);
	fsleep(15);
	gpiod_set_value(start_gpio, 0);
	fsleep(15);

	return 0;
}

static int ad7779_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct ad7779_state *st;
	struct gpio_desc *reset_gpio, *start_gpio;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);

	st->mclk = devm_clk_get_enabled(&spi->dev, "mclk");
	if (IS_ERR(st->mclk))
		return PTR_ERR(st->mclk);

	if (!spi->irq)
		return dev_err_probe(&spi->dev, ret,
				     "DRDY irq not present\n"); 

	reset_gpio = devm_gpiod_get_optional(&spi->dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset_gpio))
		return PTR_ERR(reset_gpio);

	start_gpio = devm_gpiod_get(&spi->dev, "start", GPIOD_OUT_HIGH);
	if (IS_ERR(start_gpio))
		return PTR_ERR(start_gpio);

	crc8_populate_msb(ad7779_crc8_table, AD7779_CRC8_POLY);
	st->spi = spi;

	st->chip_info = spi_get_device_match_data(spi);
	if (!st->chip_info)
		return -ENODEV;

	ret = ad7779_reset(indio_dev, start_gpio);
	if (ret)
		return ret;

	ad7779_powerup(st, start_gpio);
	if (ret)
		return ret;

	indio_dev->name = st->chip_info->name;
	indio_dev->info = &ad7779_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = st->chip_info->channels;
	indio_dev->num_channels = ARRAY_SIZE(ad7779_channels);

	st->trig = devm_iio_trigger_alloc(&spi->dev, "%s-dev%d",
					  		indio_dev->name, iio_device_id(indio_dev));
	if (!st->trig)
		return -ENOMEM;

	st->trig->ops= &ad7779_trigger_ops;
	st->trig->dev.parent = &spi->dev;

	iio_trigger_set_drvdata(st->trig, st);

	ret = devm_request_irq(&spi->dev, spi->irq,
						   iio_trigger_generic_data_rdy_poll,
						   IRQF_ONESHOT | IRQF_NO_AUTOEN,
						   indio_dev->name, st->trig);
	if (ret)
		return dev_err_probe(&spi->dev, ret, "request irq %d failed\n",
				     st->spi->irq);

	ret = devm_iio_trigger_register(&spi->dev, st->trig);
	if (ret)
		return ret;

	indio_dev->trig = iio_trigger_get(st->trig);

	init_completion(&st->completion);

	ret = devm_iio_triggered_buffer_setup(&spi->dev, indio_dev,
										  &iio_pollfunc_store_time,
										  &ad7779_trigger_handler,
										  &ad7779_buffer_setup_ops);
	if (ret)
		return ret;

	ret = ad7779_spi_write_mask(st, AD7779_REG_DOUT_FORMAT,
				    AD7779_DCLK_CLK_DIV_MSK,
				    FIELD_PREP(AD7779_DCLK_CLK_DIV_MSK, 7));
	if (ret)
		return ret;

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static int __maybe_unused ad7779_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ad7779_state *st = iio_priv(indio_dev);
	int ret;

	ret = ad7779_spi_write_mask(st, AD7779_REG_GENERAL_USER_CONFIG_1,
				    AD7779_MOD_POWERMODE_MSK,
				    FIELD_PREP(AD7779_MOD_POWERMODE_MSK,
					       AD7779_LOW_POWER));
	if (ret)
		return ret;

	st->power_mode = AD7779_LOW_POWER;
	return 0;
}

static int __maybe_unused ad7779_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ad7779_state *st = iio_priv(indio_dev);
	int ret;

	ret = ad7779_spi_write_mask(st, AD7779_REG_GENERAL_USER_CONFIG_1,
				    AD7779_MOD_POWERMODE_MSK,
				    FIELD_PREP(AD7779_MOD_POWERMODE_MSK,
					       AD7779_HIGH_POWER));
	if (ret)
		return ret;

	st->power_mode = AD7779_HIGH_POWER;

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ad7779_pm_ops, ad7779_suspend, ad7779_resume);

static const struct ad7779_chip_info ad7770_chip_info = {
	.name = "ad7770",
	.channels = ad7779_channels,
};

static const struct ad7779_chip_info ad7771_chip_info = {
	.name = "ad7771",
	.channels = ad7779_channels_filter,
};

static const struct ad7779_chip_info AD7779_chip_info = {
	.name = "AD7779",
	.channels = ad7779_channels,
};

static const struct spi_device_id ad7779_id[] = {
	{
		.name = "ad7770",
		.driver_data = (kernel_ulong_t)&ad7770_chip_info
	},
	{
		.name = "ad7771",
		.driver_data = (kernel_ulong_t)&ad7771_chip_info
	},
	{
		.name = "AD7779",
		.driver_data = (kernel_ulong_t)&AD7779_chip_info
	},
	{ }
};
MODULE_DEVICE_TABLE(spi, ad7779_id);

static const struct of_device_id ad7779_of_table[] = {
	{
		.compatible = "adi,ad7770",
		.data = &ad7770_chip_info,
	},
	{
		.compatible = "adi,ad7771",
		.data = &ad7771_chip_info,
	},
	{
		.compatible = "adi,AD7779",
		.data = &AD7779_chip_info,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, ad7779_of_table);

static struct spi_driver ad7779_driver = {
	.driver = {
		.name = "ad7779",
		.pm = pm_sleep_ptr(&ad7779_pm_ops),
		.of_match_table = ad7779_of_table,
	},
	.probe = ad7779_probe,
	.id_table = ad7779_id,
};
module_spi_driver(ad7779_driver);

MODULE_AUTHOR("Ramona Alexandra Nechita <ramona.nechita@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD7779 ADC");
MODULE_LICENSE("GPL");
