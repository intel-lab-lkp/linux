/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */
/*
 * Copyright (c) 2024 Robert Bosch GmbH.
 */
#ifndef _SMI240_H
#define _SMI240_H

#include <linux/iio/iio.h>

struct regmap;
struct device;

enum capture_mode { SMI240_CAPTURE_OFF = 0, SMI240_CAPTURE_ON = 1 };

struct smi240_data {
	struct regmap *regmap;
	u16 accel_filter_freq;
	u16 anglvel_filter_freq;
	u8 bite_reps;
	enum capture_mode capture;
	/*
	 * Ensure natural alignment for timestamp if present.
	 * Channel size: 2 bytes.
	 * Max length needed: 2 * 3 channels + temp channel + 2 bytes padding + 8 byte ts.
	 * If fewer channels are enabled, less space may be needed, as
	 * long as the timestamp is still aligned to 8 bytes.
	 */
	s16 buf[12] __aligned(8);

	__be32 spi_buf __aligned(IIO_DMA_MINALIGN);
};

int smi240_core_probe(struct device *dev, struct regmap *regmap);

#endif /* _SMI240_H */
