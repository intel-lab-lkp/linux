// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * MPRLS0025PA - Honeywell MicroPressure pressure sensor series driver
 *
 * Copyright (c) Andreas Klinger <ak@it-klinger.de>
 *
 * Data sheet:
 *  https://prod-edam.honeywell.com/content/dam/honeywell-edam/sps/siot/en-us/products/sensors/pressure-sensors/board-mount-pressure-sensors/micropressure-mpr-series/documents/sps-siot-mpr-series-datasheet-32332628-ciid-172626.pdf
 */

#ifndef _MPRLS0025PA_H
#define _MPRLS0025PA_H

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/stddef.h>
#include <linux/types.h>

#define MPR_MEASUREMENT_RD_SIZE 4
#define MPR_CMD_NOP      0xf0
#define MPR_CMD_SYNC     0xaa
#define MPR_PKT_NOP_LEN  MPR_MEASUREMENT_RD_SIZE
#define MPR_PKT_SYNC_LEN 3

/* bits in i2c status byte */
#define MPR_I2C_POWER	BIT(6)	/* device is powered */
#define MPR_I2C_BUSY	BIT(5)	/* device is busy */
#define MPR_I2C_MEMORY	BIT(2)	/* integrity test passed */
#define MPR_I2C_MATH	BIT(0)	/* internal math saturation */

struct device;

struct iio_chan_spec;
struct iio_dev;

struct mpr_data;

typedef int (*mpr_xfer_fn)(struct mpr_data *, const u8, const u8);

enum mpr_func_id {
	MPR_FUNCTION_A,
	MPR_FUNCTION_B,
	MPR_FUNCTION_C,
};

/**
 * struct mpr_chan
 * @pres: pressure value
 * @ts: timestamp
 */
struct mpr_chan {
	s32 pres;
	s64 ts;
};

/**
 * struct mpr_data
 * @dev: current device structure
 * @read_cb: function that implements the sensor reads
 * @write_cb: function that implements the sensor writes
 * @pmin: minimal pressure in pascal
 * @pmax: maximal pressure in pascal
 * @function: transfer function
 * @outmin: minimum raw pressure in counts (based on transfer function)
 * @outmax: maximum raw pressure in counts (based on transfer function)
 * @scale: pressure scale
 * @scale2: pressure scale, decimal places
 * @offset: pressure offset
 * @offset2: pressure offset, decimal places
 * @gpiod_reset: reset
 * @irq: end of conversion irq. used to distinguish between irq mode and
 *       reading in a loop until data is ready
 * @completion: handshake from irq to read
 * @chan: channel values for buffered mode
 * @buffer: raw conversion data
 */
struct mpr_data {
	struct device		*dev;
	mpr_xfer_fn		read_cb;
	mpr_xfer_fn		write_cb;
	struct mutex		lock;
	u32			pmin;
	u32			pmax;
	enum mpr_func_id	function;
	u32			outmin;
	u32			outmax;
	int			scale;
	int			scale_dec;
	int			offset;
	int			offset_dec;
	struct gpio_desc	*gpiod_reset;
	int			irq;
	struct completion	completion;
	struct mpr_chan		chan;
	u8			buffer[MPR_MEASUREMENT_RD_SIZE] __aligned(IIO_DMA_MINALIGN);
};

int mpr_common_probe(struct device *dev, mpr_xfer_fn read, mpr_xfer_fn write,
		     int irq);

#endif
