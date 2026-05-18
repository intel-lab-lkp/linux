/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Analog Devices LTC2378 and similar ADCs common definitions and properties
 * Copyright (C) 2026 Analog Devices, Inc.
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#ifndef __DRIVERS_IIO_ADC_LTC2378_H__
#define __DRIVERS_IIO_ADC_LTC2378_H__

#include <linux/iio/iio.h>
#ifdef CONFIG_LTC2378_OFFLOAD_BUFFER
#include <linux/pwm.h>
#endif
#include <linux/spi/spi.h>
#ifdef CONFIG_LTC2378_OFFLOAD_BUFFER
#include <linux/spi/offload/consumer.h>
#include <linux/spi/offload/types.h>
#endif
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
	const char out_format;
};

struct ltc2378_state {
	const struct ltc2378_chip_info *info;
	struct gpio_desc *cnv_gpio;
	struct spi_device *spi;
	struct spi_transfer xfer;
	int ref_uV;
#ifdef CONFIG_LTC2378_OFFLOAD_BUFFER
	unsigned int cnv_Hz;
	struct pwm_waveform cnv_wf;
	struct spi_offload *offload;
	struct spi_offload_trigger *offload_trigger;
	struct spi_message offload_msg;
	struct spi_transfer offload_xfer;
	struct spi_offload_trigger_config offload_trigger_config;
	struct pwm_device *cnv_trigger;
#endif

	/*
	 * DMA (thus cache coherency maintenance) requires the
	 * transfer buffers to live in their own cache lines.
	 */
	struct {
		union {
			__be16 sample_buf16_be;
			__be32 sample_buf32_be;
			u16 sample_buf16;
			u32 sample_buf32;
		} data;
		aligned_s64 timestamp;
	} scan __aligned(IIO_DMA_MINALIGN);
};

#ifdef CONFIG_LTC2378_OFFLOAD_BUFFER
extern const struct attribute_group ltc2378_offload_attribute_group;
#endif

#ifdef CONFIG_LTC2378_OFFLOAD_BUFFER
int ltc2378_offload_buffer_setup(struct iio_dev *indio_dev, struct spi_device *spi);
#else
static inline int ltc2378_offload_buffer_setup(struct iio_dev *indio_dev,
					       struct spi_device *spi)
{
	might_sleep();
	return -ENODEV;
}
#endif

#endif /* __DRIVERS_IIO_ADC_LTC2378_H__ */
