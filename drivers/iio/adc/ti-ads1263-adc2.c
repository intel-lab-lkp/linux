// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Texas Instruments ADS1263 auxiliary ADC (ADC2) driver
 *
 * Copyright (C) 2025 Kurt Borja <kuurtb@gmail.com>
 */

#include <linux/align.h>
#include <linux/array_size.h>
#include <linux/bitmap.h>
#include <linux/bitops.h>
#include <linux/cleanup.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dev_printk.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#include <linux/iio/iio.h>
#include <linux/iio/trigger_consumer.h>
#include <linux/iio/triggered_buffer.h>

#include "ti-ads1262.h"

/* ADC2CFG REF2 constants */
#define ADS1263_ADC2_REF2_INTER			0
#define ADS1263_ADC2_REF2_COUNT			5

struct ads1263_adc2 {
	struct iio_dev *indio_dev;
	struct ads1263_adc2_ctx *ctx;
	u32 vref_uV;
	u32 refmux;
};

static const int ads1263_adc2_gain_avail[] = {
	1, 2, 4, 8, 16, 32, 64, 128
};

static const int ads1263_adc2_data_rate_avail[] = {
	10, 100, 400, 800
};

static const unsigned long ads1263_adc2_latency_us[] = {
	121000, 31200, 8710, 4970
};

static const struct iio_chan_spec ads1263_adc2_iio_voltage_template = {
	.type = IIO_VOLTAGE,
	.indexed = true,
	.scan_type = {
		.format = IIO_SCAN_FORMAT_SIGNED_INT,
		.realbits = 24,
		.storagebits = 32,
		.shift = 8,
		.endianness = IIO_BE,
	},
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW) |
			      BIT(IIO_CHAN_INFO_SCALE) |
			      BIT(IIO_CHAN_INFO_HARDWAREGAIN) |
			      BIT(IIO_CHAN_INFO_SAMP_FREQ),
	.info_mask_shared_by_all_available = BIT(IIO_CHAN_INFO_HARDWAREGAIN) |
					     BIT(IIO_CHAN_INFO_SAMP_FREQ),
};

static int ads1263_adc2_channel_hot_reload(struct ads1263_adc2 *st,
					   const struct iio_chan_spec *chan)
{
	struct ads1263_adc2_ctx *ctx = st->ctx;
	unsigned long i;
	int ret;

	/* Hot reloading is only required on buffer mode */
	if (!iio_device_try_claim_buffer_mode(st->indio_dev))
		return 0;

	i = find_first_bit(st->indio_dev->active_scan_mask,
			   iio_get_masklength(st->indio_dev));
	if (i != chan->scan_index) {
		iio_device_release_direct(st->indio_dev);
		return 0;
	}

	ret = ctx->enable(ctx, &ctx->channels[chan->scan_index]);

	iio_device_release_buffer_mode(st->indio_dev);

	return ret;
}

static int ads1263_adc2_channel_read(struct iio_dev *indio_dev,
				     struct ads1263_adc2_channel *chan_data,
				     __be32 *val)
{
	struct ads1263_adc2 *st = iio_priv(indio_dev);
	struct ads1263_adc2_ctx *ctx = st->ctx;
	struct device *dev = &ctx->adev.dev;
	int ret;

	PM_RUNTIME_ACQUIRE_AUTOSUSPEND(dev->parent, pm);
	if (PM_RUNTIME_ACQUIRE_ERR(&pm))
		return -ENXIO;

	IIO_DEV_ACQUIRE_DIRECT_MODE(indio_dev, claim);
	if (IIO_DEV_ACQUIRE_FAILED(claim))
		return -EBUSY;

	ret = ctx->enable(ctx, chan_data);
	if (ret)
		return ret;

	ret = ctx->start(ctx);
	if (ret)
		return ret;

	ret = ctx->stop(ctx);
	if (ret)
		return ret;

	fsleep(ads1263_adc2_latency_us[chan_data->data_rate]);

	return ctx->read(ctx, val);
}

static int ads1263_adc2_read_raw(struct iio_dev *indio_dev,
				 struct iio_chan_spec const *chan,
				 int *val, int *val2, long mask)
{
	struct ads1263_adc2 *st = iio_priv(indio_dev);
	struct ads1263_adc2_ctx *ctx = st->ctx;
	struct ads1263_adc2_channel *chan_data;
	u8 realbits;
	__be32 raw;
	u32 cnv;
	int ret;

	chan_data = &st->ctx->channels[chan->scan_index];
	realbits = chan->scan_type.realbits;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = ads1263_adc2_channel_read(indio_dev, chan_data, &raw);
		if (ret)
			return ret;

		cnv = be32_to_cpu(raw);
		cnv >>= chan->scan_type.shift;
		*val = sign_extend32(cnv, realbits - 1);

		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SCALE:
		u64 divd, divr, tmp, rem;

		mutex_lock(&ctx->chan_lock);
		divd = st->vref_uV;
		divr = BIT_ULL(chan_data->gain + realbits - 1) * 1000;
		mutex_unlock(&ctx->chan_lock);

		tmp = div64_u64(divd * 1000000000ULL, divr);
		*val = div64_u64_rem(tmp, 1000000000ULL, &rem);
		*val2 = rem;

		return IIO_VAL_INT_PLUS_NANO;

	case IIO_CHAN_INFO_HARDWAREGAIN:
		mutex_lock(&ctx->chan_lock);
		*val = ads1263_adc2_gain_avail[chan_data->gain];
		mutex_unlock(&ctx->chan_lock);
		return IIO_VAL_INT;

	case IIO_CHAN_INFO_SAMP_FREQ:
		mutex_lock(&ctx->chan_lock);
		*val = ads1263_adc2_data_rate_avail[chan_data->data_rate];
		mutex_unlock(&ctx->chan_lock);

		return IIO_VAL_INT;

	default:
		return -EOPNOTSUPP;
	}
}

static int
ads1263_adc2_read_avail(struct iio_dev *indio_dev,
			struct iio_chan_spec const *chan, const int **vals,
			int *type, int *length, long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		*type = IIO_VAL_INT;
		*vals = ads1263_adc2_gain_avail;
		*length = ARRAY_SIZE(ads1263_adc2_gain_avail);
		return IIO_AVAIL_LIST;

	case IIO_CHAN_INFO_SAMP_FREQ:
		*type = IIO_VAL_INT;
		*vals = ads1263_adc2_data_rate_avail;
		*length = ARRAY_SIZE(ads1263_adc2_data_rate_avail);
		return IIO_AVAIL_LIST;

	default:
		return -EOPNOTSUPP;
	}
}

static int ads1263_adc2_write_raw(struct iio_dev *indio_dev,
				  struct iio_chan_spec const *chan,
				  int val, int val2, long mask)
{
	struct ads1263_adc2 *st = iio_priv(indio_dev);
	struct ads1263_adc2_ctx *ctx = st->ctx;
	struct ads1263_adc2_channel *chan_data;
	unsigned int i;

	chan_data = &ctx->channels[chan->scan_index];

	switch (mask) {
	case IIO_CHAN_INFO_HARDWAREGAIN:
		for (i = 0; i < ARRAY_SIZE(ads1263_adc2_gain_avail); i++) {
			if (val == ads1263_adc2_gain_avail[i])
				break;
		}
		if (i == ARRAY_SIZE(ads1263_adc2_gain_avail))
			return -EINVAL;

		mutex_lock(&ctx->chan_lock);
		chan_data->gain = i;
		mutex_unlock(&ctx->chan_lock);

		return 0;

	case IIO_CHAN_INFO_SAMP_FREQ:
		for (i = 0; i < ARRAY_SIZE(ads1263_adc2_data_rate_avail); i++) {
			if (val == ads1263_adc2_data_rate_avail[i])
				break;
		}
		if (i == ARRAY_SIZE(ads1263_adc2_data_rate_avail))
			return -EINVAL;

		mutex_lock(&ctx->chan_lock);
		chan_data->data_rate = i;
		mutex_unlock(&ctx->chan_lock);

		return 0;

	default:
		return -EOPNOTSUPP;
	}

	return ads1263_adc2_channel_hot_reload(st, chan);
}

static int ads1263_adc2_write_raw_get_fmt(struct iio_dev *indio_dev,
					  struct iio_chan_spec const *chan,
					  long mask)
{
	switch (mask) {
	case IIO_CHAN_INFO_CONVDELAY:
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return IIO_VAL_INT_PLUS_MICRO;
	}
}

static const struct iio_info ads1263_adc2_iio_info = {
	.read_raw = ads1263_adc2_read_raw,
	.read_avail = ads1263_adc2_read_avail,
	.write_raw = ads1263_adc2_write_raw,
	.write_raw_get_fmt = ads1263_adc2_write_raw_get_fmt,
};

static int ads1263_adc2_buffer_preenable(struct iio_dev *indio_dev)
{
	struct ads1263_adc2 *st = iio_priv(indio_dev);
	struct ads1263_adc2_ctx *ctx = st->ctx;
	struct device *dev = &ctx->adev.dev;
	unsigned long i;
	int ret;

	ret = pm_runtime_resume_and_get(dev->parent);
	if (ret)
		return ret;

	i = find_first_bit(indio_dev->active_scan_mask,
			   iio_get_masklength(indio_dev));
	ret = ctx->enable(ctx, &ctx->channels[i]);
	if (ret)
		goto out_runtime_autosuspend;

	ret = ctx->start(ctx);
	if (ret)
		goto out_runtime_autosuspend;

	return 0;

out_runtime_autosuspend:
	pm_runtime_put_autosuspend(dev->parent);

	return ret;
}

static int ads1263_adc2_buffer_postdisable(struct iio_dev *indio_dev)
{
	struct ads1263_adc2 *st = iio_priv(indio_dev);
	struct ads1263_adc2_ctx *ctx = st->ctx;
	struct device *dev = &ctx->adev.dev;

	ctx->stop(ctx);
	pm_runtime_put_autosuspend(dev->parent);

	return 0;
}

static const struct iio_buffer_setup_ops ads1263_adc2_buffer_ops = {
	.preenable = ads1263_adc2_buffer_preenable,
	.postdisable = ads1263_adc2_buffer_postdisable,
	.validate_scan_mask = iio_validate_scan_mask_onehot,
};

static irqreturn_t ads1263_adc2_trigger_handler(int irq, void *p)
{
	struct iio_poll_func *pf = p;
	struct iio_dev *indio_dev = pf->indio_dev;
	struct ads1263_adc2 *st = iio_priv(indio_dev);
	struct ads1263_adc2_ctx *ctx = st->ctx;
	struct {
		__be32 conv;
		aligned_s64 ts;
	} scan = {};
	int ret;

	ret = ctx->read(ctx, &scan.conv);
	if (ret)
		goto out_notify_done;

	iio_push_to_buffers_with_ts(indio_dev, &scan, sizeof(scan),
				    pf->timestamp);

out_notify_done:
	iio_trigger_notify_done(indio_dev->trig);

	return IRQ_HANDLED;
}

static int ads1263_adc2_channels_setup(struct iio_dev *indio_dev)
{
	struct ads1263_adc2 *st = iio_priv(indio_dev);
	struct device *dev = &st->ctx->adev.dev;
	struct ads1263_adc2_ctx *ctx = st->ctx;
	struct iio_chan_spec *chns;
	unsigned int i;

	/* Account for the timestamp channel */
	chns = devm_kcalloc(dev, ctx->num_channels + 1, sizeof(*chns),
			    GFP_KERNEL);
	if (!chns)
		return -ENOMEM;

	for (i = 0; i < ctx->num_channels; i++) {
		guard(mutex)(&ctx->chan_lock);

		ctx->channels[i].refmux = st->refmux;

		chns[i] = ads1263_adc2_iio_voltage_template;
		chns[i].scan_index = i;
		chns[i].channel = ctx->channels[i].positive_input;
		chns[i].channel2 = ctx->channels[i].negative_input;
		chns[i].differential = true;
	}

	chns[i] = (struct iio_chan_spec)
		IIO_CHAN_SOFT_TIMESTAMP(ctx->num_channels - 1);
	chns[i].scan_index = i;

	indio_dev->num_channels = ctx->num_channels + 1;
	indio_dev->channels = chns;

	return 0;
}

static int ads1263_adc2_regulator_setup(struct ads1263_adc2 *st)
{
	struct device *dev = &st->ctx->adev.dev;
	const char *reg_id, *propname;
	u32 refmux = 0;
	int ret;

	propname = "ti,refmux";
	ret = device_property_read_u32(dev, propname, &refmux);
	if (refmux >= ADS1263_ADC2_REF2_COUNT)
		return dev_err_probe(dev, ret, "%s out of range\n", propname);
	st->refmux = refmux;

	if (refmux == ADS1263_ADC2_REF2_INTER) {
		/* The internal voltage reference is 2.5 V */
		st->vref_uV = 2500000;
		return 0;
	}

	reg_id = "vref";
	ret = devm_regulator_get_enable_read_voltage(dev, reg_id);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulator %s\n",
				     reg_id);
	st->vref_uV = ret;

	return 0;
}

static int ads1263_adc2_probe(struct auxiliary_device *auxdev,
			      const struct auxiliary_device_id *id)
{
	struct ads1263_adc2_ctx *ctx =
		container_of(auxdev, struct ads1263_adc2_ctx, adev);
	struct device *dev = &auxdev->dev;
	struct iio_dev *indio_dev;
	struct ads1263_adc2 *st;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->ctx = ctx;
	st->indio_dev = indio_dev;

	ret = ads1263_adc2_regulator_setup(st);
	if (ret)
		return ret;

	indio_dev->name = (char *)id->driver_data;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ads1263_adc2_iio_info;
	ret = ads1263_adc2_channels_setup(indio_dev);
	if (ret)
		return ret;

	ret = devm_iio_triggered_buffer_setup(dev, indio_dev,
					      iio_pollfunc_store_time,
					      ads1263_adc2_trigger_handler,
					      &ads1263_adc2_buffer_ops);
	if (ret)
		return ret;

	return devm_iio_device_register(dev, indio_dev);
}

static const struct auxiliary_device_id ads1263_adc2_auxiliary_match[] = {
	{ .name = "ti_ads1262.ads1263_adc2",
	  .driver_data = (kernel_ulong_t)"ads1263_adc2" },
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, ads1263_adc2_auxiliary_match);

static struct auxiliary_driver ads1263_adc2_driver = {
	.name = "ads1263_adc2",
	.probe = ads1263_adc2_probe,
	.id_table = ads1263_adc2_auxiliary_match,
};
module_auxiliary_driver(ads1263_adc2_driver);

MODULE_DESCRIPTION("Texas Instruments ADS1263 auxiliary ADC (ADC2) driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kurt Borja <kuurtb@gmail.com>");
