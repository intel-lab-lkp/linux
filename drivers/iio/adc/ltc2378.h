/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Analog Devices LTC2378 and similar ADCs common definitions and properties
 * Copyright (C) 2026 Analog Devices, Inc.
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#ifndef __DRIVERS_IIO_ADC_LTC2378_H__
#define __DRIVERS_IIO_ADC_LTC2378_H__

#include <linux/iio/iio.h>
#include <linux/pwm.h>
#include <linux/spi/spi.h>
#include <linux/spi/offload/consumer.h>
#include <linux/spi/offload/types.h>
#include <linux/types.h>
#include <linux/units.h>

#define LTC2378_TDSDOBUSYL_NS		5
#define LTC2378_TBUSYLH_NS		13
#define LTC2378_TCNV_HIGH_NS		20

struct ltc2378_chip_info {
	const char *name;
	int resolution;
	unsigned int max_sample_rate_hz;
	unsigned int tconv_ns;
	bool twos_comp; /* Output code is 2's complement or straight binary */
};

struct ltc2378_state {
	const struct ltc2378_chip_info *info;
	struct gpio_desc *cnv_gpio;
	struct spi_device *spi;
	struct spi_transfer xfer;
	struct iio_chan_spec chans[2]; /* 1 physical chan + 1 timestamp chan */
	int ref_uV;
	unsigned int cnv_Hz;
	struct pwm_waveform cnv_wf;
	struct spi_offload *offload;
	struct spi_offload_trigger *offload_trigger;
	struct spi_message offload_msg;
	struct spi_transfer offload_xfer;
	struct spi_offload_trigger_config offload_trigger_config;
	struct pwm_device *cnv_trigger;
	int sample_freq_range[3];

	/*
	 * DMA (thus cache coherency maintenance) requires the
	 * transfer buffers to live in their own cache lines.
	 */
	struct {
		union {
			u16 sample_buf16;
			u32 sample_buf32;
		} data;
		aligned_s64 timestamp;
	} scan __aligned(IIO_DMA_MINALIGN);
};

#define LTC2378_WRITE_RAW LTC2378_WRITE_RAW_PTR

#define LTC2378_READ_AVAIL LTC2378_READ_AVAIL_PTR

#ifdef CONFIG_LTC2378_OFFLOAD_BUFFER

int ltc2378_offload_buffer_setup(struct iio_dev *indio_dev, struct spi_device *spi);

int ltc2378_get_sampling_frequency(struct ltc2378_state *st, int *val);

int ltc2378_set_sampling_frequency(struct ltc2378_state *st, int freq_Hz);

int ltc2378_write_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
		      int val, int val2, long mask);

int ltc2378_read_avail(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
		       const int **vals, int *type, int *length, long mask);

#define LTC2378_WRITE_RAW_PTR (&ltc2378_write_raw)

#define LTC2378_READ_AVAIL_PTR (&ltc2378_read_avail)

#else /* CONFIG_IIO_LTC2378_LIB_OFFLOAD_BUFFER */

#define LTC2378_WRITE_RAW_PTR (NULL)

#define LTC2378_READ_AVAIL_PTR (NULL)

static inline int ltc2378_offload_buffer_setup(struct iio_dev *indio_dev,
					       struct spi_device *spi)
{
	return -ENODEV;
}

int ltc2378_get_sampling_frequency(struct ltc2378_state *st, int *val)
{
	return -EOPNOTSUPP;
}

int ltc2378_set_sampling_frequency(struct ltc2378_state *st, int freq_Hz);
{
	return -EOPNOTSUPP;
}

int ltc2378_write_raw(struct iio_dev *indio_dev, struct iio_chan_spec const *chan,
		      int val, int val2, long mask)
{
	return -EOPNOTSUPP;
}

#endif

#endif /* __DRIVERS_IIO_ADC_LTC2378_H__ */
