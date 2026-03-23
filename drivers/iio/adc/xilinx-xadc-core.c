// SPDX-License-Identifier: GPL-2.0-only
/*
 * Xilinx XADC driver
 *
 * Copyright 2013-2014 Analog Devices Inc.
 *  Author: Lars-Peter Clausen <lars@metafoo.de>
 *
 * Documentation for the parts can be found at:
 *  - XADC hardmacro: Xilinx UG480
 *  - ZYNQ XADC interface: Xilinx UG585
 *  - AXI XADC interface: Xilinx PG019
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/slab.h>

#include <linux/iio/buffer.h>
#include <linux/iio/events.h>
#include <linux/iio/iio.h>
#include <linux/iio/trigger.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#include "xilinx-xadc.h"

void xadc_write_reg(struct xadc *xadc, unsigned int reg, uint32_t val)
{
	writel(val, xadc->base + reg);
}
EXPORT_SYMBOL_GPL(xadc_write_reg);

void xadc_read_reg(struct xadc *xadc, unsigned int reg, uint32_t *val)
{
	*val = readl(xadc->base + reg);
}
EXPORT_SYMBOL_GPL(xadc_read_reg);

static int _xadc_update_adc_reg(struct xadc *xadc, unsigned int reg, u16 mask, u16 val)
{
	uint16_t tmp;
	int ret;

	ret = _xadc_read_adc_reg(xadc, reg, &tmp);
	if (ret)
		return ret;

	return _xadc_write_adc_reg(xadc, reg, (tmp & ~mask) | val);
}

static int xadc_update_adc_reg(struct xadc *xadc, unsigned int reg, u16 mask, u16 val)
{
	int ret;

	mutex_lock(&xadc->mutex);
	ret = _xadc_update_adc_reg(xadc, reg, mask, val);
	mutex_unlock(&xadc->mutex);

	return ret;
}

static unsigned long xadc_get_dclk_rate(struct xadc *xadc)
{
	return xadc->ops->get_dclk_rate(xadc);
}

static int xadc_update_scan_mode(struct iio_dev *indio_dev, const unsigned long *mask)
{
	struct xadc *xadc = iio_priv(indio_dev);
	void *data;
	size_t n;

	n = bitmap_weight(mask, iio_get_masklength(indio_dev));

	data = devm_krealloc_array(indio_dev->dev.parent, xadc->data,
				   n, sizeof(*xadc->data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	memset(data, 0, n * sizeof(*xadc->data));
	xadc->data = data;

	return 0;
}

static unsigned int xadc_scan_index_to_channel(unsigned int scan_index)
{
	switch (scan_index) {
	case 5:
		return XADC_REG_VCCPINT;
	case 6:
		return XADC_REG_VCCPAUX;
	case 7:
		return XADC_REG_VCCO_DDR;
	case 8:
		return XADC_REG_TEMP;
	case 9:
		return XADC_REG_VCCINT;
	case 10:
		return XADC_REG_VCCAUX;
	case 11:
		return XADC_REG_VPVN;
	case 12:
		return XADC_REG_VREFP;
	case 13:
		return XADC_REG_VREFN;
	case 14:
		return XADC_REG_VCCBRAM;
	default:
		return XADC_REG_VAUX(scan_index - 16);
	}
}

static irqreturn_t xadc_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct xadc *xadc = iio_priv(indio_dev);
	unsigned int chan;
	int i, j;

	if (!xadc->data)
		goto out;

	j = 0;
	iio_for_each_active_channel(indio_dev, i) {
		chan = xadc_scan_index_to_channel(i);
		xadc_read_adc_reg(xadc, chan, &xadc->data[j]);
		j++;
	}

	iio_push_to_buffers(indio_dev, xadc->data);

out:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int xadc_trigger_set_state(struct iio_trigger *trigger, bool state)
{
	struct xadc *xadc = iio_trigger_get_drvdata(trigger);
	unsigned int convst, val;
	unsigned long flags;
	int ret = 0;

	mutex_lock(&xadc->mutex);

	if (state) {
		/* Only one of the two triggers can be active at a time. */
		if (xadc->trigger != NULL) {
			ret = -EBUSY;
			goto err_out;
		} else {
			xadc->trigger = trigger;
			if (trigger == xadc->convst_trigger)
				convst = XADC_CONF0_EC;
			else
				convst = 0;
		}
		ret = _xadc_update_adc_reg(xadc, XADC_REG_CONF1, XADC_CONF0_EC,
					   convst);
		if (ret)
			goto err_out;
	} else {
		xadc->trigger = NULL;
	}

	spin_lock_irqsave(&xadc->lock, flags);
	xadc_read_reg(xadc, XADC_AXI_REG_IPIER, &val);
	xadc_write_reg(xadc, XADC_AXI_REG_IPISR, XADC_AXI_INT_EOS);
	if (state)
		val |= XADC_AXI_INT_EOS;
	else
		val &= ~XADC_AXI_INT_EOS;
	xadc_write_reg(xadc, XADC_AXI_REG_IPIER, val);
	spin_unlock_irqrestore(&xadc->lock, flags);

err_out:
	mutex_unlock(&xadc->mutex);

	return ret;
}

static const struct iio_trigger_ops xadc_trigger_ops = {
	.set_trigger_state = &xadc_trigger_set_state,
};

static struct iio_trigger *xadc_alloc_trigger(struct iio_dev *indio_dev, const char *name)
{
	struct device *dev = indio_dev->dev.parent;
	struct iio_trigger *trig;
	int ret;

	trig = devm_iio_trigger_alloc(dev, "%s%d-%s", indio_dev->name,
				      iio_device_id(indio_dev), name);
	if (trig == NULL)
		return ERR_PTR(-ENOMEM);

	trig->ops = &xadc_trigger_ops;
	iio_trigger_set_drvdata(trig, iio_priv(indio_dev));

	ret = devm_iio_trigger_register(dev, trig);
	if (ret)
		return ERR_PTR(ret);

	return trig;
}

static int xadc_power_adc_b(struct xadc *xadc, unsigned int seq_mode)
{
	uint16_t val;

	/*
	 * As per datasheet the power-down bits are don't care in the
	 * UltraScale, but as per reality setting the power-down bit for the
	 * non-existing ADC-B powers down the main ADC, so just return and don't
	 * do anything.
	 */
	if (xadc->ops->type == XADC_TYPE_US)
		return 0;

	/* Powerdown the ADC-B when it is not needed. */
	switch (seq_mode) {
	case XADC_CONF1_SEQ_SIMULTANEOUS:
	case XADC_CONF1_SEQ_INDEPENDENT:
		val = 0;
		break;
	default:
		val = XADC_CONF2_PD_ADC_B;
		break;
	}

	return xadc_update_adc_reg(xadc, XADC_REG_CONF2, XADC_CONF2_PD_MASK,
		val);
}

static int xadc_get_seq_mode(struct xadc *xadc, unsigned long scan_mode)
{
	unsigned int aux_scan_mode = scan_mode >> 16;

	/* UltraScale has only one ADC and supports only continuous mode */
	if (xadc->ops->type == XADC_TYPE_US)
		return XADC_CONF1_SEQ_CONTINUOUS;

	if (xadc->external_mux_mode == XADC_EXTERNAL_MUX_DUAL)
		return XADC_CONF1_SEQ_SIMULTANEOUS;

	if ((aux_scan_mode & 0xff00) == 0 ||
		(aux_scan_mode & 0x00ff) == 0)
		return XADC_CONF1_SEQ_CONTINUOUS;

	return XADC_CONF1_SEQ_SIMULTANEOUS;
}

int xadc_postdisable(struct iio_dev *indio_dev)
{
	struct xadc *xadc = iio_priv(indio_dev);
	unsigned long scan_mask;
	int ret;
	u32 i;

	scan_mask = 1; /* Run calibration as part of the sequence */
	for (i = 0; i < indio_dev->num_channels; i++)
		scan_mask |= BIT(indio_dev->channels[i].scan_index);

	/* Enable all channels and calibration */
	ret = xadc_write_adc_reg(xadc, XADC_REG_SEQ(0), scan_mask & 0xffff);
	if (ret)
		return ret;

	ret = xadc_write_adc_reg(xadc, XADC_REG_SEQ(1), scan_mask >> 16);
	if (ret)
		return ret;

	ret = xadc_update_adc_reg(xadc, XADC_REG_CONF1, XADC_CONF1_SEQ_MASK,
		XADC_CONF1_SEQ_CONTINUOUS);
	if (ret)
		return ret;

	return xadc_power_adc_b(xadc, XADC_CONF1_SEQ_CONTINUOUS);
}
EXPORT_SYMBOL_GPL(xadc_postdisable);

static int xadc_preenable(struct iio_dev *indio_dev)
{
	struct xadc *xadc = iio_priv(indio_dev);
	unsigned long scan_mask;
	int seq_mode;
	int ret;

	ret = xadc_update_adc_reg(xadc, XADC_REG_CONF1, XADC_CONF1_SEQ_MASK,
		XADC_CONF1_SEQ_DEFAULT);
	if (ret)
		goto err;

	scan_mask = *indio_dev->active_scan_mask;
	seq_mode = xadc_get_seq_mode(xadc, scan_mask);

	ret = xadc_write_adc_reg(xadc, XADC_REG_SEQ(0), scan_mask & 0xffff);
	if (ret)
		goto err;

	/*
	 * In simultaneous mode the upper and lower aux channels are samples at
	 * the same time. In this mode the upper 8 bits in the sequencer
	 * register are don't care and the lower 8 bits control two channels
	 * each. As such we must set the bit if either the channel in the lower
	 * group or the upper group is enabled.
	 */
	if (seq_mode == XADC_CONF1_SEQ_SIMULTANEOUS)
		scan_mask = ((scan_mask >> 8) | scan_mask) & 0xff0000;

	ret = xadc_write_adc_reg(xadc, XADC_REG_SEQ(1), scan_mask >> 16);
	if (ret)
		goto err;

	ret = xadc_power_adc_b(xadc, seq_mode);
	if (ret)
		goto err;

	ret = xadc_update_adc_reg(xadc, XADC_REG_CONF1, XADC_CONF1_SEQ_MASK,
		seq_mode);
	if (ret)
		goto err;

	return 0;
err:
	xadc_postdisable(indio_dev);
	return ret;
}

static const struct iio_buffer_setup_ops xadc_buffer_ops = {
	.preenable = &xadc_preenable,
	.postdisable = &xadc_postdisable,
};

int xadc_read_samplerate(struct xadc *xadc)
{
	unsigned int div;
	uint16_t val16;
	int ret;

	ret = xadc_read_adc_reg(xadc, XADC_REG_CONF2, &val16);
	if (ret)
		return ret;

	div = (val16 & XADC_CONF2_DIV_MASK) >> XADC_CONF2_DIV_OFFSET;
	if (div < 2)
		div = 2;

	return xadc_get_dclk_rate(xadc) / div / 26;
}
EXPORT_SYMBOL_GPL(xadc_read_samplerate);

static int xadc_read_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
			 int *val, int *val2, long info)
{
	struct xadc *xadc = iio_priv(indio_dev);
	unsigned int bits = chan->scan_type.realbits;
	uint16_t val16;
	int ret;

	switch (info) {
	case IIO_CHAN_INFO_RAW:
		if (iio_buffer_enabled(indio_dev))
			return -EBUSY;
		ret = xadc_read_adc_reg(xadc, chan->address, &val16);
		if (ret < 0)
			return ret;

		val16 >>= chan->scan_type.shift;
		if (chan->scan_type.sign == 'u')
			*val = val16;
		else
			*val = sign_extend32(val16, bits - 1);

		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		switch (chan->type) {
		case IIO_VOLTAGE:
			/* V = (val * 3.0) / 2**bits */
			switch (chan->address) {
			case XADC_REG_VCCINT:
			case XADC_REG_VCCAUX:
			case XADC_REG_VREFP:
			case XADC_REG_VREFN:
			case XADC_REG_VCCBRAM:
			case XADC_REG_VCCPINT:
			case XADC_REG_VCCPAUX:
			case XADC_REG_VCCO_DDR:
				*val = 3000;
				break;
			default:
				*val = 1000;
				break;
			}
			*val2 = bits;
			return IIO_VAL_FRACTIONAL_LOG2;
		case IIO_TEMP:
			*val = xadc->ops->temp_scale;
			*val2 = bits;
			return IIO_VAL_FRACTIONAL_LOG2;
		default:
			return -EINVAL;
		}
	case IIO_CHAN_INFO_OFFSET:
		/* Only the temperature channel has an offset */
		*val = -((xadc->ops->temp_offset << bits) / xadc->ops->temp_scale);
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SAMP_FREQ:
		ret = xadc_read_samplerate(xadc);
		if (ret < 0)
			return ret;

		*val = ret;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

int xadc_setup_buffer_and_triggers(struct device *dev, struct iio_dev *indio_dev,
				   struct xadc *xadc, int irq)
{
	int ret;

	if (!(xadc->ops->flags & XADC_FLAGS_BUFFERED))
		return 0;

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      &iio_pollfunc_store_time,
					      &xadc_trigger_handler,
					      &xadc_buffer_ops);
	if (ret)
		return ret;

	if (irq > 0) {
		xadc->convst_trigger = xadc_alloc_trigger(indio_dev, "convst");
		if (IS_ERR(xadc->convst_trigger))
			return PTR_ERR(xadc->convst_trigger);

		xadc->samplerate_trigger = xadc_alloc_trigger(indio_dev,
							      "samplerate");
		if (IS_ERR(xadc->samplerate_trigger))
			return PTR_ERR(xadc->samplerate_trigger);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(xadc_setup_buffer_and_triggers);

int xadc_write_samplerate(struct xadc *xadc, int val)
{
	unsigned long clk_rate = xadc_get_dclk_rate(xadc);
	unsigned int div;

	if (!clk_rate)
		return -EINVAL;

	if (val <= 0)
		return -EINVAL;

	/* Max. 150 kSPS */
	if (val > XADC_MAX_SAMPLERATE)
		val = XADC_MAX_SAMPLERATE;

	val *= 26;

	/* Min 1MHz */
	if (val < 1000000)
		val = 1000000;

	/*
	 * We want to round down, but only if we do not exceed the 150 kSPS
	 * limit.
	 */
	div = clk_rate / val;
	if (clk_rate / div / 26 > XADC_MAX_SAMPLERATE)
		div++;
	if (div < 2)
		div = 2;
	else if (div > 0xff)
		div = 0xff;

	return xadc_update_adc_reg(xadc, XADC_REG_CONF2, XADC_CONF2_DIV_MASK,
		div << XADC_CONF2_DIV_OFFSET);
}
EXPORT_SYMBOL_GPL(xadc_write_samplerate);

static int xadc_write_raw(struct iio_dev *indio_dev,
	struct iio_chan_spec const *chan, int val, int val2, long info)
{
	struct xadc *xadc = iio_priv(indio_dev);

	if (info != IIO_CHAN_INFO_SAMP_FREQ)
		return -EINVAL;

	return xadc_write_samplerate(xadc, val);
}

static const struct iio_event_spec xadc_temp_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE) |
				BIT(IIO_EV_INFO_VALUE) |
				BIT(IIO_EV_INFO_HYSTERESIS),
	},
};

/* Separate values for upper and lower thresholds, but only a shared enabled */
static const struct iio_event_spec xadc_voltage_events[] = {
	{
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_RISING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_FALLING,
		.mask_separate = BIT(IIO_EV_INFO_VALUE),
	}, {
		.type = IIO_EV_TYPE_THRESH,
		.dir = IIO_EV_DIR_EITHER,
		.mask_separate = BIT(IIO_EV_INFO_ENABLE),
	},
};

#define XADC_CHAN_TEMP(_chan, _scan_index, _addr, _bits) { \
	.type = IIO_TEMP, \
	.indexed = 1, \
	.channel = (_chan), \
	.address = (_addr), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
		BIT(IIO_CHAN_INFO_SCALE) | \
		BIT(IIO_CHAN_INFO_OFFSET), \
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
	.event_spec = xadc_temp_events, \
	.num_event_specs = ARRAY_SIZE(xadc_temp_events), \
	.scan_index = (_scan_index), \
	.scan_type = { \
		.sign = 'u', \
		.realbits = (_bits), \
		.storagebits = 16, \
		.shift = 16 - (_bits), \
		.endianness = IIO_CPU, \
	}, \
}

#define XADC_CHAN_VOLTAGE(_chan, _scan_index, _addr, _bits, _ext, _alarm) { \
	.type = IIO_VOLTAGE, \
	.indexed = 1, \
	.channel = (_chan), \
	.address = (_addr), \
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) | \
		BIT(IIO_CHAN_INFO_SCALE), \
	.info_mask_shared_by_all = BIT(IIO_CHAN_INFO_SAMP_FREQ), \
	.event_spec = (_alarm) ? xadc_voltage_events : NULL, \
	.num_event_specs = (_alarm) ? ARRAY_SIZE(xadc_voltage_events) : 0, \
	.scan_index = (_scan_index), \
	.scan_type = { \
		.sign = ((_addr) == XADC_REG_VREFN) ? 's' : 'u', \
		.realbits = (_bits), \
		.storagebits = 16, \
		.shift = 16 - (_bits), \
		.endianness = IIO_CPU, \
	}, \
	.extend_name = _ext, \
}

/* 7 Series */
#define XADC_7S_CHAN_TEMP(_chan, _scan_index, _addr) \
	XADC_CHAN_TEMP(_chan, _scan_index, _addr, 12)
#define XADC_7S_CHAN_VOLTAGE(_chan, _scan_index, _addr, _ext, _alarm) \
	XADC_CHAN_VOLTAGE(_chan, _scan_index, _addr, 12, _ext, _alarm)

static const struct iio_chan_spec xadc_7s_channels[] = {
	XADC_7S_CHAN_TEMP(0, 8, XADC_REG_TEMP),
	XADC_7S_CHAN_VOLTAGE(0, 9, XADC_REG_VCCINT, "vccint", true),
	XADC_7S_CHAN_VOLTAGE(1, 10, XADC_REG_VCCAUX, "vccaux", true),
	XADC_7S_CHAN_VOLTAGE(2, 14, XADC_REG_VCCBRAM, "vccbram", true),
	XADC_7S_CHAN_VOLTAGE(3, 5, XADC_REG_VCCPINT, "vccpint", true),
	XADC_7S_CHAN_VOLTAGE(4, 6, XADC_REG_VCCPAUX, "vccpaux", true),
	XADC_7S_CHAN_VOLTAGE(5, 7, XADC_REG_VCCO_DDR, "vccoddr", true),
	XADC_7S_CHAN_VOLTAGE(6, 12, XADC_REG_VREFP, "vrefp", false),
	XADC_7S_CHAN_VOLTAGE(7, 13, XADC_REG_VREFN, "vrefn", false),
	XADC_7S_CHAN_VOLTAGE(8, 11, XADC_REG_VPVN, NULL, false),
	XADC_7S_CHAN_VOLTAGE(9, 16, XADC_REG_VAUX(0), NULL, false),
	XADC_7S_CHAN_VOLTAGE(10, 17, XADC_REG_VAUX(1), NULL, false),
	XADC_7S_CHAN_VOLTAGE(11, 18, XADC_REG_VAUX(2), NULL, false),
	XADC_7S_CHAN_VOLTAGE(12, 19, XADC_REG_VAUX(3), NULL, false),
	XADC_7S_CHAN_VOLTAGE(13, 20, XADC_REG_VAUX(4), NULL, false),
	XADC_7S_CHAN_VOLTAGE(14, 21, XADC_REG_VAUX(5), NULL, false),
	XADC_7S_CHAN_VOLTAGE(15, 22, XADC_REG_VAUX(6), NULL, false),
	XADC_7S_CHAN_VOLTAGE(16, 23, XADC_REG_VAUX(7), NULL, false),
	XADC_7S_CHAN_VOLTAGE(17, 24, XADC_REG_VAUX(8), NULL, false),
	XADC_7S_CHAN_VOLTAGE(18, 25, XADC_REG_VAUX(9), NULL, false),
	XADC_7S_CHAN_VOLTAGE(19, 26, XADC_REG_VAUX(10), NULL, false),
	XADC_7S_CHAN_VOLTAGE(20, 27, XADC_REG_VAUX(11), NULL, false),
	XADC_7S_CHAN_VOLTAGE(21, 28, XADC_REG_VAUX(12), NULL, false),
	XADC_7S_CHAN_VOLTAGE(22, 29, XADC_REG_VAUX(13), NULL, false),
	XADC_7S_CHAN_VOLTAGE(23, 30, XADC_REG_VAUX(14), NULL, false),
	XADC_7S_CHAN_VOLTAGE(24, 31, XADC_REG_VAUX(15), NULL, false),
};

/* UltraScale */
#define XADC_US_CHAN_TEMP(_chan, _scan_index, _addr) \
	XADC_CHAN_TEMP(_chan, _scan_index, _addr, 10)
#define XADC_US_CHAN_VOLTAGE(_chan, _scan_index, _addr, _ext, _alarm) \
	XADC_CHAN_VOLTAGE(_chan, _scan_index, _addr, 10, _ext, _alarm)

static const struct iio_chan_spec xadc_us_channels[] = {
	XADC_US_CHAN_TEMP(0, 8, XADC_REG_TEMP),
	XADC_US_CHAN_VOLTAGE(0, 9, XADC_REG_VCCINT, "vccint", true),
	XADC_US_CHAN_VOLTAGE(1, 10, XADC_REG_VCCAUX, "vccaux", true),
	XADC_US_CHAN_VOLTAGE(2, 14, XADC_REG_VCCBRAM, "vccbram", true),
	XADC_US_CHAN_VOLTAGE(3, 5, XADC_REG_VCCPINT, "vccpsintlp", true),
	XADC_US_CHAN_VOLTAGE(4, 6, XADC_REG_VCCPAUX, "vccpsintfp", true),
	XADC_US_CHAN_VOLTAGE(5, 7, XADC_REG_VCCO_DDR, "vccpsaux", true),
	XADC_US_CHAN_VOLTAGE(6, 12, XADC_REG_VREFP, "vrefp", false),
	XADC_US_CHAN_VOLTAGE(7, 13, XADC_REG_VREFN, "vrefn", false),
	XADC_US_CHAN_VOLTAGE(8, 11, XADC_REG_VPVN, NULL, false),
	XADC_US_CHAN_VOLTAGE(9, 16, XADC_REG_VAUX(0), NULL, false),
	XADC_US_CHAN_VOLTAGE(10, 17, XADC_REG_VAUX(1), NULL, false),
	XADC_US_CHAN_VOLTAGE(11, 18, XADC_REG_VAUX(2), NULL, false),
	XADC_US_CHAN_VOLTAGE(12, 19, XADC_REG_VAUX(3), NULL, false),
	XADC_US_CHAN_VOLTAGE(13, 20, XADC_REG_VAUX(4), NULL, false),
	XADC_US_CHAN_VOLTAGE(14, 21, XADC_REG_VAUX(5), NULL, false),
	XADC_US_CHAN_VOLTAGE(15, 22, XADC_REG_VAUX(6), NULL, false),
	XADC_US_CHAN_VOLTAGE(16, 23, XADC_REG_VAUX(7), NULL, false),
	XADC_US_CHAN_VOLTAGE(17, 24, XADC_REG_VAUX(8), NULL, false),
	XADC_US_CHAN_VOLTAGE(18, 25, XADC_REG_VAUX(9), NULL, false),
	XADC_US_CHAN_VOLTAGE(19, 26, XADC_REG_VAUX(10), NULL, false),
	XADC_US_CHAN_VOLTAGE(20, 27, XADC_REG_VAUX(11), NULL, false),
	XADC_US_CHAN_VOLTAGE(21, 28, XADC_REG_VAUX(12), NULL, false),
	XADC_US_CHAN_VOLTAGE(22, 29, XADC_REG_VAUX(13), NULL, false),
	XADC_US_CHAN_VOLTAGE(23, 30, XADC_REG_VAUX(14), NULL, false),
	XADC_US_CHAN_VOLTAGE(24, 31, XADC_REG_VAUX(15), NULL, false),
};

static const struct iio_info xadc_info = {
	.read_raw = &xadc_read_raw,
	.write_raw = &xadc_write_raw,
	.read_event_config = &xadc_read_event_config,
	.write_event_config = &xadc_write_event_config,
	.read_event_value = &xadc_read_event_value,
	.write_event_value = &xadc_write_event_value,
	.update_scan_mode = &xadc_update_scan_mode,
};

static int xadc_parse_dt(struct iio_dev *indio_dev, unsigned int *conf, int irq)
{
	struct device *dev = indio_dev->dev.parent;
	struct xadc *xadc = iio_priv(indio_dev);
	const struct iio_chan_spec *channel_templates;
	struct iio_chan_spec *channels, *chan;
	struct fwnode_handle *chan_node, *child;
	unsigned int max_channels;
	unsigned int num_channels;
	const char *external_mux;
	u32 ext_mux_chan;
	u32 reg;
	int ret;
	int i;

	*conf = 0;

	ret = device_property_read_string(dev, "xlnx,external-mux", &external_mux);
	if (ret < 0 || strcasecmp(external_mux, "none") == 0)
		xadc->external_mux_mode = XADC_EXTERNAL_MUX_NONE;
	else if (strcasecmp(external_mux, "single") == 0)
		xadc->external_mux_mode = XADC_EXTERNAL_MUX_SINGLE;
	else if (strcasecmp(external_mux, "dual") == 0)
		xadc->external_mux_mode = XADC_EXTERNAL_MUX_DUAL;
	else
		return -EINVAL;

	if (xadc->external_mux_mode != XADC_EXTERNAL_MUX_NONE) {
		ret = device_property_read_u32(dev, "xlnx,external-mux-channel", &ext_mux_chan);
		if (ret < 0)
			return ret;

		if (xadc->external_mux_mode == XADC_EXTERNAL_MUX_SINGLE) {
			if (ext_mux_chan == 0)
				ext_mux_chan = XADC_REG_VPVN;
			else if (ext_mux_chan <= 16)
				ext_mux_chan = XADC_REG_VAUX(ext_mux_chan - 1);
			else
				return -EINVAL;
		} else {
			if (ext_mux_chan > 0 && ext_mux_chan <= 8)
				ext_mux_chan = XADC_REG_VAUX(ext_mux_chan - 1);
			else
				return -EINVAL;
		}

		*conf |= XADC_CONF0_MUX | XADC_CONF0_CHAN(ext_mux_chan);
	}
	if (xadc->ops->type == XADC_TYPE_S7) {
		channel_templates = xadc_7s_channels;
		max_channels = ARRAY_SIZE(xadc_7s_channels);
	} else {
		channel_templates = xadc_us_channels;
		max_channels = ARRAY_SIZE(xadc_us_channels);
	}
	channels = devm_kmemdup_array(dev, channel_templates, max_channels,
				      sizeof(*channel_templates), GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	num_channels = 9;
	chan = &channels[9];

	chan_node = device_get_named_child_node(dev, "xlnx,channels");
	fwnode_for_each_child_node(chan_node, child) {
		if (num_channels >= max_channels) {
			fwnode_handle_put(child);
			break;
		}

		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret || reg > 16)
			continue;

		if (fwnode_property_read_bool(child, "xlnx,bipolar"))
			chan->scan_type.sign = 's';

		if (reg == 0) {
			chan->scan_index = 11;
			chan->address = XADC_REG_VPVN;
		} else {
			chan->scan_index = 15 + reg;
			chan->address = XADC_REG_VAUX(reg - 1);
		}
		num_channels++;
		chan++;
	}
	fwnode_handle_put(chan_node);

	/* No IRQ => no events */
	if (irq <= 0) {
		for (i = 0; i < num_channels; i++) {
			channels[i].event_spec = NULL;
			channels[i].num_event_specs = 0;
		}
	}

	indio_dev->num_channels = num_channels;
	indio_dev->channels = devm_krealloc_array(dev, channels,
						  num_channels, sizeof(*channels),
						  GFP_KERNEL);
	/* If we can't resize the channels array, just use the original */
	if (!indio_dev->channels)
		indio_dev->channels = channels;

	return 0;
}

const char * const xadc_type_names[] = {
	[XADC_TYPE_S7] = "xadc",
	[XADC_TYPE_US] = "xilinx-system-monitor",
};

struct iio_dev *xadc_device_setup(struct device *dev, int size,
				  const struct xadc_ops **ops)
{
	struct iio_dev *indio_dev;

	*ops = device_get_match_data(dev);
	if (!*ops)
		return ERR_PTR(-ENODEV);

	indio_dev = devm_iio_device_alloc(dev, size);
	if (!indio_dev)
		return ERR_PTR(-ENOMEM);

	indio_dev->name = xadc_type_names[(*ops)->type];
	indio_dev->info = &xadc_info;
	indio_dev->modes = INDIO_DIRECT_MODE;

	return indio_dev;
}
EXPORT_SYMBOL_GPL(xadc_device_setup);

int xadc_device_configure(struct device *dev, struct iio_dev *indio_dev,
			  int irq, unsigned int *conf0,
			  unsigned int *bipolar_mask)
{
	int ret;
	u32 i;

	ret = xadc_parse_dt(indio_dev, conf0, irq);
	if (ret)
		return ret;

	*bipolar_mask = 0;
	for (i = 0; i < indio_dev->num_channels; i++) {
		if (indio_dev->channels[i].scan_type.sign == 's')
			*bipolar_mask |= BIT(indio_dev->channels[i].scan_index);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(xadc_device_configure);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Lars-Peter Clausen <lars@metafoo.de>");
MODULE_DESCRIPTION("Xilinx XADC IIO core driver");
