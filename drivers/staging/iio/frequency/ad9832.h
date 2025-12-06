/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * AD9832 SPI DDS driver
 *
 * Copyright 2011 Analog Devices Inc.
 */
#ifndef IIO_DDS_AD9832_H_
#define IIO_DDS_AD9832_H_
#include <linux/types.h>

/*
 * TODO: struct ad9832_platform_data needs to go into include/linux/iio
 */

/**
 * struct ad9832_platform_data - platform specific information
 * @freq0:		power up freq0 tuning word in Hz
 * @freq1:		power up freq1 tuning word in Hz
 * @phase0:		power up phase0 value [0..4095] correlates with 0..2PI
 * @phase1:		power up phase1 value [0..4095] correlates with 0..2PI
 * @phase2:		power up phase2 value [0..4095] correlates with 0..2PI
 * @phase3:		power up phase3 value [0..4095] correlates with 0..2PI
 */

struct ad9832_platform_data {
	u32 freq0;
	u32 freq1;
	u16 phase0;
	u16 phase1;
	u16 phase2;
	u16 phase3;
};

#endif /* IIO_DDS_AD9832_H_ */
