// SPDX-License-Identifier: GPL-2.0
/*
 * AMD Versal SysMon core driver
 *
 * Copyright (C) 2019 - 2022, Xilinx, Inc.
 * Copyright (C) 2022 - 2026, Advanced Micro Devices, Inc.
 */

#include <linux/array_size.h>
#include <linux/bitfield.h>
#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/devm-helpers.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/units.h>

#include <linux/iio/events.h>
#include <linux/iio/iio.h>

#include "versal-sysmon.h"

/* OT and TEMP hysteresis mode bits in SYSMON_TEMP_EV_CFG */
#define SYSMON_OT_HYST_MASK		BIT(0)
#define SYSMON_TEMP_HYST_MASK		BIT(1)

/* Compute alarm register offset from a channel address */
#define SYSMON_ALARM_OFFSET(addr) \
	(SYSMON_ALARM_REG + ((addr) / SYSMON_ALARM_BITS_PER_REG) * SYSMON_REG_STRIDE)

/*
 * Both RAW and PROCESSED are exposed: RAW is needed for event thresholds
 * (which operate in hardware register format), PROCESSED gives userspace
 * the converted millivolt or millicelsius value.
 */
#define SYSMON_CHAN_TEMP(_chan, _address, _name) {		\
	.type = IIO_TEMP,					\
	.indexed = 1,						\
	.address = _address,					\
	.channel = _chan,					\
	.info_mask_separate =					\
		BIT(IIO_CHAN_INFO_RAW) |				\
		BIT(IIO_CHAN_INFO_PROCESSED),			\
	.scan_type = {						\
		.sign = 's',					\
		.realbits = 15,					\
		.storagebits = 16,				\
		.endianness = IIO_CPU,				\
	},							\
	.datasheet_name = _name,				\
}

#define SYSMON_CHAN_TEMP_EVENT(_chan, _address, _name, _events) {\
	.type = IIO_TEMP,					\
	.indexed = 1,						\
	.address = _address,					\
	.channel = _chan,					\
	.event_spec = _events,					\
	.num_event_specs = ARRAY_SIZE(_events),			\
	.scan_type = {						\
		.sign = 's',					\
		.realbits = 15,					\
		.storagebits = 16,				\
		.endianness = IIO_CPU,				\
	},							\
	.datasheet_name = _name,				\
}

enum sysmon_alarm_bit {
	SYSMON_BIT_ALARM0 = 0,
	SYSMON_BIT_ALARM1 = 1,
	SYSMON_BIT_ALARM2 = 2,
	SYSMON_BIT_ALARM3 = 3,
	SYSMON_BIT_ALARM4 = 4,
	SYSMON_BIT_OT = 8,
	SYSMON_BIT_TEMP = 9,
};

/* Temperature event specification: rising threshold + hysteresis only */
static const struct iio_event_spec sysmon_temp_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE) |
				 BIT(IIO_EV_INFO_VALUE) |
				 BIT(IIO_EV_INFO_HYSTERESIS),
	},
};

/* Supply event specifications */
static const struct iio_event_spec sysmon_supply_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	},
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
};

/* Static temperature channels (always present) */
static const struct iio_chan_spec temp_channels[] = {
	SYSMON_CHAN_TEMP(0, SYSMON_TEMP_MAX, "temp"),
	SYSMON_CHAN_TEMP(1, SYSMON_TEMP_MIN, "min"),
	SYSMON_CHAN_TEMP(2, SYSMON_TEMP_MAX_MAX, "max_max"),
	SYSMON_CHAN_TEMP(3, SYSMON_TEMP_MIN_MIN, "min_min"),
};

/* Temperature event channels (threshold alarms) */
static const struct iio_chan_spec temp_event_channels[] = {
	SYSMON_CHAN_TEMP_EVENT(4, SYSMON_ADDR_TEMP_EVENT, "temp",
			      sysmon_temp_events),
	SYSMON_CHAN_TEMP_EVENT(5, SYSMON_ADDR_OT_EVENT, "ot",
			      sysmon_temp_events),
};

static void sysmon_q8p7_to_millicelsius(s16 raw_data, int *val)
{
	*val = (raw_data * (int)MILLI) >> SYSMON_FRACTIONAL_SHIFT;
}

static void sysmon_millicelsius_to_q8p7(u32 *raw_data, int val)
{
	*raw_data = (val << SYSMON_FRACTIONAL_SHIFT) / (int)MILLI;
}

static void sysmon_supply_rawtoprocessed(int raw_data, int *val)
{
	int mantissa, format, exponent;

	mantissa = FIELD_GET(SYSMON_MANTISSA_MASK, raw_data);
	exponent = SYSMON_SUPPLY_MANTISSA_BITS - FIELD_GET(SYSMON_MODE_MASK, raw_data);
	format = FIELD_GET(SYSMON_FMT_MASK, raw_data);
	/*
	 * When format bit is set the mantissa is two's complement
	 * (per hardware spec); sign-extend to int for correct arithmetic.
	 */
	if (format)
		mantissa = sign_extend32(mantissa, 15);

	*val = (mantissa * (int)MILLI) >> exponent;
}

static void sysmon_supply_processedtoraw(int val, u32 reg_val, u32 *raw_data)
{
	int exponent = FIELD_GET(SYSMON_MODE_MASK, reg_val);
	int format = FIELD_GET(SYSMON_FMT_MASK, reg_val);
	int scale, tmp;

	scale = BIT(SYSMON_SUPPLY_MANTISSA_BITS - exponent);
	tmp = (val * scale) / (int)MILLI;

	if (format)
		tmp = clamp_t(int, tmp, S16_MIN, S16_MAX);
	else
		tmp = clamp_t(int, tmp, 0, U16_MAX);

	*raw_data = (u16)tmp;
}

static int sysmon_temp_thresh_offset(int address,
				     enum iio_event_direction dir)
{
	switch (address) {
	case SYSMON_ADDR_TEMP_EVENT:
		return (dir == IIO_EV_DIR_RISING) ? SYSMON_TEMP_TH_UP :
						    SYSMON_TEMP_TH_LOW;
	case SYSMON_ADDR_OT_EVENT:
		return (dir == IIO_EV_DIR_RISING) ? SYSMON_OT_TH_UP :
						    SYSMON_OT_TH_LOW;
	default:
		return -EINVAL;
	}
}

static int sysmon_supply_thresh_offset(int address,
				       enum iio_event_direction dir)
{
	if (dir == IIO_EV_DIR_RISING)
		return (address * SYSMON_REG_STRIDE) + SYSMON_SUPPLY_TH_UP;
	if (dir == IIO_EV_DIR_FALLING)
		return (address * SYSMON_REG_STRIDE) + SYSMON_SUPPLY_TH_LOW;

	return -EINVAL;
}

static int sysmon_read_raw(struct iio_dev *indio_dev,
			   struct iio_chan_spec const *chan,
			   int *val, int *val2, long mask)
{
	struct sysmon *sysmon = iio_priv(indio_dev);
	unsigned int regval;
	int ret;

	if (mask != IIO_CHAN_INFO_RAW && mask != IIO_CHAN_INFO_PROCESSED)
		return -EINVAL;

	guard(mutex)(&sysmon->lock);

	switch (chan->type) {
	case IIO_TEMP:
		ret = regmap_read(sysmon->regmap, chan->address, &regval);
		if (ret)
			return ret;
		if (mask == IIO_CHAN_INFO_PROCESSED)
			sysmon_q8p7_to_millicelsius(regval, val);
		else
			*val = regval;
		return IIO_VAL_INT;

	case IIO_VOLTAGE:
		ret = regmap_read(sysmon->regmap,
				  (chan->address * SYSMON_REG_STRIDE) +
				  SYSMON_SUPPLY_BASE, &regval);
		if (ret)
			return ret;
		if (mask == IIO_CHAN_INFO_PROCESSED)
			sysmon_supply_rawtoprocessed(regval, val);
		else
			*val = regval;
		return IIO_VAL_INT;

	default:
		return -EINVAL;
	}
}

static int sysmon_get_event_mask(unsigned long address)
{
	if (address == SYSMON_ADDR_TEMP_EVENT)
		return BIT(SYSMON_BIT_TEMP);
	if (address == SYSMON_ADDR_OT_EVENT)
		return BIT(SYSMON_BIT_OT);

	return BIT(address / SYSMON_ALARM_BITS_PER_REG);
}

static int sysmon_read_alarm_config(struct sysmon *sysmon,
				    unsigned long address)
{
	u32 shift = address % SYSMON_ALARM_BITS_PER_REG;
	u32 offset = SYSMON_ALARM_OFFSET(address);
	unsigned int reg_val;
	int ret;

	ret = regmap_read(sysmon->regmap, offset, &reg_val);
	if (ret)
		return ret;

	return !!(reg_val & BIT(shift));
}

static int sysmon_write_alarm_config(struct sysmon *sysmon,
				     unsigned long address, bool enable)
{
	u32 shift = address % SYSMON_ALARM_BITS_PER_REG;
	u32 offset = SYSMON_ALARM_OFFSET(address);

	if (enable)
		return regmap_set_bits(sysmon->regmap, offset, BIT(shift));

	return regmap_clear_bits(sysmon->regmap, offset, BIT(shift));
}

static int sysmon_read_event_config(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir)
{
	u32 alarm_event_mask = sysmon_get_event_mask(chan->address);
	struct sysmon *sysmon = iio_priv(indio_dev);
	unsigned int imr;
	int config_value;
	int ret;

	ret = regmap_read(sysmon->regmap, SYSMON_IMR, &imr);
	if (ret)
		return ret;

	/* IMR bits are 1=masked, invert to get 1=enabled */
	imr = ~imr;

	if (chan->type == IIO_VOLTAGE) {
		config_value = sysmon_read_alarm_config(sysmon, chan->address);
		if (config_value < 0)
			return config_value;
		return config_value && (imr & alarm_event_mask);
	}

	return !!(imr & alarm_event_mask);
}

static int sysmon_write_event_config(struct iio_dev *indio_dev,
				     const struct iio_chan_spec *chan,
				     enum iio_event_type type,
				     enum iio_event_direction dir,
				     bool state)
{
	u32 offset = SYSMON_ALARM_OFFSET(chan->address);
	u32 ier = sysmon_get_event_mask(chan->address);
	struct sysmon *sysmon = iio_priv(indio_dev);
	unsigned int alarm_config;
	int ret;

	guard(mutex)(&sysmon->lock);

	if (chan->type == IIO_VOLTAGE) {
		ret = sysmon_write_alarm_config(sysmon, chan->address, state);
		if (ret)
			return ret;

		ret = regmap_read(sysmon->regmap, offset, &alarm_config);
		if (ret)
			return ret;

		if (alarm_config)
			return regmap_write(sysmon->regmap, SYSMON_IER, ier);

		return regmap_write(sysmon->regmap, SYSMON_IDR, ier);
	}

	if (chan->type == IIO_TEMP) {
		if (state) {
			ret = regmap_write(sysmon->regmap, SYSMON_IER, ier);
			if (ret)
				return ret;
			sysmon->temp_mask &= ~ier;
		} else {
			ret = regmap_write(sysmon->regmap, SYSMON_IDR, ier);
			if (ret)
				return ret;
			sysmon->temp_mask |= ier;
		}
	}

	return 0;
}

/*
 * Recompute the lower threshold register from upper threshold and
 * cached hysteresis. Called when either upper threshold or hysteresis
 * is written.
 */
static int sysmon_update_temp_lower(struct sysmon *sysmon, int address)
{
	unsigned int upper_reg;
	int upper_mc, lower_mc, hysteresis;
	u32 raw_val;
	int upper_off, lower_off, ret;

	upper_off = sysmon_temp_thresh_offset(address, IIO_EV_DIR_RISING);
	if (upper_off < 0)
		return upper_off;
	lower_off = sysmon_temp_thresh_offset(address, IIO_EV_DIR_FALLING);
	if (lower_off < 0)
		return lower_off;

	if (address == SYSMON_ADDR_OT_EVENT)
		hysteresis = sysmon->ot_hysteresis;
	else
		hysteresis = sysmon->temp_hysteresis;

	ret = regmap_read(sysmon->regmap, upper_off, &upper_reg);
	if (ret)
		return ret;
	sysmon_q8p7_to_millicelsius(upper_reg, &upper_mc);

	lower_mc = upper_mc - hysteresis;
	sysmon_millicelsius_to_q8p7(&raw_val, lower_mc);

	return regmap_write(sysmon->regmap, lower_off, raw_val);
}

static int sysmon_read_event_value(struct iio_dev *indio_dev,
				   const struct iio_chan_spec *chan,
				   enum iio_event_type type,
				   enum iio_event_direction dir,
				   enum iio_event_info info,
				   int *val, int *val2)
{
	struct sysmon *sysmon = iio_priv(indio_dev);
	unsigned int reg_val;
	int offset;
	int ret;

	guard(mutex)(&sysmon->lock);

	if (chan->type == IIO_TEMP) {
		if (info == IIO_EV_INFO_VALUE) {
			/* Only rising threshold is exposed */
			offset = sysmon_temp_thresh_offset(chan->address,
							   IIO_EV_DIR_RISING);
			if (offset < 0)
				return offset;
			ret = regmap_read(sysmon->regmap, offset, &reg_val);
			if (ret)
				return ret;
			sysmon_q8p7_to_millicelsius(reg_val, val);
			return IIO_VAL_INT;
		}
		if (info == IIO_EV_INFO_HYSTERESIS) {
			if (chan->address == SYSMON_ADDR_OT_EVENT)
				*val = sysmon->ot_hysteresis;
			else
				*val = sysmon->temp_hysteresis;
			return IIO_VAL_INT;
		}
	}

	if (chan->type == IIO_VOLTAGE) {
		offset = sysmon_supply_thresh_offset(chan->address, dir);
		if (offset < 0)
			return offset;
		ret = regmap_read(sysmon->regmap, offset, &reg_val);
		if (ret)
			return ret;
		sysmon_supply_rawtoprocessed(reg_val, val);
		return IIO_VAL_INT;
	}

	return -EINVAL;
}

static int sysmon_write_event_value(struct iio_dev *indio_dev,
				    const struct iio_chan_spec *chan,
				    enum iio_event_type type,
				    enum iio_event_direction dir,
				    enum iio_event_info info,
				    int val, int val2)
{
	struct sysmon *sysmon = iio_priv(indio_dev);
	unsigned int reg_val;
	u32 raw_val;
	int offset;
	int ret;

	guard(mutex)(&sysmon->lock);

	if (chan->type == IIO_TEMP) {
		if (info == IIO_EV_INFO_VALUE) {
			/* Only rising threshold is exposed */
			offset = sysmon_temp_thresh_offset(chan->address,
							   IIO_EV_DIR_RISING);
			if (offset < 0)
				return offset;
			sysmon_millicelsius_to_q8p7(&raw_val, val);
			ret = regmap_write(sysmon->regmap, offset, raw_val);
			if (ret)
				return ret;
			/* Recompute lower = upper - hysteresis */
			return sysmon_update_temp_lower(sysmon,
							chan->address);
		}
		if (info == IIO_EV_INFO_HYSTERESIS) {
			if (val < 0)
				return -EINVAL;
			if (chan->address == SYSMON_ADDR_OT_EVENT)
				sysmon->ot_hysteresis = val;
			else
				sysmon->temp_hysteresis = val;
			return sysmon_update_temp_lower(sysmon,
							chan->address);
		}
	}

	if (chan->type == IIO_VOLTAGE) {
		offset = sysmon_supply_thresh_offset(chan->address, dir);
		if (offset < 0)
			return offset;
		ret = regmap_read(sysmon->regmap, offset, &reg_val);
		if (ret)
			return ret;
		sysmon_supply_processedtoraw(val, reg_val, &raw_val);
		return regmap_write(sysmon->regmap, offset, raw_val);
	}

	return -EINVAL;
}

static int sysmon_read_label(struct iio_dev *indio_dev,
			     struct iio_chan_spec const *chan,
			     char *label)
{
	if (chan->datasheet_name)
		return sysfs_emit(label, "%s\n", chan->datasheet_name);

	return -EINVAL;
}

static const struct iio_info sysmon_iio_info = {
	.read_raw = sysmon_read_raw,
	.read_label = sysmon_read_label,
	.read_event_config = sysmon_read_event_config,
	.write_event_config = sysmon_write_event_config,
	.read_event_value = sysmon_read_event_value,
	.write_event_value = sysmon_write_event_value,
};

static void sysmon_push_event(struct iio_dev *indio_dev, u32 address)
{
	const struct iio_chan_spec *chan;
	enum iio_event_direction dir;

	for (unsigned int i = 0; i < indio_dev->num_channels; i++) {
		if (indio_dev->channels[i].address != address)
			continue;

		chan = &indio_dev->channels[i];
		/* Temp uses hysteresis mode (rising only), voltage uses window */
		dir = (chan->type == IIO_TEMP) ? IIO_EV_DIR_RISING :
						 IIO_EV_DIR_EITHER;
		iio_push_event(indio_dev,
			       IIO_UNMOD_EVENT_CODE(chan->type,
						    chan->channel,
						    IIO_EV_TYPE_THRESH,
						    dir),
			       iio_get_time_ns(indio_dev));
	}
}

static int sysmon_handle_event(struct iio_dev *indio_dev, u32 event)
{
	u32 alarm_flag_offset = SYSMON_ALARM_FLAG + (event * SYSMON_REG_STRIDE);
	u32 alarm_reg_offset = SYSMON_ALARM_REG + (event * SYSMON_REG_STRIDE);
	struct sysmon *sysmon = iio_priv(indio_dev);
	unsigned long alarm_flag_reg;
	unsigned int reg_val;
	u32 address, bit;
	int ret;

	switch (event) {
	case SYSMON_BIT_TEMP:
		sysmon_push_event(indio_dev, SYSMON_ADDR_TEMP_EVENT);
		ret = regmap_write(sysmon->regmap, SYSMON_IDR,
				   BIT(SYSMON_BIT_TEMP));
		if (ret)
			return ret;
		sysmon->masked_temp |= BIT(SYSMON_BIT_TEMP);
		break;

	case SYSMON_BIT_OT:
		sysmon_push_event(indio_dev, SYSMON_ADDR_OT_EVENT);
		ret = regmap_write(sysmon->regmap, SYSMON_IDR,
				   BIT(SYSMON_BIT_OT));
		if (ret)
			return ret;
		sysmon->masked_temp |= BIT(SYSMON_BIT_OT);
		break;

	case SYSMON_BIT_ALARM0:
	case SYSMON_BIT_ALARM1:
	case SYSMON_BIT_ALARM2:
	case SYSMON_BIT_ALARM3:
	case SYSMON_BIT_ALARM4:
		ret = regmap_read(sysmon->regmap, alarm_flag_offset, &reg_val);
		if (ret)
			return ret;
		alarm_flag_reg = reg_val;

		for_each_set_bit(bit, &alarm_flag_reg,
				 SYSMON_ALARM_BITS_PER_REG) {
			address = bit + (SYSMON_ALARM_BITS_PER_REG * event);
			sysmon_push_event(indio_dev, address);
			ret = regmap_update_bits(sysmon->regmap,
						 alarm_reg_offset,
						 BIT(bit), 0);
			if (ret)
				return ret;
		}
		ret = regmap_write(sysmon->regmap, alarm_flag_offset,
				   alarm_flag_reg);
		if (ret)
			return ret;
		break;

	default:
		break;
	}

	return 0;
}

static void sysmon_handle_events(struct iio_dev *indio_dev,
				 unsigned long events)
{
	unsigned int bit;

	for_each_set_bit(bit, &events, SYSMON_NO_OF_EVENTS)
		sysmon_handle_event(indio_dev, bit);
}

static void sysmon_unmask_temp(struct sysmon *sysmon, unsigned int isr)
{
	unsigned int unmask, status;

	status = isr & SYSMON_TEMP_INTR_MASK;

	unmask = (sysmon->masked_temp ^ status) & sysmon->masked_temp;
	sysmon->masked_temp &= status;

	unmask &= ~sysmon->temp_mask;

	regmap_write(sysmon->regmap, SYSMON_IER, unmask);
}

/*
 * Versal threshold interrupts are level-sensitive. Active threshold
 * interrupts are masked in the handler and polled via delayed work
 * until the condition clears, then unmasked.
 */
static void sysmon_unmask_worker(struct work_struct *work)
{
	struct sysmon *sysmon = container_of(work, struct sysmon,
					     sysmon_unmask_work.work);
	unsigned int isr;

	spin_lock_irq(&sysmon->irq_lock);
	regmap_read(sysmon->regmap, SYSMON_ISR, &isr);
	regmap_write(sysmon->regmap, SYSMON_ISR, isr);
	sysmon_unmask_temp(sysmon, isr);
	spin_unlock_irq(&sysmon->irq_lock);

	if (sysmon->masked_temp)
		schedule_delayed_work(&sysmon->sysmon_unmask_work,
				      msecs_to_jiffies(SYSMON_UNMASK_WORK_DELAY_MS));
	else
		regmap_write(sysmon->regmap, SYSMON_STATUS_RESET, 1);
}

static irqreturn_t sysmon_iio_irq(int irq, void *data)
{
	struct iio_dev *indio_dev = data;
	struct sysmon *sysmon;
	unsigned int isr, imr;

	sysmon = iio_priv(indio_dev);
	spin_lock(&sysmon->irq_lock);

	regmap_read(sysmon->regmap, SYSMON_ISR, &isr);
	regmap_read(sysmon->regmap, SYSMON_IMR, &imr);

	isr &= ~imr;
	regmap_write(sysmon->regmap, SYSMON_ISR, isr);

	if (isr) {
		sysmon_handle_events(indio_dev, isr);
		schedule_delayed_work(&sysmon->sysmon_unmask_work,
				      msecs_to_jiffies(SYSMON_UNMASK_WORK_DELAY_MS));
	}

	spin_unlock(&sysmon->irq_lock);

	return IRQ_RETVAL(isr);
}

static int sysmon_init_interrupt(struct sysmon *sysmon,
				 struct device *dev,
				 struct iio_dev *indio_dev,
				 int irq)
{
	unsigned int imr;
	int ret;

	/* Events not supported without IRQ (e.g. I2C path) */
	if (!irq)
		return 0;

	ret = devm_delayed_work_autocancel(dev, &sysmon->sysmon_unmask_work,
					   sysmon_unmask_worker);
	if (ret)
		return ret;

	ret = regmap_read(sysmon->regmap, SYSMON_IMR, &imr);
	if (ret)
		return ret;
	sysmon->temp_mask = imr & SYSMON_TEMP_INTR_MASK;

	return devm_request_irq(dev, irq, sysmon_iio_irq, 0,
				"sysmon-irq", indio_dev);
}

/*
 * Initialize the cached hysteresis for a temperature channel from the
 * current hardware threshold registers: hysteresis = upper - lower.
 */
static int sysmon_init_hysteresis(struct sysmon *sysmon, int address,
				  int *hysteresis)
{
	unsigned int upper_reg, lower_reg;
	int upper_mc, lower_mc;
	int upper_off, lower_off;
	int ret;

	upper_off = sysmon_temp_thresh_offset(address, IIO_EV_DIR_RISING);
	if (upper_off < 0)
		return upper_off;
	lower_off = sysmon_temp_thresh_offset(address, IIO_EV_DIR_FALLING);
	if (lower_off < 0)
		return lower_off;

	ret = regmap_read(sysmon->regmap, upper_off, &upper_reg);
	if (ret)
		return ret;
	ret = regmap_read(sysmon->regmap, lower_off, &lower_reg);
	if (ret)
		return ret;

	sysmon_q8p7_to_millicelsius(upper_reg, &upper_mc);
	sysmon_q8p7_to_millicelsius(lower_reg, &lower_mc);
	*hysteresis = upper_mc - lower_mc;

	return 0;
}

/**
 * sysmon_parse_fw() - Parse firmware nodes and configure IIO channels.
 * @indio_dev: IIO device instance
 * @dev: Parent device
 * @has_irq: true if an IRQ is available (enables event channels)
 *
 * Reads voltage-channels and temperature-channels container nodes from
 * firmware and builds the IIO channel array. Static temperature channels
 * and event channels are prepended, followed by supply and satellite
 * channels from DT.
 *
 * Event channels and per-channel event specs are only added when the
 * device has an IRQ. I2C devices have no interrupt line, and the I2C
 * regmap cannot be called from atomic context, so events are not
 * supported on that path.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int sysmon_parse_fw(struct iio_dev *indio_dev, struct device *dev,
			   bool has_irq)
{
	struct fwnode_handle *supply_node __free(fwnode_handle) =
		device_get_named_child_node(dev, "voltage-channels");
	struct fwnode_handle *temp_node __free(fwnode_handle) =
		device_get_named_child_node(dev, "temperature-channels");
	unsigned int num_supply = 0, num_temp = 0;
	unsigned int idx, temp_chan_idx, volt_chan_idx;
	struct iio_chan_spec *sysmon_channels;
	unsigned int num_events;
	const char *label;
	u32 reg;
	int ret;

	if (supply_node)
		num_supply = fwnode_get_child_node_count(supply_node);
	if (temp_node)
		num_temp = fwnode_get_child_node_count(temp_node);

	num_events = has_irq ? ARRAY_SIZE(temp_event_channels) : 0;

	sysmon_channels = devm_kcalloc(dev,
				       size_add(ARRAY_SIZE(temp_channels) +
						num_events,
						num_supply + num_temp),
				       sizeof(*sysmon_channels), GFP_KERNEL);
	if (!sysmon_channels)
		return -ENOMEM;

	/* Static temperature channels first (fixed indices) */
	idx = 0;
	memcpy(sysmon_channels, temp_channels, sizeof(temp_channels));
	idx += ARRAY_SIZE(temp_channels);

	/* Temperature event channels (only when IRQ is available) */
	if (has_irq) {
		memcpy(sysmon_channels + idx, temp_event_channels,
		       sizeof(temp_event_channels));
		idx += ARRAY_SIZE(temp_event_channels);
	}

	/* Supply channels from DT */
	fwnode_for_each_child_node_scoped(supply_node, child) {
		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "missing reg for supply channel\n");

		if (reg > SYSMON_SUPPLY_IDX_MAX)
			return dev_err_probe(dev, -EINVAL,
					     "supply reg %u exceeds max %u\n",
					     reg, SYSMON_SUPPLY_IDX_MAX);

		ret = fwnode_property_read_string(child, "label", &label);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "missing label for supply channel\n");

		sysmon_channels[idx++] = (struct iio_chan_spec) {
			.type = IIO_VOLTAGE,
			.indexed = 1,
			.address = reg,
			.info_mask_separate =
				BIT(IIO_CHAN_INFO_RAW) |
				BIT(IIO_CHAN_INFO_PROCESSED),
			.event_spec = has_irq ?
				sysmon_supply_events : NULL,
			.num_event_specs = has_irq ?
				ARRAY_SIZE(sysmon_supply_events) : 0,
			.scan_type = {
				.realbits = 19,
				.storagebits = 32,
				.endianness = IIO_CPU,
				.sign = fwnode_property_read_bool(child,
					"bipolar") ? 's' : 'u',
			},
			.datasheet_name = label,
		};
	}

	/* Temperature satellite channels from DT */
	fwnode_for_each_child_node_scoped(temp_node, child) {
		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "missing reg for temp channel\n");

		if (reg < 1 || reg > SYSMON_TEMP_SAT_MAX)
			return dev_err_probe(dev, -EINVAL,
					     "temp reg %u out of range [1..%u]\n",
					     reg, SYSMON_TEMP_SAT_MAX);

		ret = fwnode_property_read_string(child, "label", &label);
		if (ret < 0)
			return dev_err_probe(dev, ret,
					     "missing label for temp channel\n");

		sysmon_channels[idx++] = (struct iio_chan_spec) {
			.type = IIO_TEMP,
			.indexed = 1,
			.address = SYSMON_TEMP_SAT_BASE +
				   ((reg - 1) * SYSMON_REG_STRIDE),
			.info_mask_separate =
				BIT(IIO_CHAN_INFO_RAW) |
				BIT(IIO_CHAN_INFO_PROCESSED),
			.scan_type = {
				.sign = 's',
				.realbits = 15,
				.storagebits = 16,
				.endianness = IIO_CPU,
			},
			.datasheet_name = label,
		};
	}

	indio_dev->num_channels = idx;
	indio_dev->info = &sysmon_iio_info;

	/*
	 * Assign per-type sequential channel numbers.
	 * IIO sysfs uses type prefix (in_tempN, in_voltageN)
	 * so numbers only need to be unique within each type.
	 */
	temp_chan_idx = 0;
	volt_chan_idx = 0;
	for (idx = 0; idx < indio_dev->num_channels; idx++) {
		if (sysmon_channels[idx].type == IIO_TEMP)
			sysmon_channels[idx].channel = temp_chan_idx++;
		else
			sysmon_channels[idx].channel = volt_chan_idx++;
	}

	indio_dev->channels = sysmon_channels;

	return 0;
}

/**
 * sysmon_core_probe() - Initialize Versal SysMon core
 * @dev: Parent device
 * @regmap: Register map for hardware access
 *
 * Return: 0 on success, negative errno on failure.
 */
int sysmon_core_probe(struct device *dev, struct regmap *regmap)
{
	struct iio_dev *indio_dev;
	struct sysmon *sysmon;
	bool has_irq;
	int irq;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*sysmon));
	if (!indio_dev)
		return -ENOMEM;

	sysmon = iio_priv(indio_dev);
	sysmon->regmap = regmap;

	ret = devm_mutex_init(dev, &sysmon->lock);
	if (ret)
		return ret;
	spin_lock_init(&sysmon->irq_lock);

	/* Disable all interrupts and clear pending status */
	ret = regmap_write(sysmon->regmap, SYSMON_IDR, SYSMON_INTR_ALL_MASK);
	if (ret)
		return ret;
	ret = regmap_write(sysmon->regmap, SYSMON_ISR, SYSMON_INTR_ALL_MASK);
	if (ret)
		return ret;

	irq = fwnode_irq_get(dev_fwnode(dev), 0);
	has_irq = irq > 0;

	indio_dev->name = "versal-sysmon";
	indio_dev->modes = INDIO_DIRECT_MODE;

	ret = sysmon_parse_fw(indio_dev, dev, has_irq);
	if (ret)
		return ret;

	if (has_irq) {
		/* Set hysteresis mode for both temperature channels */
		ret = regmap_set_bits(sysmon->regmap, SYSMON_TEMP_EV_CFG,
				      SYSMON_OT_HYST_MASK |
				      SYSMON_TEMP_HYST_MASK);
		if (ret)
			return ret;

		/* Initialize cached hysteresis from hardware registers */
		ret = sysmon_init_hysteresis(sysmon, SYSMON_ADDR_TEMP_EVENT,
					     &sysmon->temp_hysteresis);
		if (ret)
			return ret;
		ret = sysmon_init_hysteresis(sysmon, SYSMON_ADDR_OT_EVENT,
					     &sysmon->ot_hysteresis);
		if (ret)
			return ret;

		ret = sysmon_init_interrupt(sysmon, dev, indio_dev, irq);
		if (ret)
			return ret;
	}

	return devm_iio_device_register(dev, indio_dev);
}
EXPORT_SYMBOL_GPL(sysmon_core_probe);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("AMD Versal SysMon Core Driver");
MODULE_AUTHOR("Salih Erim <salih.erim@amd.com>");
