// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2026 Analog Devices, Inc.
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#include <linux/err.h>
#include <linux/iio/buffer.h>
#include <linux/iio/buffer-dmaengine.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/iio/types.h>
#include <linux/kstrtox.h>
#include <linux/limits.h>
#include <linux/linkage.h>
#include <linux/math.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/pwm.h>
#include <linux/spi/offload/consumer.h>
#include <linux/spi/offload/types.h>
#include <linux/time64.h>

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
	u64 offload_offset_ns;
	int ret;

	if (freq_Hz == 0)
		return -EINVAL;

	if (freq_Hz < 1 || freq_Hz > st->info->max_sample_rate_hz)
		return -ERANGE;

	/* Configure CNV PWM waveform */
	cnv_wf.period_length_ns = DIV_ROUND_CLOSEST(NSEC_PER_SEC, freq_Hz);

	/*
	 * Ensure CNV high time meets minimum requirement (20ns).
	 * The PWM hardware may round the duty cycle, so iterate
	 * until we get at least the minimum required high time.
	 */
	do {
		cnv_wf.duty_length_ns = target;
		ret = pwm_round_waveform_might_sleep(st->cnv_trigger, &cnv_wf);
		if (ret)
			return ret;
		target += 10;  /* Increment by PWM duty cycle period */
	} while (cnv_wf.duty_length_ns < LTC2378_TCNV_HIGH_NS);

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
	do {
		config->periodic.offset_ns = offload_offset_ns;
		ret = spi_offload_trigger_validate(st->offload_trigger, config);
		if (ret)
			return ret;
		offload_offset_ns += 10;
	} while (config->periodic.offset_ns < min_read_offset);

	st->cnv_wf = cnv_wf;
	st->cnv_Hz = DIV_ROUND_CLOSEST_ULL(HZ_PER_GHZ, cnv_wf.period_length_ns);

	return 0;
}

static ssize_t sampling_frequency_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct ltc2378_state *st = iio_priv(dev_to_iio_dev(dev));

	return sysfs_emit(buf, "%u\n", st->cnv_Hz);
}

static ssize_t sampling_frequency_store(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t len)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct ltc2378_state *st = iio_priv(indio_dev);
	unsigned int val;
	int ret;

	if (!iio_device_claim_direct(indio_dev))
		return -EBUSY;

	ret = kstrtouint(buf, 10, &val);
	if (ret)
		goto out_store;

	ret = ltc2378_update_conversion_rate(st, val);

out_store:
	iio_device_release_direct(indio_dev);
	return ret ?: len;
}

static IIO_DEVICE_ATTR_RW(sampling_frequency, 0);

static ssize_t sampling_frequency_available_show(struct device *dev,
						 struct device_attribute *attr,
						 char *buf)
{
	struct iio_dev *indio_dev = dev_to_iio_dev(dev);
	struct ltc2378_state *st = iio_priv(indio_dev);

	return sysfs_emit(buf, "[%u %u %u]\n",
			  1, 1, st->info->max_sample_rate_hz);
}

static IIO_DEVICE_ATTR_RO(sampling_frequency_available, 0);

static struct attribute *ltc2378_offload_attributes[] = {
	&iio_dev_attr_sampling_frequency.dev_attr.attr,
	&iio_dev_attr_sampling_frequency_available.dev_attr.attr,
	NULL,
};

const struct attribute_group ltc2378_offload_attribute_group = {
	.attrs = ltc2378_offload_attributes,
};
EXPORT_SYMBOL_NS_GPL(ltc2378_offload_attribute_group, "IIO_LTC2378");

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
	if (ret && ret != -ENODEV)
		return dev_err_probe(dev, ret, "failed to get offload\n");

	ret = ltc2378_spi_offload_setup(indio_dev, st);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to setup SPI offload\n");

	ret = ltc2378_pwm_get(st);
	if (ret)
		return ret;

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
