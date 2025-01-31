// SPDX-License-Identifier: GPL-2.0-only
/*
 * ROHM ADC driver for BD79124 ADC/GPO device
 * https://fscdn.rohm.com/en/products/databook/datasheet/ic/data_converter/dac/bd79124muf-c-e.pdf
 *
 * Copyright (c) 2025, ROHM Semiconductor.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/byteorder/generic.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/types.h>

#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/mfd/rohm-bd79124.h>

#define BD79124_ADC_BITS 12
#define BD79124_MASK_CONV_MODE GENMASK(6, 5)
#define BD79124_MASK_AUTO_INTERVAL GENMASK(1, 0)
#define BD79124_CONV_MODE_MANSEQ 0
#define BD79124_CONV_MODE_AUTO 1
#define BD79124_INTERVAL_075 0
#define BD79124_INTERVAL_150 1
#define BD79124_INTERVAL_300 2
#define BD79124_INTERVAL_600 3

#define BD79124_MASK_DWC_EN BIT(4)
#define BD79124_MASK_STATS_EN BIT(5)
#define BD79124_MASK_SEQ_START BIT(4)
#define BD79124_MASK_SEQ_MODE GENMASK(1, 0)
#define BD79124_MASK_SEQ_MANUAL 0
#define BD79124_MASK_SEQ_SEQ 1

#define BD79124_MASK_HYSTERESIS GENMASK(3, 0)
#define BD79124_LOW_LIMIT_MIN 0
#define BD79124_HIGH_LIMIT_MAX GENMASK(11, 0)

/*
 * The high limit, low limit and last measurement result are each stored in
 * 2 consequtive registers. 4 bits are in the high bits of the 1.st register
 * and 8 bits in the next register.
 *
 * These macros return the address of the 1.st reg for the given channel
 */
#define BD79124_GET_HIGH_LIMIT_REG(ch) (BD79124_REG_HYSTERESIS_CH0 + (ch) * 4)
#define BD79124_GET_LOW_LIMIT_REG(ch) (BD79124_REG_EVENTCOUNT_CH0 + (ch) * 4)
#define BD79124_GET_RECENT_RES_REG(ch) (BD79124_REG_RECENT_CH0_LSB + (ch) * 2)

/*
 * The hysteresis for a channel is stored in the same register where the
 * 4 bits of high limit reside.
 */
#define BD79124_GET_HYSTERESIS_REG(ch) BD79124_GET_HIGH_LIMIT_REG(ch)

enum {
	BD79124_CH_0,
	BD79124_CH_1,
	BD79124_CH_2,
	BD79124_CH_3,
	BD79124_CH_4,
	BD79124_CH_5,
	BD79124_CH_6,
	BD79124_CH_7,
	BD79124_MAX_NUM_CHANNELS
};

struct bd79124_data {
	s64 timestamp;
	struct regmap *map;
	struct device *dev;
	int vmax;
	int alarm_monitored[BD79124_MAX_NUM_CHANNELS];
	/*
	 * The BD79124 is configured to run the measurements in the background.
	 * This is done for the event monitoring as well as for the read_raw().
	 * Protect the measurement starting/stopping using a mutex.
	 */
	struct mutex mutex;
};

struct bd79124_raw {
	u8 bit0_3; /* Is set in high bits of the byte */
	u8 bit4_11;
};
#define BD79124_RAW_TO_INT(r) ((r.bit4_11 << 4) | (r.bit0_3 >> 4))

/*
 * The high and low limits as well as the recent result values are stored in
 * the same way in 2 consequent registers. The first register contains 4 bits
 * of the value. These bits are stored in the high bits [7:4] of register, but
 * they represent the low bits [3:0] of the value.
 * The value bits [11:4] are stored in the next register.
 *
 * Read data from register and convert to integer.
 */
static int bd79124_read_reg_to_int(struct bd79124_data *d, int reg,
				   unsigned int *val)
{
	int ret;
	struct bd79124_raw raw;

	ret = regmap_bulk_read(d->map, reg, &raw, sizeof(raw));
	if (ret)
		dev_dbg(d->dev, "bulk_read failed %d\n", ret);
	*val = BD79124_RAW_TO_INT(raw);

	return ret;
}

/*
 * The high and low limits as well as the recent result values are stored in
 * the same way in 2 consequent registers. The first register contains 4 bits
 * of the value. These bits are stored in the high bits [7:4] of register, but
 * they represent the low bits [3:0] of the value.
 * The value bits [11:4] are stored in the next regoster.
 *
 * Conver the integer to register format and write it using rmw cycle.
 */
static int bd79124_write_int_to_reg(struct bd79124_data *d, int reg,
				    unsigned int val)
{
	struct bd79124_raw raw;
	int ret, tmp;

	raw.bit4_11 = (u8)(val >> 4);
	raw.bit0_3 = (u8)(val << 4);

	ret = regmap_read(d->map, reg, &tmp);
	if (ret)
		return ret;

	raw.bit0_3 |= (0xf & tmp);

	return regmap_bulk_write(d->map, reg, &raw, sizeof(raw));
}

static const struct iio_event_spec bd79124_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_ENABLE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_HYSTERESIS),
	},
};

#define BD79124_CHAN(idx) {						\
	.type = IIO_VOLTAGE,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
	.indexed = 1,							\
	.channel = idx,							\
}

#define BD79124_CHAN_EV(idx) {						\
	.type = IIO_VOLTAGE,						\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
	.indexed = 1,							\
	.channel = idx,							\
	.event_spec = bd79124_events,					\
	.num_event_specs = ARRAY_SIZE(bd79124_events),			\
}

static const struct iio_chan_spec bd79124_channels[] = {
	BD79124_CHAN_EV(0),
	BD79124_CHAN_EV(1),
	BD79124_CHAN_EV(2),
	BD79124_CHAN_EV(3),
	BD79124_CHAN_EV(4),
	BD79124_CHAN_EV(5),
	BD79124_CHAN_EV(6),
	BD79124_CHAN_EV(7),
};

static const struct iio_chan_spec bd79124_channels_noirq[] = {
	BD79124_CHAN(0),
	BD79124_CHAN(1),
	BD79124_CHAN(2),
	BD79124_CHAN(3),
	BD79124_CHAN(4),
	BD79124_CHAN(5),
	BD79124_CHAN(6),
	BD79124_CHAN(7),
};

/*
 * The BD79124 supports muxing the pins as ADC inputs or as a general purpose
 * output. This muxing is handled by a pinmux driver. Here we just check the
 * settings from the register, and disallow using the pin if pinmux is set to
 * GPO.
 *
 * NOTE: This driver does not perform any locking to ensure the pinmux stays
 * toggled to ADC for the duration of the whatever operation is being done.
 * It is responsibility of the user to configure the pinmux.
 */
static bool bd79124_chan_is_adc(struct bd79124_data *d, unsigned int offset)
{
	int ret, val;

	ret = regmap_read(d->map, BD79124_REG_PINCFG, &val);
	/* If read fails, don't allow using as AIN (to be on a safe side) */
	if (ret)
		return 0;

	return !(val & BIT(offset));
}

static int bd79124_read_event_value(struct iio_dev *idev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info, int *val,
				    int *val2)
{
	struct bd79124_data *d = iio_priv(idev);
	int ret, reg;

	if (chan->channel >= BD79124_MAX_NUM_CHANNELS)
		return -EINVAL;

	/* ensure pinmux is set to ADC */
	if (!bd79124_chan_is_adc(d, chan->channel))
		return -EBUSY;

	switch (info) {
	case IIO_EV_INFO_VALUE:
		if (dir == IIO_EV_DIR_RISING)
			reg = BD79124_GET_HIGH_LIMIT_REG(chan->channel);
		else if (dir == IIO_EV_DIR_FALLING)
			reg = BD79124_GET_LOW_LIMIT_REG(chan->channel);
		else
			return -EINVAL;

		ret = bd79124_read_reg_to_int(d, reg, val);
		if (ret)
			return ret;

		return IIO_VAL_INT;

	case IIO_EV_INFO_HYSTERESIS:
		reg = BD79124_GET_HYSTERESIS_REG(chan->channel);
		ret = regmap_read(d->map, reg, val);
		if (ret)
			return ret;
		/* Mask the non hysteresis bits */
		*val &= BD79124_MASK_HYSTERESIS;
		/*
		 * The data-sheet says the hysteresis register value needs to be
		 * sifted left by 3 (or multiplied by 8, depending on the
		 * page :] )
		 */
		*val <<= 3;

		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static int bd79124_start_measurement(struct bd79124_data *d, int chan)
{
	int val, ret, regval;

	/* ensure pinmux is set to ADC */
	if (!bd79124_chan_is_adc(d, chan))
		return -EBUSY;

	/* See if already started */
	ret = regmap_read(d->map, BD79124_REG_AUTO_CHANNELS, &val);
	if (val & BIT(chan))
		return 0;

	/* Stop the sequencer */
	ret = regmap_clear_bits(d->map, BD79124_REG_SEQUENCE_CFG,
				BD79124_MASK_SEQ_START);
	if (ret)
		return ret;

	/* Add the channel to measured channels */
	ret = regmap_write(d->map, BD79124_REG_AUTO_CHANNELS, val | BIT(chan));
	if (ret)
		return ret;

	/* Restart the sequencer */
	ret = regmap_set_bits(d->map, BD79124_REG_SEQUENCE_CFG,
			      BD79124_MASK_SEQ_START);
	if (ret)
		return ret;

	/*
	 * Start the measurement at the background. Don't bother checking if
	 * it was started, regmap has cache
	 */
	regval = FIELD_PREP(BD79124_MASK_CONV_MODE, BD79124_CONV_MODE_AUTO);

	return regmap_update_bits(d->map, BD79124_REG_OPMODE_CFG,
				BD79124_MASK_CONV_MODE, regval);
}

static int bd79124_stop_measurement(struct bd79124_data *d, int chan)
{
	int val, ret;

	/* Ensure pinmux is set to ADC */
	if (!bd79124_chan_is_adc(d, chan))
		return -EBUSY;

	/* If alarm is requested for the channel we won't stop measurement */
	if (d->alarm_monitored[chan])
		return 0;

	/* See if already stopped */
	ret = regmap_read(d->map, BD79124_REG_AUTO_CHANNELS, &val);
	if (!(val & BIT(chan)))
		return 0;

	/* Stop the sequencer */
	ret = regmap_clear_bits(d->map, BD79124_REG_SEQUENCE_CFG,
				BD79124_MASK_SEQ_START);

	/* Clear the channel from the measured channels */
	ret = regmap_write(d->map, BD79124_REG_AUTO_CHANNELS,
			   (~BIT(chan)) & val);
	if (ret)
		return ret;

	/* Stop background conversion if it was the last channel */
	if (!((~BIT(chan)) & val)) {
		int regval = FIELD_PREP(BD79124_MASK_CONV_MODE,
					BD79124_CONV_MODE_MANSEQ);

		ret = regmap_update_bits(d->map, BD79124_REG_OPMODE_CFG,
					 BD79124_MASK_CONV_MODE, regval);
		if (ret)
			return ret;
	}

	/* Restart the sequencer */
	return regmap_set_bits(d->map, BD79124_REG_SEQUENCE_CFG,
			       BD79124_MASK_SEQ_START);
}

static int bd79124_read_event_config(struct iio_dev *idev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir)
{
	struct bd79124_data *d = iio_priv(idev);
	int val, ret, reg, disabled;

	if (chan->channel >= BD79124_MAX_NUM_CHANNELS)
		return -EINVAL;

	/* ensure pinmux is set to ADC */
	if (!bd79124_chan_is_adc(d, chan->channel))
		return -EBUSY;

	ret = regmap_read(d->map, BD79124_REG_ALERT_CH_SEL, &val);
	if (ret)
		return ret;

	/* The event is disabled if alerts for the channel are disabled */
	if (!(val & BIT(chan->channel)))
		return 0;

	/*
	 * If alerts are on, then the event may be disabled if limit is set to
	 * the one extreme. (HW does not support disabling rising/falling
	 * thresholds independently. Hence we resort to setting high limit to
	 * MAX, or low limit to 0 to try effectively disable thresholds).
	 */
	if (dir == IIO_EV_DIR_RISING) {
		reg = BD79124_GET_HIGH_LIMIT_REG(chan->channel);
		disabled = BD79124_HIGH_LIMIT_MAX;
	} else if (dir == IIO_EV_DIR_FALLING) {
		reg = BD79124_GET_LOW_LIMIT_REG(chan->channel);
		disabled = BD79124_LOW_LIMIT_MIN;
	} else {
		return -EINVAL;
	}
	ret = bd79124_read_reg_to_int(d, reg, &val);
	if (ret)
		return ret;

	return val != disabled;
}

static int bd79124_disable_event(struct bd79124_data *d,
				 enum iio_event_direction dir, int channel)
{
	int dir_bit = BIT(dir), reg;
	unsigned int limit;

	d->alarm_monitored[channel] &= (~dir_bit);
	/*
	 * Set thresholds either to 0 or to 2^12 - 1 as appropriate to prevent
	 * alerts and thus disable event generation.
	 */
	if (dir == IIO_EV_DIR_RISING) {
		reg = BD79124_GET_HIGH_LIMIT_REG(channel);
		limit = BD79124_HIGH_LIMIT_MAX;
	} else if (dir == IIO_EV_DIR_FALLING) {
		reg = BD79124_GET_LOW_LIMIT_REG(channel);
		limit = BD79124_LOW_LIMIT_MIN;
	} else {
		return -EINVAL;
	}

	/*
	 * Stop measurement if there is no more events to monitor.
	 * We don't bother checking the retval because the limit
	 * setting should in any case effectively disable the alarm.
	 */
	if (!d->alarm_monitored[channel]) {
		bd79124_stop_measurement(d, channel);
		regmap_clear_bits(d->map, BD79124_REG_ALERT_CH_SEL,
			       BIT(channel));
	}

	return bd79124_write_int_to_reg(d, reg, limit);
}

/* Do we need to disable the measurement for the duration of the limit conf ? */
static int bd79124_write_event_config(struct iio_dev *idev,
				      const struct iio_chan_spec *chan,
				      enum iio_event_type type,
				      enum iio_event_direction dir, bool state)
{
	struct bd79124_data *d = iio_priv(idev);
	int dir_bit = BIT(dir);

	guard(mutex)(&d->mutex);

	if (chan->channel >= BD79124_MAX_NUM_CHANNELS)
		return -EINVAL;

	/* ensure pinmux is set to ADC */
	if (!bd79124_chan_is_adc(d, chan->channel))
		return -EBUSY;

	if (state) {
		int ret;

		/* Set channel to be measured */
		ret = bd79124_start_measurement(d, chan->channel);
		if (ret)
			return ret;

		d->alarm_monitored[chan->channel] |= dir_bit;

		/* Add the channel to the list of monitored channels */
		ret = regmap_set_bits(d->map, BD79124_REG_ALERT_CH_SEL,
				      BIT(chan->channel));
		if (ret)
			return ret;

		/*
		 * Enable comparator. Trust the regmap cache, no need to check
		 * if it was already enabled.
		 *
		 * We could do this in the hw-init, but there may be users who
		 * never enable alarms and for them it makes sense to not
		 * enable the comparator at probe.
		 */
		return regmap_set_bits(d->map, BD79124_REG_GEN_CFG,
				      BD79124_MASK_DWC_EN);
	}

	return bd79124_disable_event(d, dir, chan->channel);
}

static int bd79124_write_event_value(struct iio_dev *idev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir,
				     enum iio_event_info info, int val,
				     int val2)
{
	struct bd79124_data *d = iio_priv(idev);
	int reg;

	if (chan->channel >= BD79124_MAX_NUM_CHANNELS)
		return -EINVAL;

	/* ensure pinmux is set to ADC */
	if (!bd79124_chan_is_adc(d, chan->channel))
		return -EBUSY;

	switch (info) {
	case IIO_EV_INFO_VALUE:
		if (dir == IIO_EV_DIR_RISING)
			reg = BD79124_GET_HIGH_LIMIT_REG(chan->channel);
		else if (dir == IIO_EV_DIR_FALLING)
			reg = BD79124_GET_LOW_LIMIT_REG(chan->channel);
		else
			return -EINVAL;

		return bd79124_write_int_to_reg(d, reg, val);

	case IIO_EV_INFO_HYSTERESIS:
			reg = BD79124_GET_HYSTERESIS_REG(chan->channel);
			val >>= 3;

		return regmap_update_bits(d->map, reg, BD79124_MASK_HYSTERESIS,
					  val);
	default:
		return -EINVAL;
	}
}

static int bd79124_read_last_result(struct bd79124_data *d, int chan,
				    int *result)
{
	struct bd79124_raw raw;
	int ret;

	ret = regmap_bulk_read(d->map, BD79124_GET_RECENT_RES_REG(chan), &raw,
			       sizeof(raw));
	if (ret)
		return ret;

	*result = BD79124_RAW_TO_INT(raw);

	return 0;
}

static int bd79124_single_chan_seq(struct bd79124_data *d, int chan, int *old)
{
	int ret;

	/* Stop the sequencer */
	ret = regmap_clear_bits(d->map, BD79124_REG_SEQUENCE_CFG,
				BD79124_MASK_SEQ_START);
	if (ret)
		return ret;

	/*
	 * It may be we have some channels monitored for alarms so we want to
	 * cache the old config and return it when the single channel
	 * measurement has been completed. Read the old config.
	 */
	ret = regmap_read(d->map, BD79124_REG_AUTO_CHANNELS, old);
	if (ret)
		return ret;

	ret = regmap_write(d->map, BD79124_REG_AUTO_CHANNELS, BIT(chan));
	if (ret)
		return ret;

	/* Restart the sequencer */
	return regmap_set_bits(d->map, BD79124_REG_SEQUENCE_CFG,
			      BD79124_MASK_SEQ_START);
}

static int bd79124_single_chan_seq_end(struct bd79124_data *d, int old)
{
	int ret;

	/* Stop the sequencer */
	ret = regmap_clear_bits(d->map, BD79124_REG_SEQUENCE_CFG,
				BD79124_MASK_SEQ_START);
	if (ret)
		return ret;

	ret = regmap_write(d->map, BD79124_REG_AUTO_CHANNELS, old);
	if (ret)
		return ret;

	/* Restart the sequencer */
	return regmap_set_bits(d->map, BD79124_REG_SEQUENCE_CFG,
			      BD79124_MASK_SEQ_START);
}

static int bd79124_read_raw(struct iio_dev *idev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long m)
{
	struct bd79124_data *d = iio_priv(idev);
	int ret;

	if (chan->channel >= BD79124_MAX_NUM_CHANNELS)
		return -EINVAL;

	switch (m) {
	case IIO_CHAN_INFO_RAW:
	{
		int old_chan_cfg, tmp;
		int regval = FIELD_PREP(BD79124_MASK_CONV_MODE,
					BD79124_CONV_MODE_AUTO);

		guard(mutex)(&d->mutex);

		/* ensure pinmux is set to ADC */
		if (!bd79124_chan_is_adc(d, chan->channel))
			return -EBUSY;

		/*
		 * Start the automatic conversion. This is needed here if no
		 * events have been enabled.
		 */
		ret = regmap_update_bits(d->map, BD79124_REG_OPMODE_CFG,
			BD79124_MASK_CONV_MODE, regval);
		if (ret)
			return ret;

		ret = bd79124_single_chan_seq(d, chan->channel, &old_chan_cfg);
		if (ret)
			return ret;

		/*
		 * The maximum conversion time is 6 uS. Ensure the sample is
		 * ready
		 */
		udelay(6);

		ret = bd79124_read_last_result(d, chan->channel, val);
		/* Try unconditionally returning the chan config */
		tmp = bd79124_single_chan_seq_end(d, old_chan_cfg);
		if (tmp)
			dev_err(d->dev,
				"Failed to return config. Alarms may be disabled\n");

		if (ret)
			return ret;

		return IIO_VAL_INT;
	}
	case IIO_CHAN_INFO_SCALE:
		*val = d->vmax / 1000;
		*val2 = BD79124_ADC_BITS;
		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static const struct iio_info bd79124_info = {
	.read_raw = bd79124_read_raw,
	.read_event_config = &bd79124_read_event_config,
	.write_event_config = &bd79124_write_event_config,
	.read_event_value = &bd79124_read_event_value,
	.write_event_value = &bd79124_write_event_value,
};

static irqreturn_t bd79124_event_handler(int irq, void *priv)
{
	int ret, i_hi, i_lo, i;
	struct iio_dev *idev = priv;
	struct bd79124_data *d = iio_priv(idev);

	/*
	 * Return IRQ_NONE if bailing-out without acking. This allows the IRQ
	 * subsystem to disable the offending IRQ line if we get a hardware
	 * problem. This behaviour has saved my poor bottom a few times in the
	 * past as, instead of getting unusably unresponsive, the system has
	 * spilled out the magic words "...nobody cared".
	 */
	ret = regmap_read(d->map, BD79124_REG_EVENT_FLAG_HI, &i_hi);
	if (ret)
		return IRQ_NONE;

	ret = regmap_read(d->map, BD79124_REG_EVENT_FLAG_LO, &i_lo);
	if (ret)
		return IRQ_NONE;

	if (!i_lo && !i_hi)
		return IRQ_NONE;

	for (i = 0; i < BD79124_MAX_NUM_CHANNELS; i++) {
		u64 ecode;

		if (BIT(i) & i_hi) {
			ecode = IIO_UNMOD_EVENT_CODE(IIO_VOLTAGE, i,
					IIO_EV_TYPE_THRESH, IIO_EV_DIR_RISING);

			iio_push_event(idev, ecode, d->timestamp);
			/*
			 * The BD79124 keeps the IRQ asserted for as long as
			 * the voltage exceeds the threshold. It may not serve
			 * the purpose to keep the IRQ firing and events
			 * generated in a loop because it may yield the
			 * userspace to have some problems when event handling
			 * there is slow.
			 *
			 * Thus, we disable the event for the channel. Userspace
			 * needs to re-enable the event.
			 *
			 * We don't check the result as there is not much to do.
			 * Also, this should not lead to total IRQ storm
			 * because the BD79124 is running in autonomous mode,
			 * which means there is by minimum 0.75 mS idle time
			 * between changing the channels. That should be
			 * sufficient to show some life on system, even if the
			 * event handling couldn't keep up.
			 */
			bd79124_disable_event(d, IIO_EV_DIR_RISING, i);
		}
		if (BIT(i) & i_lo) {
			ecode = IIO_UNMOD_EVENT_CODE(IIO_VOLTAGE, i,
					IIO_EV_TYPE_THRESH, IIO_EV_DIR_FALLING);

			iio_push_event(idev, ecode, d->timestamp);
		}
	}

	ret = regmap_write(d->map, BD79124_REG_EVENT_FLAG_HI, i_hi);
	if (ret)
		return IRQ_NONE;

	ret = regmap_write(d->map, BD79124_REG_EVENT_FLAG_LO, i_lo);
	if (ret)
		return IRQ_NONE;

	return IRQ_HANDLED;
}

static irqreturn_t bd79124_irq_handler(int irq, void *priv)
{
	struct iio_dev *idev = priv;
	struct bd79124_data *d = iio_priv(idev);

	d->timestamp = iio_get_time_ns(idev);

	return IRQ_WAKE_THREAD;
}

static int bd79124_hw_init(struct bd79124_data *d)
{
	int ret, regval;

	/* Stop auto sequencer */
	ret = regmap_clear_bits(d->map, BD79124_REG_SEQUENCE_CFG,
				BD79124_MASK_SEQ_START);
	if (ret)
		return ret;

	/* Enable writing the measured values to the regsters */
	ret = regmap_set_bits(d->map, BD79124_REG_GEN_CFG,
				 BD79124_MASK_STATS_EN);
	if (ret)
		return ret;

	/* Set no channels to be auto-measured */
	ret = regmap_write(d->map, BD79124_REG_AUTO_CHANNELS, 0x0);
	if (ret)
		return ret;

	/* Set no channels to be manually measured */
	ret = regmap_write(d->map, BD79124_REG_MANUAL_CHANNELS, 0x0);
	if (ret)
		return ret;

	/* Set the measurement interval to 0.75 mS */

	regval = FIELD_PREP(BD79124_MASK_AUTO_INTERVAL, BD79124_INTERVAL_075);
	ret = regmap_update_bits(d->map, BD79124_REG_OPMODE_CFG,
			BD79124_MASK_AUTO_INTERVAL, regval);
	if (ret)
		return ret;

	/* Sequencer mode to auto */
	ret = regmap_set_bits(d->map, BD79124_REG_SEQUENCE_CFG,
			      BD79124_MASK_SEQ_SEQ);
	if (ret)
		return ret;

	regval = FIELD_PREP(BD79124_MASK_CONV_MODE, BD79124_CONV_MODE_MANSEQ);
	/* Don't start the measurement */
	return regmap_update_bits(d->map, BD79124_REG_OPMODE_CFG,
			BD79124_MASK_CONV_MODE, BD79124_CONV_MODE_MANSEQ);

}

#define BD79124_VDD_MAX 5250000
#define BD79124_VDD_MIN 2700000

static int bd79124_probe(struct platform_device *pdev)
{
	struct bd79124_data *d;
	struct iio_dev *idev;
	int ret, irq, *parent_data;

	idev = devm_iio_device_alloc(&pdev->dev, sizeof(*d));
	if (!idev)
		return -ENOMEM;

	d = iio_priv(idev);

	parent_data = dev_get_drvdata(pdev->dev.parent);
	if (!parent_data)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "reference voltage missing\n");

	/*
	 * Recommended VDD voltage from the data-sheet:
	 * Analog/Digital Supply Voltage VDD 2.70 - 5.25 V
	 */
	d->vmax = *parent_data;
	if (d->vmax < BD79124_VDD_MIN || d->vmax > BD79124_VDD_MAX) {
		return dev_err_probe(d->dev, -EINVAL,
				     "VDD (%d) out of range [%d - %d]\n",
				     d->vmax, BD79124_VDD_MIN, BD79124_VDD_MAX);

		return -EINVAL;
	}

	irq = platform_get_irq_byname_optional(pdev, "thresh-alert");
	if (irq < 0) {
		if (irq == -EPROBE_DEFER)
			return irq;

		idev->channels = &bd79124_channels_noirq[0];
		idev->num_channels = ARRAY_SIZE(bd79124_channels_noirq);
		dev_dbg(d->dev, "No IRQ found, events disabled\n");
	} else {
		idev->channels = &bd79124_channels[0];
		idev->num_channels = ARRAY_SIZE(bd79124_channels);
	}

	idev->info = &bd79124_info;
	idev->name = "bd79124";
	idev->modes = INDIO_DIRECT_MODE;

	d->dev = &pdev->dev;
	d->map = dev_get_regmap(d->dev->parent, NULL);
	if (!d->map)
		return dev_err_probe(d->dev, -ENODEV, "No regmap\n");

	mutex_init(&d->mutex);

	ret = bd79124_hw_init(d);
	if (ret)
		return ret;

	if (irq > 0) {
		ret = devm_request_threaded_irq(d->dev, irq, bd79124_irq_handler,
					&bd79124_event_handler, IRQF_ONESHOT,
					"adc-thresh-alert", idev);
		if (ret)
			return dev_err_probe(d->dev, ret,
					     "Failed to register IRQ\n");
	}

	return devm_iio_device_register(d->dev, idev);
}

static const struct platform_device_id bd79124_adc_id[] = {
	{ "bd79124-adc", },
	{ }
};
MODULE_DEVICE_TABLE(platform, bd79124_adc_id);

static struct platform_driver bd79124_driver = {
	.driver = {
		.name = "bd79124-adc",
		/*
		 * Probing explicitly requires a few millisecond of sleep.
		 * Enabling the VDD regulator may include ramp up rates.
		 */
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe = bd79124_probe,
	.id_table = bd79124_adc_id,
};
module_platform_driver(bd79124_driver);

MODULE_AUTHOR("Matti Vaittinen <mazziesaccount@gmail.com>");
MODULE_DESCRIPTION("Driver for ROHM BD79124 ADC");
MODULE_LICENSE("GPL");
