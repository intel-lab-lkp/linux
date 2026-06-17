/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Analog Devices LTC2378 and similar ADCs common definitions and properties
 * Copyright (C) 2026 Analog Devices, Inc.
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#ifndef __DRIVERS_IIO_ADC_LTC2378_H__
#define __DRIVERS_IIO_ADC_LTC2378_H__

#include <linux/iio/iio.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>
#include <linux/types.h>
#include <linux/units.h>

#define LTC2378_TDSDOBUSYL_NS		5
#define LTC2378_TBUSYLH_NS		13
#define LTC2378_TCNV_HIGH_NS		20

struct ltc2378_chip_info {
	const char *name;
	int resolution;
	bool bipolar;
};

struct ltc2378_state {
	const struct ltc2378_chip_info *info;
	struct gpio_desc *cnv_gpio;
	struct spi_device *spi;
	struct spi_transfer xfer;
	unsigned int num_iio_chans;
	struct iio_chan_spec chans[2]; /* 1 physical chan + 1 timestamp chan */
	int ref_uV;

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

static inline int ltc2378_convert_and_acquire(struct ltc2378_state *st)
{
	int ret;

	/* Cause a rising edge of CNV to initiate a new ADC conversion */
	gpiod_set_value_cansleep(st->cnv_gpio, 1);
	fsleep(4);
	ret = spi_sync_transfer(st->spi, &st->xfer, 1);
	gpiod_set_value_cansleep(st->cnv_gpio, 0);

	return ret;
}

#endif /* __DRIVERS_IIO_ADC_LTC2378_H__ */
