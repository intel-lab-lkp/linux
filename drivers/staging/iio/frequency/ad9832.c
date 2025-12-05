// SPDX-License-Identifier: GPL-2.0
/*
 * AD9832 SPI DDS driver
 *
 * Copyright 2011 Analog Devices Inc.
 */

#include <asm/div64.h>

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/cleanup.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/sysfs.h>
#include <linux/unaligned.h>

#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>

/* Registers */
#define AD9832_FREQ0LL		0x0
#define AD9832_FREQ0HL		0x1
#define AD9832_FREQ0LM		0x2
#define AD9832_FREQ0HM		0x3
#define AD9832_FREQ1LL		0x4
#define AD9832_FREQ1HL		0x5
#define AD9832_FREQ1LM		0x6
#define AD9832_FREQ1HM		0x7
#define AD9832_PHASE0L		0x8
#define AD9832_PHASE0H		0x9
#define AD9832_PHASE1L		0xA
#define AD9832_PHASE1H		0xB
#define AD9832_PHASE2L		0xC
#define AD9832_PHASE2H		0xD
#define AD9832_PHASE3L		0xE
#define AD9832_PHASE3H		0xF

#define AD9832_PHASE_SYM	0x10
#define AD9832_FREQ_SYM		0x11
#define AD9832_PINCTRL_EN	0x12
#define AD9832_OUTPUT_EN	0x13

/* Command Control Bits */
#define AD9832_CMD_PHA8BITSW	0x1
#define AD9832_CMD_PHA16BITSW	0x0
#define AD9832_CMD_FRE8BITSW	0x3
#define AD9832_CMD_FRE16BITSW	0x2
#define AD9832_CMD_FPSELECT	0x6
#define AD9832_CMD_SYNCSELSRC	0x8
#define AD9832_CMD_SLEEPRESCLR	0xC

#define AD9832_FREQ		BIT(11)
#define AD9832_PHASE_MASK	GENMASK(10, 9)
#define AD9832_SYNC		BIT(13)
#define AD9832_SELSRC		BIT(12)
#define AD9832_SLEEP		BIT(13)
#define AD9832_RESET		BIT(12)
#define AD9832_CLR		BIT(11)
#define AD9832_FREQ_BITS	32
#define AD9832_PHASE_BITS	12
#define AD9832_CMD_MSK		GENMASK(15, 12)
#define AD9832_ADD_MSK		GENMASK(11, 8)
#define AD9832_DAT_MSK		GENMASK(7, 0)

/**
 * struct ad9832_state - driver instance specific data
 * @spi:		spi_device
 * @mclk:		external master clock
 * @ctrl_fp:		cached frequency/phase control word
 * @ctrl_ss:		cached sync/selsrc control word
 * @ctrl_src:		cached sleep/reset/clr word
 * @freq:		cached frequencies
 * @phase:		cached phases
 * @xfer:		default spi transfer
 * @msg:		default spi message
 * @freq_xfer:		tuning word spi transfer
 * @freq_msg:		tuning word spi message
 * @phase_xfer:		tuning word spi transfer
 * @phase_msg:		tuning word spi message
 * @lock:		protect sensor state
 * @data:		spi transmit buffer
 * @phase_data:		tuning word spi transmit buffer
 * @freq_data:		tuning word spi transmit buffer
 */
struct ad9832_state {
	struct spi_device		*spi;
	struct clk			*mclk;
	unsigned short			ctrl_fp;
	unsigned short			ctrl_ss;
	unsigned short			ctrl_src;
	u32				freq[2];
	u32				phase[4];
	struct spi_transfer		xfer;
	struct spi_message		msg;
	struct spi_transfer		freq_xfer[4];
	struct spi_message		freq_msg;
	struct spi_transfer		phase_xfer[2];
	struct spi_message		phase_msg;
	struct mutex			lock;	/* protect sensor state */
	/*
	 * DMA (thus cache coherency maintenance) requires the
	 * transfer buffers to live in their own cache lines.
	 */
	union {
		__be16			freq_data[4];
		__be16			phase_data[2];
		__be16			data;
	} __aligned(IIO_DMA_MINALIGN);
};

static unsigned long ad9832_calc_freqreg(unsigned long mclk, u32 fout)
{
	unsigned long long freqreg = (u64)fout *
				     (u64)((u64)1L << AD9832_FREQ_BITS);
	do_div(freqreg, mclk);
	return freqreg;
}

static int ad9832_write_frequency(struct ad9832_state *st,
				  unsigned int addr, u32 fout)
{
	unsigned long clk_freq;
	unsigned long regval;
	u8 regval_bytes[4];
	u16 freq_cmd;
	int ret, idx;

	switch (addr) {
	case AD9832_FREQ0HM:
		idx = 0;
		break;
	case AD9832_FREQ1HM:
		idx = 1;
		break;
	default:
		return -EINVAL;
	}

	clk_freq = clk_get_rate(st->mclk);

	if (!clk_freq || fout > (clk_freq / 2))
		return -EINVAL;

	regval = ad9832_calc_freqreg(clk_freq, fout);
	put_unaligned_be32(regval, regval_bytes);

	for (int i = 0; i < ARRAY_SIZE(regval_bytes); i++) {
		freq_cmd = (i % 2 == 0) ? AD9832_CMD_FRE8BITSW : AD9832_CMD_FRE16BITSW;

		st->freq_data[i] = cpu_to_be16(FIELD_PREP(AD9832_CMD_MSK, freq_cmd) |
			FIELD_PREP(AD9832_ADD_MSK, addr - i) |
			FIELD_PREP(AD9832_DAT_MSK, regval_bytes[i]));
	}

	ret = spi_sync(st->spi, &st->freq_msg);
	if (ret)
		return ret;

	st->freq[idx] = fout;
	return 0;
}

static int ad9832_write_phase(struct ad9832_state *st,
			      unsigned long addr, u32 phase)
{
	u8 phase_bytes[2];
	u16 phase_cmd;
	int ret, idx;

	switch (addr) {
	case AD9832_PHASE0H:
		idx = 0;
		break;
	case AD9832_PHASE1H:
		idx = 1;
		break;
	case AD9832_PHASE2H:
		idx = 2;
		break;
	case AD9832_PHASE3H:
		idx = 3;
		break;
	default:
		return -EINVAL;
	}

	if (phase >= BIT(AD9832_PHASE_BITS))
		return -EINVAL;

	put_unaligned_be16(phase, phase_bytes);

	for (int i = 0; i < ARRAY_SIZE(phase_bytes); i++) {
		phase_cmd = (i % 2 == 0) ? AD9832_CMD_PHA8BITSW : AD9832_CMD_PHA16BITSW;

		st->phase_data[i] = cpu_to_be16(FIELD_PREP(AD9832_CMD_MSK, phase_cmd) |
			FIELD_PREP(AD9832_ADD_MSK, addr - i) |
			FIELD_PREP(AD9832_DAT_MSK, phase_bytes[i]));
	}

	ret = spi_sync(st->spi, &st->phase_msg);
	if (ret)
		return ret;

	st->phase[idx] = phase;
	return 0;
}

static int ad9832_write_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int val, int val2, long mask)
{
	struct ad9832_state *st = iio_priv(indio_dev);

	if (val < 0)
		return -EINVAL;

	guard(mutex)(&st->lock);
	switch (mask) {
	case IIO_CHAN_INFO_FREQUENCY:
		return ad9832_write_frequency(st, chan->address, val);
	case IIO_CHAN_INFO_PHASE:
		return ad9832_write_phase(st, chan->address, val);
	default:
		return -EINVAL;
	}
}

static int ad9832_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct ad9832_state *st = iio_priv(indio_dev);

	guard(mutex)(&st->lock);
	switch (mask) {
	case IIO_CHAN_INFO_FREQUENCY:
		switch (chan->address) {
		case AD9832_FREQ0HM:
			*val = st->freq[0];
			return IIO_VAL_INT;
		case AD9832_FREQ1HM:
			*val = st->freq[1];
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_PHASE:
		switch (chan->address) {
		case AD9832_PHASE0H:
			*val = st->phase[0];
			return IIO_VAL_INT;
		case AD9832_PHASE1H:
			*val = st->phase[1];
			return IIO_VAL_INT;
		case AD9832_PHASE2H:
			*val = st->phase[2];
			return IIO_VAL_INT;
		case AD9832_PHASE3H:
			*val = st->phase[3];
			return IIO_VAL_INT;
		default:
			return -EINVAL;
		}
	default:
		return -EINVAL;
	}
}

static ssize_t ad9832_store(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct ad9832_state *st = iio_priv(indio_dev);
	struct iio_dev_attr *this_attr = to_iio_dev_attr(attr);
	int ret;
	unsigned long val;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	guard(mutex)(&st->lock);
	switch (this_attr->address) {
	case AD9832_PINCTRL_EN:
		st->ctrl_ss &= ~AD9832_SELSRC;
		st->ctrl_ss |= FIELD_PREP(AD9832_SELSRC, val ? 0 : 1);

		st->data = cpu_to_be16(FIELD_PREP(AD9832_CMD_MSK, AD9832_CMD_SYNCSELSRC) |
						  st->ctrl_ss);
		ret = spi_sync(st->spi, &st->msg);
		break;
	case AD9832_FREQ_SYM:
		if (val != 1 && val != 0) {
			ret = -EINVAL;
			break;
		}

		st->ctrl_fp &= ~AD9832_FREQ;
		st->ctrl_fp |= FIELD_PREP(AD9832_FREQ, val);
		st->data = cpu_to_be16(FIELD_PREP(AD9832_CMD_MSK, AD9832_CMD_FPSELECT) |
						  st->ctrl_fp);
		ret = spi_sync(st->spi, &st->msg);
		break;
	case AD9832_PHASE_SYM:
		if (val > 3) {
			ret = -EINVAL;
			break;
		}

		st->ctrl_fp &= ~AD9832_PHASE_MASK;
		st->ctrl_fp |= FIELD_PREP(AD9832_PHASE_MASK, val);

		st->data = cpu_to_be16(FIELD_PREP(AD9832_CMD_MSK, AD9832_CMD_FPSELECT) |
						  st->ctrl_fp);
		ret = spi_sync(st->spi, &st->msg);
		break;
	case AD9832_OUTPUT_EN:
		if (val)
			st->ctrl_src &= ~(AD9832_RESET | AD9832_SLEEP | AD9832_CLR);
		else
			st->ctrl_src |= FIELD_PREP(AD9832_RESET, 1);

		st->data = cpu_to_be16(FIELD_PREP(AD9832_CMD_MSK, AD9832_CMD_SLEEPRESCLR) |
						  st->ctrl_src);
		ret = spi_sync(st->spi, &st->msg);
		break;
	default:
		ret = -ENODEV;
	}

	return ret ? ret : len;
}

#define AD9832_CHAN_FREQ(_channel, _addr) {			\
	.type = IIO_ALTVOLTAGE,					\
	.output = 1,						\
	.indexed = 1,						\
	.channel = _channel,					\
	.address = _addr,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_FREQUENCY),	\
}

#define AD9832_CHAN_PHASE(_channel, _addr) {			\
	.type = IIO_ALTVOLTAGE,					\
	.output = 1,						\
	.indexed = 1,						\
	.channel = _channel,					\
	.address = _addr,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_PHASE),		\
}

static const struct iio_chan_spec ad9832_channels[] = {
	AD9832_CHAN_FREQ(0, AD9832_FREQ0HM),
	AD9832_CHAN_FREQ(1, AD9832_FREQ1HM),
	AD9832_CHAN_PHASE(2, AD9832_PHASE0H),
	AD9832_CHAN_PHASE(3, AD9832_PHASE1H),
	AD9832_CHAN_PHASE(4, AD9832_PHASE2H),
	AD9832_CHAN_PHASE(5, AD9832_PHASE3H),
};

static IIO_CONST_ATTR(out_altvoltage_frequency_scale, "1");	   /* 1Hz */
static IIO_CONST_ATTR(out_altvoltage_phase_scale, "0.0015339808"); /* 2PI / 2^12 rad */

static IIO_DEVICE_ATTR(out_altvoltage_frequencysymbol, 0200, NULL,
		       ad9832_store, AD9832_FREQ_SYM);
static IIO_DEVICE_ATTR(out_altvoltage_phasesymbol, 0200, NULL,
		       ad9832_store, AD9832_PHASE_SYM);
static IIO_DEVICE_ATTR(out_altvoltage_out_enable, 0200, NULL,
		       ad9832_store, AD9832_OUTPUT_EN);
static IIO_DEVICE_ATTR(out_altvoltage_pincontrol_en, 0200, NULL,
		       ad9832_store, AD9832_PINCTRL_EN);

static struct attribute *ad9832_attributes[] = {
	&iio_const_attr_out_altvoltage_frequency_scale.dev_attr.attr,
	&iio_const_attr_out_altvoltage_phase_scale.dev_attr.attr,
	&iio_dev_attr_out_altvoltage_frequencysymbol.dev_attr.attr,
	&iio_dev_attr_out_altvoltage_phasesymbol.dev_attr.attr,
	&iio_dev_attr_out_altvoltage_out_enable.dev_attr.attr,
	&iio_dev_attr_out_altvoltage_pincontrol_en.dev_attr.attr,
	NULL,
};

static const struct attribute_group ad9832_attribute_group = {
	.attrs = ad9832_attributes,
};

static const struct iio_info ad9832_info = {
	.write_raw = ad9832_write_raw,
	.read_raw = ad9832_read_raw,
	.attrs = &ad9832_attribute_group,
};

static int ad9832_probe(struct spi_device *spi)
{
	struct iio_dev *indio_dev;
	struct ad9832_state *st;
	int ret;

	indio_dev = devm_iio_device_alloc(&spi->dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);

	ret = devm_regulator_get_enable(&spi->dev, "avdd");
	if (ret)
		return dev_err_probe(&spi->dev, ret, "failed to enable AVDD supply\n");

	ret = devm_regulator_get_enable(&spi->dev, "dvdd");
	if (ret)
		return dev_err_probe(&spi->dev, ret, "failed to enable DVDD supply\n");

	st->mclk = devm_clk_get_enabled(&spi->dev, "mclk");
	if (IS_ERR(st->mclk))
		return dev_err_probe(&spi->dev, PTR_ERR(st->mclk), "failed to enable MCLK\n");

	st->spi = spi;
	mutex_init(&st->lock);

	indio_dev->name = spi_get_device_id(spi)->name;
	indio_dev->info = &ad9832_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = ad9832_channels;
	indio_dev->num_channels = ARRAY_SIZE(ad9832_channels);

	/* Setup default messages */
	st->xfer.tx_buf = &st->data;
	st->xfer.len = 2;

	spi_message_init(&st->msg);
	spi_message_add_tail(&st->xfer, &st->msg);

	st->freq_xfer[0].tx_buf = &st->freq_data[0];
	st->freq_xfer[0].len = 2;
	st->freq_xfer[0].cs_change = 1;
	st->freq_xfer[1].tx_buf = &st->freq_data[1];
	st->freq_xfer[1].len = 2;
	st->freq_xfer[1].cs_change = 1;
	st->freq_xfer[2].tx_buf = &st->freq_data[2];
	st->freq_xfer[2].len = 2;
	st->freq_xfer[2].cs_change = 1;
	st->freq_xfer[3].tx_buf = &st->freq_data[3];
	st->freq_xfer[3].len = 2;

	spi_message_init(&st->freq_msg);
	spi_message_add_tail(&st->freq_xfer[0], &st->freq_msg);
	spi_message_add_tail(&st->freq_xfer[1], &st->freq_msg);
	spi_message_add_tail(&st->freq_xfer[2], &st->freq_msg);
	spi_message_add_tail(&st->freq_xfer[3], &st->freq_msg);

	st->phase_xfer[0].tx_buf = &st->phase_data[0];
	st->phase_xfer[0].len = 2;
	st->phase_xfer[0].cs_change = 1;
	st->phase_xfer[1].tx_buf = &st->phase_data[1];
	st->phase_xfer[1].len = 2;

	spi_message_init(&st->phase_msg);
	spi_message_add_tail(&st->phase_xfer[0], &st->phase_msg);
	spi_message_add_tail(&st->phase_xfer[1], &st->phase_msg);

	st->ctrl_src = AD9832_SLEEP | AD9832_RESET | AD9832_CLR;
	st->data = cpu_to_be16(FIELD_PREP(AD9832_CMD_MSK, AD9832_CMD_SLEEPRESCLR) |
					  st->ctrl_src);
	ret = spi_sync(st->spi, &st->msg);
	if (ret) {
		dev_err(&spi->dev, "device init failed\n");
		return ret;
	}

	return devm_iio_device_register(&spi->dev, indio_dev);
}

static const struct of_device_id ad9832_of_match[] = {
	{ .compatible = "adi,ad9832" },
	{ .compatible = "adi,ad9835" },
	{ }
};
MODULE_DEVICE_TABLE(of, ad9832_of_match);

static const struct spi_device_id ad9832_id[] = {
	{"ad9832", 0},
	{"ad9835", 0},
	{ }
};
MODULE_DEVICE_TABLE(spi, ad9832_id);

static struct spi_driver ad9832_driver = {
	.driver = {
		.name	= "ad9832",
		.of_match_table = ad9832_of_match,
	},
	.probe		= ad9832_probe,
	.id_table	= ad9832_id,
};
module_spi_driver(ad9832_driver);

MODULE_AUTHOR("Michael Hennerich <michael.hennerich@analog.com>");
MODULE_DESCRIPTION("Analog Devices AD9832/AD9835 DDS");
MODULE_LICENSE("GPL v2");
