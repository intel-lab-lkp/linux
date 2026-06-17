/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Analog Devices LTC2378 and similar ADCs common definitions and properties
 * Copyright (C) 2026 Analog Devices, Inc.
 * Author: Marcelo Schmitt <marcelo.schmitt@analog.com>
 */

#ifndef __DRIVERS_IIO_ADC_LTC2378_H__
#define __DRIVERS_IIO_ADC_LTC2378_H__

#include <linux/errno.h>
#include <linux/iio/iio.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/pwm.h>
#include <linux/spi/spi.h>
#include <linux/spi/offload/consumer.h>
#include <linux/spi/offload/types.h>
#include <linux/types.h>
#include <linux/units.h>

#define LTC2378_TDSDOBUSYL_NS		5
#define LTC2378_TBUSYLH_NS		13
#define LTC2378_TCNV_HIGH_NS		20

struct ltc2378_state;

struct ltc2378_chip_info {
	const char *name;
	int resolution;
	unsigned int max_sample_rate_hz;
	unsigned int tconv_ns;
	bool bipolar;
};

/**
 * struct ltc2378_ops: Setup specific procedures for ltc2378 devices.
 * @ltc2378_buffer_setup: Custom buffer setup implementation.
 */
struct ltc2378_ops {
	int (*buffer_setup)(struct iio_dev *indio_dev, struct ltc2378_state *st);
};

struct ltc2378_state {
	const struct ltc2378_chip_info *info;
	struct gpio_desc *cnv_gpio;
	struct spi_device *spi;
	struct spi_transfer xfer;
	unsigned int num_iio_chans;
	struct iio_chan_spec chans[2]; /* 1 physical chan + 1 timestamp chan */
	int ref_uV;
	const struct ltc2378_ops *ops;
	unsigned int cnv_Hz;
	struct pwm_waveform cnv_wf;
	struct spi_offload *offload;
	struct spi_offload_trigger *offload_trigger;
	struct spi_message offload_msg;
	struct spi_transfer offload_xfer;
	struct spi_offload_trigger_config offload_trigger_config;
	struct pwm_device *cnv_trigger;

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

int ltc2378_lib_buffer_setup(struct iio_dev *indio_dev, struct ltc2378_state *st);

#define __ltc2378_set_offload_ops(st) ltc2378_set_offload_ops((st))

#define __ltc2378_set_triggered_buf_ops(st) ltc2378_set_triggered_buf_ops((st))

#ifdef CONFIG_LTC2378_LIB_OFFLOAD_BUFFER

int ltc2378_set_offload_ops(struct ltc2378_state *st);

#else

static inline int ltc2378_set_offload_ops(struct ltc2378_state *st)
{
	return -EOPNOTSUPP;
}

#endif /* CONFIG_LTC2378_LIB_OFFLOAD_BUFFER */

#ifdef CONFIG_LTC2378_LIB_TRIGGERED_BUFFER

int ltc2378_set_triggered_buf_ops(struct ltc2378_state *st);

#else

static inline int ltc2378_set_triggered_buf_ops(struct ltc2378_state *st)
{
	return -EOPNOTSUPP;
}

#endif /* CONFIG_LTC2378_LIB_TRIGGERED_BUFFER */

#endif /* __DRIVERS_IIO_ADC_LTC2378_H__ */
