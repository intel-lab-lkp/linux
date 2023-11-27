/* SPDX-License-Identifier: GPL-2.0 */
/*
 * JH7110 Cadence DSI
 *
 * Copyright (C) 2022-2023 StarFive Technology Co., Ltd.
 * Author: keith.zhao <keith.zhao@starfivetech.com>
 */

#ifndef __CDNS_DSI_JH7110_H__
#define __CDNS_DSI_JH7110_H__

#include "cdns-dsi-core.h"

#define DSI_HSS     4
#define DSI_HSE     4
#define DSI_VSS     4
#define DSI_VSE     4
#define DSI_HDR     4
#define DSI_CRC     2

/*
 * HBP should be reduced by 12 to account for the header
 * and footer on the blanking packet (6 bytes) plus
 * the header/footer on the active data packet (6 bytes)
 */
#define DSI_HBP_FRAME_OVERHEAD          12

/*
 * HSA should be reduced by 14 bytes to account for the HSS short packet (4 bytes),
 * the long blanking packet
 * header and CRC footer (4+2 bytes)
 * and the HSE short packet (4 bytes)
 */
#define DSI_HSA_FRAME_OVERHEAD          14

/*
 * HFP should be reduced by 6 bytes to account
 * for the long packet header and CRC footer
 */
#define DSI_HFP_FRAME_OVERHEAD          6

#define DSI_HSS_VSS_VSE_FRAME_OVERHEAD  4
#define DSI_BLANKING_FRAME_OVERHEAD     6
#define DSI_NULL_FRAME_OVERHEAD         6
#define DSI_EOT_PKT_SIZE                4

#define DSI_REG_HSA_LIMIT				(BIT(10) - 1 + DSI_HSA_FRAME_OVERHEAD)
#define DSI_REG_HBP_LIMIT				(BIT(16) - 1 + DSI_HBP_FRAME_OVERHEAD)
#define DSI_REG_HFP_LIMIT				(BIT(11) - 1 + DSI_HFP_FRAME_OVERHEAD)
#define DSI_REG_VSA_LIMIT				(BIT(6) - 1)
#define DSI_REG_VBP_LIMIT				(BIT(6) - 1)
#define DSI_REG_VFP_LIMIT				(BIT(8) - 1)

struct dsi_regval_t {
	/*
	 * Active line Pulse Mode:
	 * |____hsync_____|____hbp_____|________________hact_______________|_______hfp________|
	 * |_HSS_HSA_HSE__|HDR_HBP_CRC_|HDR_____________HACT____________CRC|HDR____HFP_____CRC|_HSS_
	 * Active line Event Mode:
	 * |____hsync_____|____hbp_____|________________hact_______________|_______hfp________|
	 * |_HSS_|HDR___HSA+HBP____CRC_|HDR_____________HACT____________CRC|HDR____HFP_____CRC|_HSS_
	 */
	int hsa_length, hbp_length, hact_length, hfp_length; // register field value

	/*
	 * Pulse mode Blank line
	 * |______________|_______________________blkline_pulse_pck___________________________|
	 * |_HSS_HSA_HSE__|HDR_____________________________________________________________CRC|_HSS_
	 */
	unsigned int blkline_pulse_pck;

	// BLKEOL_PCK: packet length (in byte) on end of line if burst mode (reg_blkeol_mode = 0b0x)
	unsigned int blkeol_pck;

	/*
	 * Event mode Blank line
	 * |_____|________________________blkline_event_pck___________________________|
	 * |_HSS_|HDR______________________________________________________________CRC|_HSS_
	 */
	unsigned int blkline_event_pck;

	/* BLKEOL_DURATION: specify the duration in clock cycles
	 * of the BLLP period (used for burst mode)
	 * unsigned int blkeol_duration;

	 * REG_LINE_DURATION: duration -in clock cycles - of the blanking area for VSA/VBP
	 * and VFP lines - considered when reg_blkline_mode = 1b1x

	 * Pulse mode Blank LP line EOT disabled
	 * |______________|_______________________reg_line_duration_______________________|
	 * |_HSS_HSA_HSE__|__________________________LP___________________________________|_HSS_
	 * Pulse mode Blank LP line EOT enabled
	 * |______________|___|___________________reg_line_duration_______________________|
	 * |_HSS_HSA_HSE__|EoT|______________________LP___________________________________|_HSS_
	 * Event mode Blank LP line EOT enabled
	 * |________|_____________________________reg_line_duration_______________________|
	 * |_HSS|EoT|________________________________LP___________________________________|_HSS_
	 */
	unsigned int reg_line_duration;
};

enum dsi_video_mode {
	DSI_Video_Burst,
	DSI_Video_NonBurstPulse,
	DSI_Video_NonBurstEvent,
};

struct dsi_params {
	unsigned int	dlanes;
	unsigned long	bitrate;
	unsigned int	hsa;
	unsigned int	hbp;
	unsigned int	hfp;
	unsigned int	hact;
	unsigned int	vsa;
	unsigned int	vbp;
	unsigned int	vfp;
	unsigned int	vact;
};

struct dsi_metrics {
	unsigned int bytes_one_line;
	unsigned long byteclock;

	//period and time in ps
	unsigned long byteclock_period;
	unsigned long hsa_hbp_time;
	unsigned long hact_time;
	unsigned long hfp_time;
	unsigned long one_line_time;
};

struct dpi_params {
	unsigned int bpp;
	unsigned long   pixelclock;
	unsigned int hactive;
	unsigned int hfront_porch;
	unsigned int hback_porch;
	unsigned int hsync_len;
	unsigned int vactive;
	unsigned int vfront_porch;
	unsigned int vback_porch;
	unsigned int vsync_len;
};

struct dpi_metrics {
	unsigned int pixels_one_line;
	double fps;

	unsigned long pixelclock_period;
	unsigned long hact_time;
	unsigned long hfp_time;
	unsigned long hbp_time;
	unsigned long hsa_time;
	unsigned long one_line_time;
};

struct dsi_hblk_ratio {
	int den;
	int hsa_num;
	int hbp_num;
	int hfp_num;
};

struct calc_ctrl {
	unsigned int hactive;
	unsigned int vactive;
	unsigned int bpp;
	unsigned int fps;
	double   fps_tolerance;

	unsigned int dlanes;
	unsigned long bitrate_alignment;
	unsigned long max_bitrate;
	unsigned long min_bitrate;
	unsigned int dsi_video_mode;

	unsigned int r_hsa;
	unsigned int r_hbp;
	unsigned int r_hfp;
	unsigned int line_time_tolerance;
};

extern const struct cdns_dsi_platform_ops dsi_ti_jh7110_ops;

#endif /* !__CDNS_DSI_JH7110_H__ */
