/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Analog Devices LTC2378 and similar ADCs common definitions and properties
 * Copyright (C) 2026 Analog Devices, Inc.
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#ifndef __DRIVERS_IIO_ADC_LTC2378_H__
#define __DRIVERS_IIO_ADC_LTC2378_H__

#include <linux/iio/iio.h>
#include <linux/spi/spi.h>
#include <linux/types.h>
#include <linux/units.h>

#define LTC2378_TDSDOBUSYL_NS		5
#define LTC2378_TBUSYLH_NS		13
#define LTC2378_TCNV_HIGH_NS		20

struct ltc2378_chip_info {
	const char *name;
	int resolution;
	const char out_format;
};

struct ltc2378_state {
	const struct ltc2378_chip_info *info;
	struct gpio_desc *cnv_gpio;
	struct spi_device *spi;
	struct spi_transfer xfer;
	int ref_uV;

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

#endif /* __DRIVERS_IIO_ADC_LTC2378_H__ */
