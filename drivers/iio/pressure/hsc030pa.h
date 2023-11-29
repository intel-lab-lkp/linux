/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Honeywell TruStability HSC Series pressure/temperature sensor
 *
 * Copyright (c) 2023 Petre Rodan <petre.rodan@subdimension.ro>
 */

#ifndef _HSC030PA_H
#define _HSC030PA_H

#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/types.h>

/*
 * get all conversions (4 bytes) in one go
 * since transfers are not address-based
*/
#define HSC_REG_MEASUREMENT_RD_SIZE 4

struct device;

struct iio_chan_spec;
struct iio_dev;

struct hsc_data;
struct hsc_chip_data;

typedef int (*hsc_recv_fn)(struct hsc_data *);

/**
 * struct hsc_data
 * @client: either i2c or spi kernel interface struct for current dev
 * @chip: structure containing chip's channel properties
 * @lock: lock protecting chip reads
 * @recv: function that implements the chip reads
 * @is_valid: false if last transfer has failed
 * @buffer: raw conversion data
 * @pmin: minimum measurable pressure limit
 * @pmax: maximum measurable pressure limit
 * @outmin: minimum raw pressure in counts (based on transfer function)
 * @outmax: maximum raw pressure in counts (based on transfer function)
 * @function: transfer function
 * @p_scale: pressure scale
 * @p_scale_dec: pressure scale, decimal places
 * @p_offset: pressure offset
 * @p_offset_dec: pressure offset, decimal places
 */
struct hsc_data {
	void *client;
	const struct hsc_chip_data *chip;
	struct mutex lock;
	hsc_recv_fn recv_cb;
	bool is_valid;
	u8 buffer[HSC_REG_MEASUREMENT_RD_SIZE] __aligned(IIO_DMA_MINALIGN);
	s32 pmin;
	s32 pmax;
	u32 outmin;
	u32 outmax;
	u32 function;
	s64 p_scale;
	s32 p_scale_dec;
	s64 p_offset;
	s32 p_offset_dec;
};

struct hsc_chip_data {
	bool (*valid)(struct hsc_data *data);
	const struct iio_chan_spec *channels;
	u8 num_channels;
};

enum hsc_func_id {
	HSC_FUNCTION_A,
	HSC_FUNCTION_B,
	HSC_FUNCTION_C,
	HSC_FUNCTION_F,
};

int hsc_common_probe(struct device *dev, void *client,
	hsc_recv_fn recv_fn, const char *name);

#endif
