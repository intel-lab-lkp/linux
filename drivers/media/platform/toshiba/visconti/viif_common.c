// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#include <linux/delay.h>
#include <media/mipi-csi2.h>
#include <media/v4l2-common.h>

#include "viif.h"
#include "viif_common.h"
#include "viif_regs.h"

/*=============================================*/
/* Low level guards for registers */
/*=============================================*/

#define VIIF_L1_CRGBF_R_START_ADDR_LIMIT 0x0200U
#define VIIF_L1_CRGBF_R_END_ADDR_LIMIT	 0x10BFU
#define VIIF_L2_CRGBF_R_START_ADDR_LIMIT 0x1CU
#define VIIF_L2_CRGBF_R_END_ADDR_LIMIT	 0x1FU

/**
 * hwd_viif_main_mask_vlatch() - Control Vlatch mask of MAIN unit
 *
 * @viif_dev: the VIIF device
 * @enable: true to enable Vlatch mask of MAIN unit, false to disable
 */
static void hwd_viif_main_mask_vlatch(struct viif_device *viif_dev, bool enable)
{
	u32 val = enable ? MASK_IPORTM_VLATCH : 0;

	viif_capture_write(viif_dev, REG_IPORTM0_LD, val);
	viif_capture_write(viif_dev, REG_IPORTM1_LD, val);
}

/**
 * hwd_viif_isp_set_regbuf_auto_transmission() - Set register buffer auto transmission
 *
 * @viif_dev: the VIIF device
 */
void hwd_viif_isp_set_regbuf_auto_transmission(struct viif_device *viif_dev)
{
	/* Set parameters for auto read transmission of register buffer */
	viif_capture_write(viif_dev, REG_L1_CRGBF_TRN_A_CONF, 0);
	viif_capture_write(viif_dev, REG_L2_CRGBF_TRN_A_CONF, 0);
	viif_capture_write(viif_dev, REG_L1_CRGBF_TRN_RBADDR, VIIF_L1_CRGBF_R_START_ADDR_LIMIT);
	viif_capture_write(viif_dev, REG_L1_CRGBF_TRN_READDR, VIIF_L1_CRGBF_R_END_ADDR_LIMIT);
	viif_capture_write(viif_dev, REG_L2_CRGBF_TRN_RBADDR, VIIF_L2_CRGBF_R_START_ADDR_LIMIT);
	viif_capture_write(viif_dev, REG_L2_CRGBF_TRN_READDR, VIIF_L2_CRGBF_R_END_ADDR_LIMIT);
	viif_capture_write(viif_dev, REG_L2_CRGBF_TRN_A_CONF, VAL_L2_CRGBF_TRN_AUTO_READ_BANK0);
	viif_capture_write(viif_dev, REG_L1_CRGBF_TRN_A_CONF, VAL_L1_CRGBF_TRN_AUTO_READ_BANK0);
}

/**
 * hwd_viif_isp_disable_regbuf_auto_transmission() - Disable register buffer auto transmission
 *
 * @viif_dev: the VIIF device
 */
void hwd_viif_isp_disable_regbuf_auto_transmission(struct viif_device *viif_dev)
{
	viif_capture_write(viif_dev, REG_L1_CRGBF_TRN_A_CONF, 0);
	viif_capture_write(viif_dev, REG_L2_CRGBF_TRN_A_CONF, 0);
}

/**
 * hwd_viif_isp_guard_start() - stop register auto update
 *
 * @viif_dev: the VIIF device
 *
 * This function call stops update of some hardware registers
 * while the manual setup of VIIF, L1ISP registers is in progress.
 *
 * * regbuf control: load/store HW register (settings, status) values to backup SRAM.
 * * vlatch control: copy timer-counter register value to status register.
 */
void hwd_viif_isp_guard_start(struct viif_device *viif_dev)
{
	hwd_viif_isp_disable_regbuf_auto_transmission(viif_dev);
	ndelay(500);
	hwd_viif_main_mask_vlatch(viif_dev, true);
}

/**
 * hwd_viif_isp_guard_end() - restart register auto update
 *
 * @viif_dev: the VIIF device
 *
 * see also hwd_viif_isp_guard_start().
 */
void hwd_viif_isp_guard_end(struct viif_device *viif_dev)
{
	hwd_viif_main_mask_vlatch(viif_dev, false);
	hwd_viif_isp_set_regbuf_auto_transmission(viif_dev);
}

/*=============================================*/
/* supported Visual formats */
/*=============================================*/
static const struct viif_mbus_format mbus_formats[] = {
	{ .code = MEDIA_BUS_FMT_RGB888_1X24,
	  .bpp = 24,
	  .rgb_out = true,
	  .mipi_dt = MIPI_CSI2_DT_RGB888 },
	{ .code = MEDIA_BUS_FMT_UYVY8_1X16,
	  .bpp = 16,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_YUV422_8B },
	{ .code = MEDIA_BUS_FMT_UYVY10_1X20,
	  .bpp = 20,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_YUV422_10B },
	{ .code = MEDIA_BUS_FMT_RGB565_1X16,
	  .bpp = 16,
	  .rgb_out = true,
	  .mipi_dt = MIPI_CSI2_DT_RGB565 },
	{ .code = MEDIA_BUS_FMT_SBGGR8_1X8,
	  .bpp = 8,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW8 },
	{ .code = MEDIA_BUS_FMT_SGBRG8_1X8,
	  .bpp = 8,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW8 },
	{ .code = MEDIA_BUS_FMT_SGRBG8_1X8,
	  .bpp = 8,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW8 },
	{ .code = MEDIA_BUS_FMT_SRGGB8_1X8,
	  .bpp = 8,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW8 },
	{ .code = MEDIA_BUS_FMT_SRGGB10_1X10,
	  .bpp = 10,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW10 },
	{ .code = MEDIA_BUS_FMT_SGRBG10_1X10,
	  .bpp = 10,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW10 },
	{ .code = MEDIA_BUS_FMT_SGBRG10_1X10,
	  .bpp = 10,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW10 },
	{ .code = MEDIA_BUS_FMT_SBGGR10_1X10,
	  .bpp = 10,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW10 },
	{ .code = MEDIA_BUS_FMT_SRGGB12_1X12,
	  .bpp = 12,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW12 },
	{ .code = MEDIA_BUS_FMT_SGRBG12_1X12,
	  .bpp = 12,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW12 },
	{ .code = MEDIA_BUS_FMT_SGBRG12_1X12,
	  .bpp = 12,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW12 },
	{ .code = MEDIA_BUS_FMT_SBGGR12_1X12,
	  .bpp = 12,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW12 },
	{ .code = MEDIA_BUS_FMT_SRGGB14_1X14,
	  .bpp = 14,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW14 },
	{ .code = MEDIA_BUS_FMT_SGRBG14_1X14,
	  .bpp = 14,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW14 },
	{ .code = MEDIA_BUS_FMT_SGBRG14_1X14,
	  .bpp = 14,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW14 },
	{ .code = MEDIA_BUS_FMT_SBGGR14_1X14,
	  .bpp = 14,
	  .rgb_out = false,
	  .mipi_dt = MIPI_CSI2_DT_RAW14 },
};

const struct viif_mbus_format *viif_mbus_format_from_code(unsigned int mbus_code)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(mbus_formats); i++)
		if (mbus_formats[i].code == mbus_code)
			return &mbus_formats[i];

	return NULL;
}

const struct viif_mbus_format *viif_mbus_format_nth(unsigned int n)
{
	return (n < ARRAY_SIZE(mbus_formats)) ? &mbus_formats[n] : NULL;
}
