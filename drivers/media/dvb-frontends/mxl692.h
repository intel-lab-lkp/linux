/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver for the MaxLinear MxL69x family of tuners/demods
 *
 * Copyright (C) 2020 Brad Love <brad@nextdimension.cc>
 *
 * based on code:
 * Copyright (c) 2016 MaxLinear, Inc. All rights reserved
 * which was released under GPL V2
 */

#ifndef _MXL692_H_
#define _MXL692_H_

#include <media/dvb_frontend.h>

#define MXL692_FIRMWARE "dvb-demod-mxl692.fw"

struct mxl692_mpeg_pad_drv_config {
	u8 pad_drv_mpeg_syn;
	u8 pad_drv_mpeg_dat;
	u8 pad_drv_mpeg_val;
	u8 pad_drv_mpeg_clk;
};

struct mxl692_config {
	unsigned char  id;
	u8 i2c_addr;

	/* xtal config */
	u8 xtal_calibration_enable;
	u8 xtal_sharing_enable;

	/* mpeg config */
	u8 mpeg_parallel;
	u8 mpeg_sync_pulse_width;
	u8 mpeg3wire_mode_enable;
	u8 mpeg_clk_freq;
	struct mxl692_mpeg_pad_drv_config mpeg_pad_drv;

	/*
	 * frontend
	 * returned by driver
	 */
	struct dvb_frontend **fe;
};

#endif /* _MXL692_H_ */
