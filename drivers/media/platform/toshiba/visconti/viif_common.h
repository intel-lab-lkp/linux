/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#ifndef VIIF_COMMON_H
#define VIIF_COMMON_H

#include "viif.h"

/**
 * struct viif_mbus_format - description of supported input format
 *
 * @code: V4L2 media bus format (coming from image sensor)
 * @bpp: bits per pixel
 * @mipi_dt: MIPI Datatype corresponding to code
 * @rgb_out:
 * * True: L1ISP output is RGB format
 * * False: L1ISP output is YUV format
 */
struct viif_mbus_format {
	unsigned int code;
	unsigned int bpp;
	unsigned int mipi_dt;
	bool rgb_out;
};

void hwd_viif_isp_set_regbuf_auto_transmission(struct viif_device *viif_dev);
void hwd_viif_isp_disable_regbuf_auto_transmission(struct viif_device *viif_dev);
void hwd_viif_isp_guard_start(struct viif_device *viif_dev);
void hwd_viif_isp_guard_end(struct viif_device *viif_dev);

const struct viif_mbus_format *viif_mbus_format_from_code(unsigned int mbus_code);
const struct viif_mbus_format *viif_mbus_format_nth(unsigned int n);

#endif /* VIIF_COMMON_H */
