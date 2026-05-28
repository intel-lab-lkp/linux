// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Analog Devices, Inc.
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#include <linux/cleanup.h>
#include <linux/err.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/pwm.h>
#include <linux/spi/spi.h>
#include <linux/spi/offload/consumer.h>
#include <linux/spi/offload/types.h>
#include <linux/time64.h>

#include <linux/iio/buffer.h>
#include <linux/iio/buffer-dmaengine.h>
#include <linux/iio/iio.h>
#include <linux/iio/types.h>

#include "ltc2378.h"

/*
 * SPI offload wiring schema
 *
 *     +-------------+         +-------------+
 *     |         CNV |<-----+--| GPIO        |
 *     |             |      +--| PWM0        |
 *     |             |         |             |
 *     |             |      +--| PWM1        |
 *     |             |      |  +-------------+
 *     |             |      +->| TRIGGER     |
 *     |             |         |             |
 *     |     ADC     |         |    SPI      |
 *     |             |         | controller  |
 *     |             |         |             |
 *     |         SDI |<--------| SDO         |
 *     |         SDO |-------->| SDI         |
 *     |        SCLK |<--------| SCLK        |
 *     +-------------+         +-------------+
 *
 */
static int ltc2378_update_conversion_rate(struct ltc2378_state *st, int freq_Hz)
{
	struct spi_offload_trigger_config *config = &st->offload_trigger_config;
	unsigned int min_read_offset, offload_period_ns;
	struct pwm_waveform cnv_wf = { };
	u64 target = LTC2378_TCNV_HIGH_NS;
	unsigned int count = 0;
	u64 offload_offset_ns;
	int ret;

	if (freq_Hz == 0)
		return -EINVAL;

	if (freq_Hz < 1 || freq_Hz > st->info->max_sample_rate_hz)
		return -ERANGE;

	/* Configure CNV PWM waveform */
	cnv_wf.period_length_ns = DIV_ROUND_CLOSEST(NSEC_PER_SEC, freq_Hz);

	/*
	 * Ensure CNV high time meets minimum requirement (20ns). The PWM
	 * hardware may round the duty cycle, so iterate until we get at least
	 * the minimum required high time.
	 */
	do {
		cnv_wf.duty_length_ns = target;
		ret = pwm_round_waveform_might_sleep(st->cnv_trigger, &cnv_wf);
		if (ret)
			return ret;
		target += 10;  /* Increment by PWM duty cycle period */
	} while (cnv_wf.duty_length_ns < LTC2378_TCNV_HIGH_NS || count++ < 100);

	/*
	 * Configure SPI offload PWM trigger.
	 * The trigger should fire after tBUSYLH + tCONV + tDSDOBUSYL.
	 * Minimum time needed: TBUSYLH (13ns) + TCONV (part-specific) + TDSDOBUSYL (5ns)
	 *
	 * Use the same period as CNV PWM to avoid timing issues.
	 * Convert back from period to frequency for the SPI offload API.
	 */
	offload_period_ns = cnv_wf.period_length_ns;
	config->periodic.frequency_hz = DIV_ROUND_UP(HZ_PER_GHZ, offload_period_ns);
	min_read_offset = LTC2378_TBUSYLH_NS + st->info->tconv_ns + LTC2378_TDSDOBUSYL_NS;
	offload_offset_ns = min_read_offset;
	count = 0;
	do {
		config->periodic.offset_ns = offload_offset_ns;
		ret = spi_offload_trigger_validate(st->offload_trigger, config);
		if (ret)
			return ret;
		offload_offset_ns += 10;
	} while (config->periodic.offset_ns < min_read_offset || count++ < 100);

	st->cnv_wf = cnv_wf;
	st->cnv_Hz = DIV_ROUND_CLOSEST_ULL(HZ_PER_GHZ, cnv_wf.period_length_ns);

	return 0;
}

int ltc2378_get_sampling_frequency(struct ltc2378_state *st, int *val)
{
	*val = st->cnv_Hz;

	return 0;
}
EXPORT_SYMBOL_NS_GPL(ltc2378_get_sampling_frequency, "IIO_LTC2378");

int ltc2378_set_sampling_frequency(struct ltc2378_state *st, int freq_Hz)
{
	return ltc2378_update_conversion_rate(st, freq_Hz);
}
EXPORT_SYMBOL_NS_GPL(ltc2378_set_sampling_frequency, "IIO_LTC2378");

int ltc2378_read_avail(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
		       const int **vals, int *type, int *length, long mask)
{
	struct ltc2378_state *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ:
		*vals = st->sample_freq_range;
		*type = IIO_VAL_INT;
		return IIO_AVAIL_RANGE;
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_NS_GPL(ltc2378_read_avail, "IIO_LTC2378");

int ltc2378_write_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
		      int val, int val2, long mask)
{
	struct ltc2378_state *st = iio_priv(indio_dev);

	switch (mask) {
	case IIO_CHAN_INFO_SAMP_FREQ: {
		if (val < 1 || val > st->info->max_sample_rate_hz)
			return -EINVAL;

		IIO_DEV_ACQUIRE_DIRECT_MODE(indio_dev, claim);
		if (IIO_DEV_ACQUIRE_FAILED(claim))
			return -EBUSY;

		return ltc2378_set_sampling_frequency(st, val);
	}
	default:
		return -EINVAL;
	}
}
EXPORT_SYMBOL_NS_GPL(ltc2378_write_raw, "IIO_LTC2378");

static int ltc2378_prepare_offload_message(struct device *dev,
					   struct ltc2378_state *st)
{
	st->offload_xfer.bits_per_word = st->info->resolution;
	/*
	 * Ideally, we would ask the offload provider what data word sizes are
	 * supported so we could use smaller words for less precise ADCs.
	 * Though, the currently available SPI offloading hardware only supports
	 * pushing 32-bit sized data elements to DMA memory. Because of that,
	 * we hardcode set 4 byte sized transfers.
	 */
	st->offload_xfer.len = 4;
	st->offload_xfer.offload_flags = SPI_OFFLOAD_XFER_RX_STREAM;

	/* Initialize message with offload */
	spi_message_init_with_transfers(&st->offload_msg, &st->offload_xfer, 1);
	st->offload_msg.offload = st->offload;

	return devm_spi_optimize_message(dev, st->spi, &st->offload_msg);
}

static int ltc2378_offload_buffer_postenable(struct iio_dev *indio_dev)
{
	struct ltc2378_state *st = iio_priv(indio_dev);
	int ret;

	ret = pwm_set_waveform_might_sleep(st->cnv_trigger, &st->cnv_wf, false);
	if (ret)
		return ret;

	ret = spi_offload_trigger_enable(st->offload, st->offload_trigger,
					 &st->offload_trigger_config);
	if (ret)
		goto out_pwm_disable;

	return 0;

out_pwm_disable:
	pwm_disable(st->cnv_trigger);
	return ret;
}

static int ltc2378_offload_buffer_predisable(struct iio_dev *indio_dev)
{
	struct ltc2378_state *st = iio_priv(indio_dev);

	spi_offload_trigger_disable(st->offload, st->offload_trigger);
	pwm_disable(st->cnv_trigger);

	return 0;
}

static const struct iio_buffer_setup_ops ltc2378_offload_buffer_ops = {
	.postenable = &ltc2378_offload_buffer_postenable,
	.predisable = &ltc2378_offload_buffer_predisable,
};

static int ltc2378_spi_offload_setup(struct iio_dev *indio_dev,
				     struct ltc2378_state *st)
{
	struct device *dev = &st->spi->dev;
	struct dma_chan *rx_dma;

	indio_dev->setup_ops = &ltc2378_offload_buffer_ops;

	st->offload_trigger = devm_spi_offload_trigger_get(dev, st->offload,
							   SPI_OFFLOAD_TRIGGER_PERIODIC);
	if (IS_ERR(st->offload_trigger))
		return dev_err_probe(dev, PTR_ERR(st->offload_trigger),
				     "failed to get offload trigger\n");

	st->offload_trigger_config.type = SPI_OFFLOAD_TRIGGER_PERIODIC;

	rx_dma = devm_spi_offload_rx_stream_request_dma_chan(dev, st->offload);
	if (IS_ERR(rx_dma))
		return dev_err_probe(dev, PTR_ERR(rx_dma), "failed to get offload RX DMA\n");

	return devm_iio_dmaengine_buffer_setup_with_handle(dev, indio_dev, rx_dma,
							   IIO_BUFFER_DIRECTION_IN);
}

static int ltc2378_pwm_get(struct ltc2378_state *st)
{
	struct device *dev = &st->spi->dev;

	st->cnv_trigger = devm_pwm_get(dev, NULL);
	if (IS_ERR(st->cnv_trigger))
		return dev_err_probe(dev, PTR_ERR(st->cnv_trigger),
				     "failed to get cnv pwm\n");

	pwm_disable(st->cnv_trigger);

	return 0;
}

static const struct spi_offload_config ltc2378_offload_config = {
	.capability_flags = SPI_OFFLOAD_CAP_TRIGGER |
			    SPI_OFFLOAD_CAP_RX_STREAM_DMA,
};

int ltc2378_offload_buffer_setup(struct iio_dev *indio_dev, struct spi_device *spi)
{
	struct ltc2378_state *st = iio_priv(indio_dev);
	struct device *dev = &spi->dev;
	int ret;

	st->offload = devm_spi_offload_get(dev, spi, &ltc2378_offload_config);
	ret = PTR_ERR_OR_ZERO(st->offload);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get offload\n");

	ret = ltc2378_spi_offload_setup(indio_dev, st);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to setup SPI offload\n");

	ret = ltc2378_pwm_get(st);
	if (ret)
		return ret;

	st->sample_freq_range[0] = 1; /* min */
	st->sample_freq_range[1] = 1; /* step */
	st->sample_freq_range[2] = st->info->max_sample_rate_hz; /* max */

	/*
	 * Start with a slower sampling rate so there is some room for
	 * adjusting the sampling frequency without hitting the maximum
	 * conversion rate.
	 */
	ret = ltc2378_update_conversion_rate(st, st->info->max_sample_rate_hz >> 4);
	if (ret)
		return dev_err_probe(dev, ret, "failed to sampling frequency\n");

	ret = ltc2378_prepare_offload_message(&spi->dev, st);
	if (ret)
		return dev_err_probe(dev, ret, "failed to optimize SPI message\n");

	return 0;
}
EXPORT_SYMBOL_NS_GPL(ltc2378_offload_buffer_setup, "IIO_LTC2378");

MODULE_IMPORT_NS("IIO_LTC2378");
