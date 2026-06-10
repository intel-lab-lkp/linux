// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Texas Instruments ADS1220 ADC driver
 *
 * Datasheet: https://www.ti.com/lit/gpn/ads1220
 *
 * Copyright (C) 2026 Nguyen Minh Tien <zizuzacker@gmail.com>
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/log2.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_runtime.h>
#include <linux/spi/spi.h>
#include <linux/units.h>
#include <linux/unaligned.h>

#include <linux/iio/buffer.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/triggered_buffer.h>
#include <linux/iio/trigger_consumer.h>

/* SPI commands (Table 8-7) */
#define ADS1220_CMD_RESET	0x06
#define ADS1220_CMD_START	0x08
#define ADS1220_CMD_POWERDOWN	0x02
#define ADS1220_CMD_RDATA	0x10
#define ADS1220_CMD_RREG	0x20
#define ADS1220_CMD_WREG	0x40
/* RREG/WREG operate on one register (nn = 0 => 1 byte) at address rr */
#define ADS1220_CMD_RREG_REG(reg)	(ADS1220_CMD_RREG | ((reg) << 2))
#define ADS1220_CMD_WREG_REG(reg)	(ADS1220_CMD_WREG | ((reg) << 2))

/* Configuration registers (Table 8-8) */
#define ADS1220_REG_CONFIG0	0x00
#define ADS1220_REG_CONFIG1	0x01
#define ADS1220_REG_CONFIG2	0x02
#define ADS1220_REG_CONFIG3	0x03
#define ADS1220_MAX_REG		ADS1220_REG_CONFIG3

/* CONFIG0 */
#define ADS1220_CFG0_MUX	GENMASK(7, 4)
#define ADS1220_CFG0_GAIN	GENMASK(3, 1)
#define ADS1220_CFG0_PGA_BYPASS	BIT(0)

/* CONFIG1 */
#define ADS1220_CFG1_DR		GENMASK(7, 5)
#define ADS1220_CFG1_MODE	GENMASK(4, 3)
#define ADS1220_CFG1_CM		BIT(2)
#define ADS1220_CFG1_TS		BIT(1)
#define ADS1220_CFG1_BCS	BIT(0)

/* CONFIG2 */
#define ADS1220_CFG2_VREF	GENMASK(7, 6)
#define ADS1220_CFG2_FILTER	GENMASK(5, 4)
#define ADS1220_CFG2_PSW	BIT(3)
#define ADS1220_CFG2_IDAC	GENMASK(2, 0)

/* CONFIG3 */
#define ADS1220_CFG3_I1MUX	GENMASK(7, 5)
#define ADS1220_CFG3_I2MUX	GENMASK(4, 2)
#define ADS1220_CFG3_DRDYM	BIT(1)

/* VREF[1:0] sources */
#define ADS1220_VREF_INTERNAL	0
#define ADS1220_VREF_REFP0_REFN0 1
#define ADS1220_VREF_AIN0_AIN3	2
#define ADS1220_VREF_AVDD	3
#define ADS1220_INTERNAL_VREF_uV 2048000

/* Input multiplexer codes (Table 8-10) */
#define ADS1220_MUX_SINGLE(ain)	(0x8 | (ain))	/* AINx vs AVSS */
#define ADS1220_MUX_SHORTED	0x0e		/* (AVDD + AVSS) / 2 */

#define ADS1220_DATA_BYTES	3
#define ADS1220_DATA_BITS	24

#define ADS1220_NUM_GAINS	8	/* 1, 2, 4, 8, 16, 32, 64, 128 */
#define ADS1220_MAX_SE_GAIN	4	/* single-ended forces PGA bypass */

#define ADS1220_MAX_CHANNELS	7	/* 4 single-ended + 3 differential-ish */
#define ADS1220_MAX_AIN		4

/* Worst-case single conversion: 20 SPS => 50 ms, plus margin. */
#define ADS1220_CONV_TIMEOUT_MS	100
#define ADS1220_CONV_MARGIN_US	2000

#define ADS1220_SUSPEND_DELAY_MS 2000

/* Data rate (samples per second) in normal mode, indexed by DR[2:0]. */
static const int ads1220_datarates[] = {
	20, 45, 90, 175, 330, 600, 1000,
};

/*
 * Available scales expressed as gain reciprocals (val / val2), matching the
 * convention used by the sibling ti-ads1119 driver: writing 0.25 selects a
 * gain of 4. The full list is used for differential channels; single-ended
 * channels (which force the PGA into bypass) are limited to the first three
 * entries (gains 1, 2, 4).
 */
static const int ads1220_scale_avail[] = {
	1, 1,
	1, 2,
	1, 4,
	1, 8,
	1, 16,
	1, 32,
	1, 64,
	1, 128,
};

#define ADS1220_SE_SCALE_AVAIL_LEN	(3 * 2)
#define ADS1220_SCALE_AVAIL_LEN		ARRAY_SIZE(ads1220_scale_avail)

struct ads1220_channel_config {
	unsigned int mux;
	unsigned int gain;
	unsigned int datarate;
	bool single_ended;
};

struct ads1220_state {
	struct spi_device *spi;
	struct completion completion;
	struct iio_trigger *trig;
	struct ads1220_channel_config *channels_cfg;
	unsigned int num_channels_cfg;
	int vref_uV;
	unsigned int vref_source;

	/*
	 * DMA-safe buffers. tx is used for command/register writes, rx for
	 * register and conversion-result reads. scan holds one sample plus a
	 * timestamp for the triggered buffer.
	 */
	u8 tx[2] __aligned(IIO_DMA_MINALIGN);
	u8 rx[ADS1220_DATA_BYTES];
	struct {
		s32 sample;
		aligned_s64 timestamp;
	} scan;
};

static int ads1220_command(struct ads1220_state *st, u8 cmd)
{
	st->tx[0] = cmd;

	return spi_write(st->spi, st->tx, 1);
}

static int ads1220_write_reg(struct ads1220_state *st, u8 reg, u8 val)
{
	st->tx[0] = ADS1220_CMD_WREG_REG(reg);
	st->tx[1] = val;

	return spi_write(st->spi, st->tx, 2);
}

static int ads1220_read_reg(struct ads1220_state *st, u8 reg, u8 *val)
{
	int ret;

	st->tx[0] = ADS1220_CMD_RREG_REG(reg);

	ret = spi_write_then_read(st->spi, st->tx, 1, st->rx, 1);
	if (ret)
		return ret;

	*val = st->rx[0];

	return 0;
}

static int ads1220_reset(struct ads1220_state *st)
{
	int ret;

	ret = ads1220_command(st, ADS1220_CMD_RESET);
	if (ret)
		return ret;

	/* Wait at least 50us + 32 x tCLK after RESET before any command. */
	fsleep(100);

	return 0;
}

static unsigned int ads1220_datarate_to_code(unsigned int datarate)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(ads1220_datarates); i++)
		if (ads1220_datarates[i] == datarate)
			return i;

	return 0;
}

static int ads1220_configure(struct ads1220_state *st, unsigned int mux,
			     unsigned int gain, unsigned int datarate,
			     bool single_ended, bool continuous)
{
	u8 reg0, reg1;
	int ret;

	reg0 = FIELD_PREP(ADS1220_CFG0_MUX, mux) |
	       FIELD_PREP(ADS1220_CFG0_GAIN, ilog2(gain));
	/*
	 * For single-ended inputs (AINN = AVSS) the PGA must be bypassed; the
	 * datasheet only allows gains 1, 2 and 4 in that case.
	 */
	if (single_ended)
		reg0 |= ADS1220_CFG0_PGA_BYPASS;

	ret = ads1220_write_reg(st, ADS1220_REG_CONFIG0, reg0);
	if (ret)
		return ret;

	reg1 = FIELD_PREP(ADS1220_CFG1_DR, ads1220_datarate_to_code(datarate));
	if (continuous)
		reg1 |= ADS1220_CFG1_CM;

	return ads1220_write_reg(st, ADS1220_REG_CONFIG1, reg1);
}

static int ads1220_read_sample(struct ads1220_state *st, unsigned int datarate,
			       int *val)
{
	int ret;

	if (st->spi->irq) {
		unsigned long timeout = msecs_to_jiffies(ADS1220_CONV_TIMEOUT_MS);

		if (!wait_for_completion_timeout(&st->completion, timeout))
			return -ETIMEDOUT;
	} else {
		/*
		 * No DRDY interrupt: wait for the conversion to finish. In
		 * single-shot mode the result stays latched until the next
		 * START, so waiting longer than one conversion is harmless;
		 * wait two periods plus a margin to comfortably cover the
		 * oscillator start-up and its tolerance.
		 */
		fsleep(2 * DIV_ROUND_UP(MICRO, datarate) + ADS1220_CONV_MARGIN_US);
	}

	/*
	 * Once DRDY is low the result can be clocked out directly, MSB first,
	 * without an RDATA command (datasheet section 8.5.4).
	 */
	ret = spi_read(st->spi, st->rx, ADS1220_DATA_BYTES);
	if (ret)
		return ret;

	*val = sign_extend32(get_unaligned_be24(st->rx), ADS1220_DATA_BITS - 1);

	return 0;
}

static int ads1220_single_conversion(struct ads1220_state *st,
				     const struct iio_chan_spec *chan,
				     int *val, bool calib_offset)
{
	struct device *dev = &st->spi->dev;
	struct ads1220_channel_config *cfg = &st->channels_cfg[chan->address];
	unsigned int mux = cfg->mux;
	bool single_ended = cfg->single_ended;
	int ret;

	if (calib_offset) {
		mux = ADS1220_MUX_SHORTED;
		single_ended = false;
	}

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	ret = ads1220_configure(st, mux, cfg->gain, cfg->datarate,
				single_ended, false);
	if (ret)
		goto out;

	if (st->spi->irq)
		reinit_completion(&st->completion);

	ret = ads1220_command(st, ADS1220_CMD_START);
	if (ret)
		goto out;

	ret = ads1220_read_sample(st, cfg->datarate, val);
	if (ret)
		goto out;

	ret = IIO_VAL_INT;
out:
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

static int ads1220_read_raw(struct iio_dev *indio_dev,
			    const struct iio_chan_spec *chan,
			    int *val, int *val2, long mask)
{
	struct ads1220_state *st = iio_priv(indio_dev);
	struct ads1220_channel_config *cfg = &st->channels_cfg[chan->address];
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = ads1220_single_conversion(st, chan, val, false);
		iio_device_release_direct(indio_dev);
		return ret;
	case IIO_CHAN_INFO_OFFSET:
		if (!iio_device_claim_direct(indio_dev))
			return -EBUSY;
		ret = ads1220_single_conversion(st, chan, val, true);
		iio_device_release_direct(indio_dev);
		return ret;
	case IIO_CHAN_INFO_SCALE:
		/* scale [mV] = vref / (gain * 2^23); gain is a power of two. */
		*val = st->vref_uV / MILLI;
		*val2 = (chan->scan_type.realbits - 1) + ilog2(cfg->gain);
		return IIO_VAL_FRACTIONAL_LOG2;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*val = cfg->datarate;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int ads1220_read_avail(struct iio_dev *indio_dev,
			      const struct iio_chan_spec *chan,
			      const int **vals, int *type, int *length,
			      long mask)
{
	struct ads1220_state *st = iio_priv(indio_dev);
	struct ads1220_channel_config *cfg = &st->channels_cfg[chan->address];

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		*type = IIO_VAL_FRACTIONAL;
		*vals = ads1220_scale_avail;
		*length = cfg->single_ended ? ADS1220_SE_SCALE_AVAIL_LEN :
					      ADS1220_SCALE_AVAIL_LEN;
		return IIO_AVAIL_LIST;
	case IIO_CHAN_INFO_SAMP_FREQ:
		*type = IIO_VAL_INT;
		*vals = ads1220_datarates;
		*length = ARRAY_SIZE(ads1220_datarates);
		return IIO_AVAIL_LIST;
	default:
		return -EINVAL;
	}
}

static int ads1220_write_raw(struct iio_dev *indio_dev,
			     const struct iio_chan_spec *chan,
			     int val, int val2, long mask)
{
	struct ads1220_state *st = iio_priv(indio_dev);
	struct ads1220_channel_config *cfg = &st->channels_cfg[chan->address];
	unsigned int gain;
	int i;

	switch (mask) {
	case IIO_CHAN_INFO_SCALE:
		/* The available scales are the gain reciprocals (e.g. 1/4). */
		if (val == 0 && val2 == 0)
			return -EINVAL;

		gain = MICRO / (val * MICRO + val2);
		if (!is_power_of_2(gain) || gain > BIT(ADS1220_NUM_GAINS - 1))
			return -EINVAL;
		if (cfg->single_ended && gain > ADS1220_MAX_SE_GAIN)
			return -EINVAL;

		cfg->gain = gain;
		return 0;
	case IIO_CHAN_INFO_SAMP_FREQ:
		for (i = 0; i < ARRAY_SIZE(ads1220_datarates); i++) {
			if (ads1220_datarates[i] == val) {
				cfg->datarate = val;
				return 0;
			}
		}
		return -EINVAL;
	default:
		return -EINVAL;
	}
}

static int ads1220_debugfs_reg_access(struct iio_dev *indio_dev,
				      unsigned int reg, unsigned int writeval,
				      unsigned int *readval)
{
	struct ads1220_state *st = iio_priv(indio_dev);
	u8 val;
	int ret;

	if (reg > ADS1220_MAX_REG)
		return -EINVAL;

	if (readval) {
		ret = ads1220_read_reg(st, reg, &val);
		if (ret)
			return ret;
		*readval = val;
		return 0;
	}

	return ads1220_write_reg(st, reg, writeval);
}

static const struct iio_info ads1220_info = {
	.read_raw = ads1220_read_raw,
	.read_avail = ads1220_read_avail,
	.write_raw = ads1220_write_raw,
	.debugfs_reg_access = ads1220_debugfs_reg_access,
};

static int ads1220_buffer_preenable(struct iio_dev *indio_dev)
{
	struct ads1220_state *st = iio_priv(indio_dev);
	struct device *dev = &st->spi->dev;
	struct ads1220_channel_config *cfg;
	unsigned int index;
	int ret;

	index = find_first_bit(indio_dev->active_scan_mask,
			       iio_get_masklength(indio_dev));
	cfg = &st->channels_cfg[index];

	ret = pm_runtime_resume_and_get(dev);
	if (ret)
		return ret;

	ret = ads1220_configure(st, cfg->mux, cfg->gain, cfg->datarate,
				cfg->single_ended, true);
	if (ret)
		goto err;

	ret = ads1220_command(st, ADS1220_CMD_START);
	if (ret)
		goto err;

	return 0;
err:
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);
	return ret;
}

static int ads1220_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct ads1220_state *st = iio_priv(indio_dev);
	struct device *dev = &st->spi->dev;

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return 0;
}

static const struct iio_buffer_setup_ops ads1220_buffer_setup_ops = {
	.preenable = ads1220_buffer_preenable,
	.postdisable = ads1220_buffer_postdisable,
	.validate_scan_mask = &iio_validate_scan_mask_onehot,
};

static const struct iio_trigger_ops ads1220_trigger_ops = {
	.validate_device = &iio_trigger_validate_own_device,
};

static irqreturn_t ads1220_irq_handler(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = dev_id;
	struct ads1220_state *st = iio_priv(indio_dev);

	if (iio_buffer_enabled(indio_dev) && iio_trigger_using_own(indio_dev))
		iio_trigger_poll(indio_dev->trig);
	else
		complete(&st->completion);

	return IRQ_HANDLED;
}

static irqreturn_t ads1220_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ads1220_state *st = iio_priv(indio_dev);
	int ret;

	ret = spi_read(st->spi, st->rx, ADS1220_DATA_BYTES);
	if (ret) {
		dev_err(&st->spi->dev, "Failed to read sample: %d\n", ret);
		goto done;
	}

	st->scan.sample = sign_extend32(get_unaligned_be24(st->rx),
					ADS1220_DATA_BITS - 1);

	iio_push_to_buffers_with_ts(indio_dev, &st->scan, sizeof(st->scan),
				    iio_get_time_ns(indio_dev));
done:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int ads1220_map_mux(struct device *dev, u32 ain_pos, u32 ain_neg,
			   bool differential, unsigned int *mux,
			   bool *single_ended)
{
	static const u8 diff_mux[ADS1220_MAX_AIN][ADS1220_MAX_AIN] = {
		[0][1] = 0x0, [0][2] = 0x1, [0][3] = 0x2,
		[1][2] = 0x3, [1][3] = 0x4, [1][0] = 0x6,
		[2][3] = 0x5,
		[3][2] = 0x7,
	};

	if (!differential) {
		if (ain_pos >= ADS1220_MAX_AIN)
			return -EINVAL;
		*mux = ADS1220_MUX_SINGLE(ain_pos);
		*single_ended = true;
		return 0;
	}

	if (ain_pos >= ADS1220_MAX_AIN || ain_neg >= ADS1220_MAX_AIN)
		return -EINVAL;

	/* Only the input pairs the multiplexer can route are valid. */
	if (ain_pos == ain_neg || (diff_mux[ain_pos][ain_neg] == 0 &&
				   !(ain_pos == 0 && ain_neg == 1)))
		return -EINVAL;

	*mux = diff_mux[ain_pos][ain_neg];
	*single_ended = false;

	return 0;
}

static int ads1220_alloc_channels(struct iio_dev *indio_dev)
{
	const struct iio_chan_spec ads1220_channel = {
		.type = IIO_VOLTAGE,
		.indexed = 1,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
				      BIT(IIO_CHAN_INFO_SCALE) |
				      BIT(IIO_CHAN_INFO_OFFSET) |
				      BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.info_mask_separate_available = BIT(IIO_CHAN_INFO_SCALE) |
						BIT(IIO_CHAN_INFO_SAMP_FREQ),
		.scan_type = {
			.sign = 's',
			.realbits = ADS1220_DATA_BITS,
			.storagebits = 32,
			.endianness = IIO_CPU,
		},
	};
	const struct iio_chan_spec ads1220_ts = IIO_CHAN_SOFT_TIMESTAMP(0);
	struct ads1220_state *st = iio_priv(indio_dev);
	struct device *dev = &st->spi->dev;
	struct iio_chan_spec *channels, *chan;
	unsigned int num_channels, i = 0;
	int ret;

	st->num_channels_cfg = device_get_child_node_count(dev);
	if (st->num_channels_cfg == 0 ||
	    st->num_channels_cfg > ADS1220_MAX_CHANNELS)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid channel count %u (max %u)\n",
				     st->num_channels_cfg, ADS1220_MAX_CHANNELS);

	st->channels_cfg = devm_kcalloc(dev, st->num_channels_cfg,
					sizeof(*st->channels_cfg), GFP_KERNEL);
	if (!st->channels_cfg)
		return -ENOMEM;

	/* One extra channel for the timestamp. */
	num_channels = st->num_channels_cfg + 1;
	channels = devm_kcalloc(dev, num_channels, sizeof(*channels),
				GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	device_for_each_child_node_scoped(dev, child) {
		struct ads1220_channel_config *cfg = &st->channels_cfg[i];
		bool differential;
		u32 ain[2];

		differential = fwnode_property_present(child, "diff-channels");
		if (differential)
			ret = fwnode_property_read_u32_array(child,
							     "diff-channels",
							     ain, 2);
		else
			ret = fwnode_property_read_u32(child, "single-channel",
						       &ain[0]);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to read channel property\n");

		ret = ads1220_map_mux(dev, ain[0], ain[1], differential,
				      &cfg->mux, &cfg->single_ended);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Invalid input combination\n");

		cfg->gain = 1;
		cfg->datarate = ads1220_datarates[0];

		chan = &channels[i];
		*chan = ads1220_channel;
		chan->channel = ain[0];
		chan->address = i;
		chan->scan_index = i;
		if (differential) {
			chan->channel2 = ain[1];
			chan->differential = 1;
		}

		i++;
	}

	channels[i] = ads1220_ts;
	channels[i].scan_index = i;

	indio_dev->channels = channels;
	indio_dev->num_channels = num_channels;

	return 0;
}

static int ads1220_init(struct ads1220_state *st)
{
	u8 reg2;
	int ret;

	ret = ads1220_reset(st);
	if (ret)
		return ret;

	reg2 = FIELD_PREP(ADS1220_CFG2_VREF, st->vref_source);

	ret = ads1220_write_reg(st, ADS1220_REG_CONFIG2, reg2);
	if (ret)
		return ret;

	/* DRDY only on the dedicated pin (DRDYM = 0). */
	return ads1220_write_reg(st, ADS1220_REG_CONFIG3, 0);
}

static void ads1220_powerdown(void *data)
{
	struct ads1220_state *st = data;

	ads1220_command(st, ADS1220_CMD_POWERDOWN);
}

static int ads1220_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct iio_dev *indio_dev;
	struct ads1220_state *st;
	int avdd_uV;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->spi = spi;
	spi_set_drvdata(spi, indio_dev);

	/* The ADS1220 uses SPI mode 1 (CPOL = 0, CPHA = 1). */
	spi->mode |= SPI_CPHA;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "SPI setup failed\n");

	indio_dev->name = "ads1220";
	indio_dev->info = &ads1220_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = devm_regulator_get_enable(dev, "dvdd");
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable dvdd\n");

	avdd_uV = devm_regulator_get_enable_read_voltage(dev, "avdd");
	if (avdd_uV < 0)
		return dev_err_probe(dev, avdd_uV, "Failed to get avdd\n");

	/*
	 * Reference source, in priority order:
	 *  - external reference on REFP0/REFN0 if a "vref" regulator is given;
	 *  - the analog supply (AVDD) for ratiometric single-supply setups if
	 *    "ti,vref-avdd" is set - no extra pins, full 0..AVDD input range;
	 *  - otherwise the internal 2.048V reference.
	 */
	st->vref_uV = devm_regulator_get_enable_read_voltage(dev, "vref");
	if (st->vref_uV >= 0) {
		st->vref_source = ADS1220_VREF_REFP0_REFN0;
	} else if (st->vref_uV != -ENODEV) {
		return dev_err_probe(dev, st->vref_uV, "Failed to get vref\n");
	} else if (device_property_read_bool(dev, "ti,vref-avdd")) {
		st->vref_source = ADS1220_VREF_AVDD;
		st->vref_uV = avdd_uV;
	} else {
		st->vref_source = ADS1220_VREF_INTERNAL;
		st->vref_uV = ADS1220_INTERNAL_VREF_uV;
	}

	ret = ads1220_alloc_channels(indio_dev);
	if (ret)
		return ret;

	init_completion(&st->completion);

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev, NULL,
					      ads1220_trigger_handler,
					      &ads1220_buffer_setup_ops);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Failed to set up IIO buffer\n");

	if (spi->irq > 0) {
		ret = devm_request_irq(dev, spi->irq, ads1220_irq_handler,
				       IRQF_NO_THREAD, "ads1220", indio_dev);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to request irq\n");

		st->trig = devm_iio_trigger_alloc(dev, "%s-dev%d",
						  indio_dev->name,
						  iio_device_id(indio_dev));
		if (!st->trig)
			return -ENOMEM;

		st->trig->ops = &ads1220_trigger_ops;
		iio_trigger_set_drvdata(st->trig, indio_dev);

		ret = devm_iio_trigger_register(dev, st->trig);
		if (ret)
			return dev_err_probe(dev, ret,
					     "Failed to register trigger\n");
	}

	ret = ads1220_init(st);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to initialize device\n");

	pm_runtime_set_autosuspend_delay(dev, ADS1220_SUSPEND_DELAY_MS);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_active(dev);

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to enable pm runtime\n");

	ret = devm_add_action_or_reset(dev, ads1220_powerdown, st);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static int ads1220_runtime_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct ads1220_state *st = iio_priv(indio_dev);

	return ads1220_command(st, ADS1220_CMD_POWERDOWN);
}

static int ads1220_runtime_resume(struct device *dev)
{
	/*
	 * A START/SYNC command wakes the analog parts from power-down; it is
	 * issued by the conversion path, so there is nothing to do here beyond
	 * letting the device settle after the supplies are active again.
	 */
	fsleep(100);

	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(ads1220_pm_ops, ads1220_runtime_suspend,
				 ads1220_runtime_resume, NULL);

static const struct spi_device_id ads1220_id[] = {
	{ "ads1220" },
	{ }
};
MODULE_DEVICE_TABLE(spi, ads1220_id);

static const struct of_device_id ads1220_of_match[] = {
	{ .compatible = "ti,ads1220" },
	{ }
};
MODULE_DEVICE_TABLE(of, ads1220_of_match);

static struct spi_driver ads1220_driver = {
	.driver = {
		.name = "ads1220",
		.of_match_table = ads1220_of_match,
		.pm = pm_ptr(&ads1220_pm_ops),
	},
	.probe = ads1220_probe,
	.id_table = ads1220_id,
};
module_spi_driver(ads1220_driver);

MODULE_DESCRIPTION("Texas Instruments ADS1220 ADC Driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nguyen Minh Tien <zizuzacker@gmail.com>");
