// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/* Toshiba Visconti Video Capture Support
 *
 * (C) Copyright 2023 TOSHIBA CORPORATION
 * (C) Copyright 2023 Toshiba Electronic Devices & Storage Corporation
 */

#include <linux/delay.h>
#include <media/mipi-csi2.h>
#include <media/v4l2-common.h>
#include <media/v4l2-subdev.h>

#include "viif.h"
#include "viif_common.h"
#include "viif_controls.h"
#include "viif_isp.h"
#include "viif_regs.h"

/* disable CSI2 capture at viif_mux_start() */
#define VIIF_CSI2_NOT_CAPTURE -1

/* minimum crop width and height */
#define VIIF_CROP_MIN_W 128U
#define VIIF_CROP_MIN_H 128U

/* up to 2 ROIs are available to be passed to POSTs */
/* set ROI_NONE for a POST currently not running */
#define VIIF_L2_ROI_MAX_NUM 2U
#define VIIF_L2_ROI_NONE    3U

/* validation at main_set_unit() and sub_set_unit() */
/* picture size: [unit: pixel] */
#define VIIF_MIN_HTOTAL_PIXEL 143U
#define VIIF_MAX_HTOTAL_PIXEL 65535U

/* pixel clock: [unit: kHz] */
#define VIIF_MIN_PIXEL_CLOCK 3375U
#define VIIF_MAX_PIXEL_CLOCK 600000U

/* horizontal back porch size: [unit: system clock ticks] */
#define VIIF_HBP_SYSCLK 10U

/* num of pictures accepted by the ISP */
#define VIIF_L1_INPUT_NUM_MIN 1U
#define VIIF_L1_INPUT_NUM_MAX 3U

/* active picture size: [unit: pixel] */
#define VIIF_MIN_HACTIVE_PIXEL_W_L1ISP	640U
#define VIIF_MAX_HACTIVE_PIXEL_W_L1ISP	3840U
#define VIIF_MIN_HACTIVE_PIXEL_WO_L1ISP 128U
#define VIIF_MAX_HACTIVE_PIXEL_WO_L1ISP 4096U

/* picture vertical size: [unit: line] */
#define VIIF_MIN_VTOTAL_LINE	       144U
#define VIIF_MAX_VTOTAL_LINE	       16383U
#define VIIF_MIN_VBP_LINE	       5U
#define VIIF_MAX_VBP_LINE	       4095U
#define VIIF_MIN_VACTIVE_LINE_WO_L1ISP 128U
#define VIIF_MAX_VACTIVE_LINE_WO_L1ISP 2160U
#define VIIF_MIN_VACTIVE_LINE_W_L1ISP  480U
#define VIIF_MAX_VACTIVE_LINE_W_L1ISP  2160U

/* internal operation latencies: [unit: system clock ticks]*/
#define VIIF_TABLE_LOAD_TIME	24000UL
#define VIIF_REGBUF_ACCESS_TIME 15360UL

/* offset of Vsync delay: [unit: line] */
#define VIIF_L1_DELAY_W_HDRC  31U
#define VIIF_L1_DELAY_WO_HDRC 11U

/* timeout definitions for viif_stop_mux() */
/*
 * wait time for force abort to complete (max 1line time = 1228.8 us)
 * when width = 4096, RAW24, 80Mbps
 */
#define VIIF_WAIT_ABORT_COMPLETE_TIME 1229U

/*
 * complete time of register buffer transfer.
 * actual time is about 30us in case of L1ISP
 */
#define VIIF_WAIT_ISP_REGBF_TRNS_COMPLETE_TIME 39U

/* default parameters for V4L2 subdevice node */
#define VISCONTI_VIIF_ISP_DEFAULT_WIDTH	  1920
#define VISCONTI_VIIF_ISP_DEFAULT_HEIGHT  1080
#define VISCONTI_VIIF_MAX_COMPOSED_WIDTH  8190
#define VISCONTI_VIIF_MAX_COMPOSED_HEIGHT 4094

/**
 * struct viif_input_img - input image information
 * @pixel_clock: pixel clock [unit: kHz]. Range: [3375..600000]
 * @htotal_size: horizontal total size [unit: pixel]. Range: [143..65535]
 * @hactive_size: horizontal active size [unit: pixel]
 * * Range (w/o L1ISP): [128..4096] (multiple of 2)
 * * Range (with L1ISP): [640..3840] (multiple of 8)
 * * Range (SUB path): 0
 * @vtotal_size: vertical total size [unit: line].
 * * Range: [144..16383]
 * @vbp_size: vertical back porch size.
 * * Range: [5..4095]
 * @vactive_size: vertical active size [unit: line].
 * * Range (w/o L1ISP) [128..2160] (multiple of 2)
 * * Range (with L1ISP) [480..2160] (multiple of 2)
 * @input_num: the number of input images. Range: [1..3]
 * * Range (w/o L1ISP): 1
 * * Range (with L1ISP): [1..3]
 * * Range (SUB path): 1
 * @hobc_width: the number of horizontal optical black pixels.
 * * Range (w/o L1ISP): 0
 * * Range (with L1ISP):  [0,16,32,64 or 128]
 *   * should be 0 when hobc_margin = 0
 * * Range (SUB path): 0
 * @hobc_margin: the number of horizontal optical black margin.
 * * Range (w/o L1ISP): 0
 * * Range (with L1ISP): [0..30] (even number)
 +   * should be 0 when hobc_width = 0
 * * Range (SUB path): 0
 *
 * Constraints between parameters:
 *
 * * (htotal_size > (hactive_size + hobc_width + hobc_margin))
 * * (vtotal_size > (vbp_size + vactive_size * input_num))
 * * w/o L1ISP:
 *   * vbp_size >= (39360[cycle] / 500000[kHz]) * (pixel_clock / htotal_size) + 16 + ISST time
 * * with L1ISP:
 *   * vbp_size >= (54720[cycle] / 500000[kHz]) * (pixel_clock / htotal_size) + 38 + ISST time
 *
 * Note: L1ISP is used when RAW data is input to MAIN unit
 */
struct viif_input_img {
	u32 pixel_clock;
	u32 htotal_size;
	u32 hactive_size;
	u32 vtotal_size;
	u32 vbp_size;
	u32 vactive_size;
	u32 input_num;
	u32 hobc_width;
	u32 hobc_margin;
};

/*=============================================*/
/* Low Layer Implementation */
/*=============================================*/
/* Convert the unit of time-period (from sysclk, to num lines in the image) */
static u32 sysclk_to_numlines(u32 time_in_sysclk, const struct viif_input_img *img)
{
	u64 v1 = (u64)time_in_sysclk * img->pixel_clock;
	u64 v2 = (u64)img->htotal_size * VIIF_SYS_CLK;

	return div64_u64(v1, v2);
}

static u32 lineperiod_in_sysclk(u32 hsize, u32 pixel_clock)
{
	return div64_u64((u64)hsize * VIIF_SYS_CLK, pixel_clock);
}

/* Set ROI path condition when ROI num is 2 */
static void viif_l2_set_roi_num_2(struct viif_device *viif_dev)
{
	struct viif_l2_roi_path_info *info = &viif_dev->l2_roi_path_info;
	u32 i;

	for (i = 0; i < VIIF_L2_ROI_MAX_NUM; i++) {
		/* ROI-n is the same as CROP area of POST-n */
		if (info->post_enable_flag[i]) {
			viif_capture_write(viif_dev, REG_L2_ROI_X_OUT_OFS_H(i),
					   info->post_crop_x[i]);
			viif_capture_write(viif_dev, REG_L2_ROI_X_OUT_OFS_V(i),
					   info->post_crop_y[i]);
			viif_capture_write(viif_dev, REG_L2_ROI_X_OUT_HSIZE(i),
					   info->post_crop_w[i]);
			viif_capture_write(viif_dev, REG_L2_ROI_X_OUT_VSIZE(i),
					   info->post_crop_h[i]);
			viif_capture_write(viif_dev, REG_L2_ROI_TO_POST(i), i);
		} else {
			viif_capture_write(viif_dev, REG_L2_ROI_X_OUT_OFS_H(i), 0);
			viif_capture_write(viif_dev, REG_L2_ROI_X_OUT_OFS_V(i), 0);
			viif_capture_write(viif_dev, REG_L2_ROI_X_OUT_HSIZE(i), VIIF_CROP_MIN_W);
			viif_capture_write(viif_dev, REG_L2_ROI_X_OUT_VSIZE(i), VIIF_CROP_MIN_H);
			viif_capture_write(viif_dev, REG_L2_ROI_TO_POST(i), VIIF_L2_ROI_NONE);
		}
	}
}

/* Set ROI path condition when ROI num is 1 */
static void viif_l2_set_roi_num_1(struct viif_device *viif_dev)
{
	struct viif_l2_roi_path_info *info = &viif_dev->l2_roi_path_info;
	u32 val, x_min, x_max, y_min, y_max;
	u32 i, x, y, w, h;

	/* ROI0 is input to POST0 and POST1 */
	if (info->post_enable_flag[0]) {
		/* POST0 is enabled */
		x_min = info->post_crop_x[0];
		x_max = info->post_crop_x[0] + info->post_crop_w[0];
		y_min = info->post_crop_y[0];
		y_max = info->post_crop_y[0] + info->post_crop_h[0];
		if (info->post_enable_flag[1]) {
			/* POST1 is enabled */
			x_min = min(x_min, info->post_crop_x[1]);
			val = info->post_crop_x[1] + info->post_crop_w[1];
			x_max = max(x_max, val);
			y_min = min(y_min, info->post_crop_y[1]);
			val = info->post_crop_y[1] + info->post_crop_h[1];
			y_max = max(y_max, val);
		}
		x = x_min;
		y = y_min;
		w = x_max - x_min;
		h = y_max - y_min;
	} else if (info->post_enable_flag[1]) {
		/* POST0 is disabled and POST1 is enabled */
		x = info->post_crop_x[1];
		w = info->post_crop_w[1];
		y = info->post_crop_y[1];
		h = info->post_crop_h[1];
	} else {
		/* All POSTs are disabled */
		x = 0;
		y = 0;
		w = VIIF_CROP_MIN_W;
		h = VIIF_CROP_MIN_H;
	}
	viif_capture_write(viif_dev, REG_L2_ROI_X_OUT_OFS_H(0), x);
	viif_capture_write(viif_dev, REG_L2_ROI_X_OUT_OFS_V(0), y);
	viif_capture_write(viif_dev, REG_L2_ROI_X_OUT_HSIZE(0), w);
	viif_capture_write(viif_dev, REG_L2_ROI_X_OUT_VSIZE(0), h);

	for (i = 0; i < VIIF_MAX_POST_NUM; i++)
		viif_capture_write(viif_dev, REG_L2_ROI_TO_POST(i),
				   info->post_enable_flag[i] ? 0 : VIIF_L2_ROI_NONE);
}

/* Set ROI path condition */
void visconti_viif_l2_set_roi_path(struct viif_device *viif_dev)
{
	if (viif_dev->l2_roi_path_info.roi_num == 1U)
		viif_l2_set_roi_num_1(viif_dev);
	else
		viif_l2_set_roi_num_2(viif_dev);
}

/* Set ROI parameters of L2ISP */
void visconti_viif_l2_set_roi(struct viif_device *viif_dev, const struct viif_l2_roi_config *param)
{
	u32 val;
	int i;

	/* Set the number of ROI and update resource info with roi_num */
	viif_capture_write(viif_dev, REG_L2_ROI_NUM, param->roi_num);
	viif_dev->l2_roi_path_info.roi_num = param->roi_num;

	/* Update ROI area and input to each POST */
	visconti_viif_l2_set_roi_path(viif_dev);

	/* Set the remaining parameters */
	for (i = 0; i < 2; i++) {
		viif_capture_write(viif_dev, REG_L2_ROI_X_SCALE(i), param->roi_scale[i]);
		viif_capture_write(viif_dev, REG_L2_ROI_X_SCALE_INV(i), param->roi_scale_inv[i]);
		val = (param->corrected_wo_scale_hsize[i] << 13U) | param->corrected_hsize[i];
		viif_capture_write(viif_dev, REG_L2_ROI_X_CORRECTED_HSIZE(i), val);
		val = (param->corrected_wo_scale_vsize[i] << 12U) | param->corrected_vsize[i];
		viif_capture_write(viif_dev, REG_L2_ROI_X_CORRECTED_VSIZE(i), val);
	}
}

/**
 * viif_main_set_unit() - Set static configuration of MAIN unit(CH0 or CH1)
 *
 * @viif_dev: the VIIF device
 * @data_type: DT of image; either of
 *     YUV422_8B, YUV422_10B, RGB565, RGB888, RAW8, RAW10, RAW12, RAW14
 * @in_img: Pointer to input image information
 * @rawpack: RAW pack mode. For more refer @ref hwd_viif_raw_pack_mode
 * @yuv_interp: true to use interpolation for YUV422 to YUV444 conversion.
 * Return: 0 for success, -EINVAL for parameter error
 */
static int viif_main_set_unit(struct viif_device *viif_dev, u32 data_type,
			      const struct viif_input_img *in_img, u32 rawpack, bool yuv_interp)
{
	u32 total_hact_size = 0U, total_vact_size = 0U;
	u32 sw_delay0, sw_delay1, hw_delay;
	u32 val, color, sysclk_num;
	u32 i;

	if (!in_img)
		return -EINVAL;
	if (rawpack != VIIF_RAWPACK_DISABLE && rawpack != VIIF_RAWPACK_MSBFIRST &&
	    rawpack != VIIF_RAWPACK_LSBFIRST) {
		return -EINVAL;
	}
	if (data_type != MIPI_CSI2_DT_RAW8 && data_type != MIPI_CSI2_DT_RAW10 &&
	    data_type != MIPI_CSI2_DT_RAW12 && rawpack != VIIF_RAWPACK_DISABLE) {
		return -EINVAL;
	}

	if (in_img->pixel_clock < VIIF_MIN_PIXEL_CLOCK ||
	    in_img->pixel_clock > VIIF_MAX_PIXEL_CLOCK ||
	    in_img->htotal_size < VIIF_MIN_HTOTAL_PIXEL ||
	    in_img->htotal_size > VIIF_MAX_HTOTAL_PIXEL ||
	    in_img->vtotal_size < VIIF_MIN_VTOTAL_LINE ||
	    in_img->vtotal_size > VIIF_MAX_VTOTAL_LINE || in_img->vbp_size < VIIF_MIN_VBP_LINE ||
	    in_img->vbp_size > VIIF_MAX_VBP_LINE || (in_img->hactive_size % 2U) ||
	    (in_img->vactive_size % 2U)) {
		return -EINVAL;
	}

	if (in_img->input_num < VIIF_L1_INPUT_NUM_MIN ||
	    in_img->input_num > VIIF_L1_INPUT_NUM_MAX) {
		return -EINVAL;
	}

	if (in_img->hobc_width != 0U && in_img->hobc_width != 16U && in_img->hobc_width != 32U &&
	    in_img->hobc_width != 64U && in_img->hobc_width != 128U) {
		return -EINVAL;
	}

	if (in_img->hobc_margin > 30U || (in_img->hobc_margin % 2U))
		return -EINVAL;

	if (!in_img->hobc_width && in_img->hobc_margin)
		return -EINVAL;

	if (in_img->hobc_width && !in_img->hobc_margin)
		return -EINVAL;

	if (data_type == MIPI_CSI2_DT_RAW8 || data_type == MIPI_CSI2_DT_RAW10 ||
	    data_type == MIPI_CSI2_DT_RAW12 || data_type == MIPI_CSI2_DT_RAW14) {
		/* parameter check in case of L1ISP(in case of RAW) */
		if (in_img->hactive_size < VIIF_MIN_HACTIVE_PIXEL_W_L1ISP ||
		    in_img->hactive_size > VIIF_MAX_HACTIVE_PIXEL_W_L1ISP ||
		    in_img->vactive_size < VIIF_MIN_VACTIVE_LINE_W_L1ISP ||
		    in_img->vactive_size > VIIF_MAX_VACTIVE_LINE_W_L1ISP ||
		    (in_img->hactive_size % 8U)) {
			return -EINVAL;
		}

		/* check vbp range in case of L1ISP on */
		/* the constant value "7" is configuration margin */
		val = sysclk_to_numlines(VIIF_TABLE_LOAD_TIME + VIIF_REGBUF_ACCESS_TIME * 2U,
					 in_img) +
		      VIIF_L1_DELAY_W_HDRC + 7U;
		if (in_img->vbp_size < val)
			return -EINVAL;

		/* calculate total of horizontal active size and vertical active size */
		if (rawpack != VIIF_RAWPACK_DISABLE) {
			val = (in_img->hactive_size + in_img->hobc_width + in_img->hobc_margin) *
			      2U;
		} else {
			val = in_img->hactive_size + in_img->hobc_width + in_img->hobc_margin;
		}

		total_hact_size = val;
		total_vact_size = in_img->vactive_size * in_img->input_num;
	} else {
		/* OTHER input than RAW(L1ISP is off) */
		if (in_img->hactive_size < VIIF_MIN_HACTIVE_PIXEL_WO_L1ISP ||
		    in_img->hactive_size > VIIF_MAX_HACTIVE_PIXEL_WO_L1ISP ||
		    in_img->vactive_size < VIIF_MIN_VACTIVE_LINE_WO_L1ISP ||
		    in_img->vactive_size > VIIF_MAX_VACTIVE_LINE_WO_L1ISP ||
		    in_img->input_num != 1 || in_img->hobc_width) {
			return -EINVAL;
		}

		/* check vbp range in case of L1ISP off */
		/* the constant value "16" is configuration margin */
		val = sysclk_to_numlines(VIIF_TABLE_LOAD_TIME + VIIF_REGBUF_ACCESS_TIME, in_img) +
		      16U;
		if (in_img->vbp_size < val)
			return -EINVAL;

		total_hact_size = in_img->hactive_size;
		total_vact_size = in_img->vactive_size;
	}

	if (in_img->htotal_size <= total_hact_size ||
	    (in_img->vtotal_size <= (in_img->vbp_size + total_vact_size))) {
		return -EINVAL;
	}

	/* Set DT and color type of image data */
	viif_capture_write(viif_dev, REG_IPORTM_MAIN_DT, (data_type << 8U) | data_type);
	viif_capture_write(viif_dev, REG_IPORTM_OTHER, 0x00);

	/* Set back porch*/
	viif_capture_write(viif_dev, REG_BACK_PORCH_M, (in_img->vbp_size << 16U) | VIIF_HBP_SYSCLK);

	/* single pulse of vsync is input to DPGM */
	viif_capture_write(viif_dev, REG_DPGM_VSYNC_SOURCE, VAL_DPGM_VSYNC_PULSE);

	/* set preprocess type before L2ISP based on color_type. */
	if (data_type == MIPI_CSI2_DT_YUV422_8B || data_type == MIPI_CSI2_DT_YUV422_10B) {
		color = VAL_PREPROCCESS_FMT_YUV422;
	} else if (data_type == MIPI_CSI2_DT_RGB565 || data_type == MIPI_CSI2_DT_RGB888) {
		color = VAL_PREPROCCESS_FMT_RGB;
	} else {
		/* RGB or YUV444 from L1ISP */
		color = VAL_PREPROCCESS_FMT_YUV444;
	}
	viif_capture_write(viif_dev, REG_PREPROCCESS_FMTM, color);

	/* set Total size and valid size information of image data */
	sysclk_num = lineperiod_in_sysclk(in_img->htotal_size, in_img->pixel_clock);
	sysclk_num &= GENMASK(15, 0);
	viif_capture_write(viif_dev, REG_TOTALSIZE_M, (in_img->vtotal_size << 16U) | sysclk_num);
	viif_capture_write(viif_dev, REG_VALSIZE_M, (total_vact_size << 16U) | total_hact_size);

	/* set image size information to L2ISP */
	viif_capture_write(viif_dev, REG_L2_SENSOR_CROP_VSIZE, in_img->vactive_size);
	viif_capture_write(viif_dev, REG_L2_SENSOR_CROP_HSIZE, in_img->hactive_size);

	/* RAW input case */
	if (data_type >= MIPI_CSI2_DT_RAW8) {
		/* interpolaton mode = by LINE */
		viif_capture_write(viif_dev, REG_L1_IBUF_INPUT_ORDER, in_img->input_num);
		viif_capture_write(viif_dev, REG_L1_SYSM_HEIGHT, in_img->vactive_size);
		viif_capture_write(viif_dev, REG_L1_SYSM_WIDTH, in_img->hactive_size);
		val = (in_img->hobc_margin << 8U) | in_img->hobc_width;
		viif_capture_write(viif_dev, REG_L1_HOBC_MARGIN, val);
	}

	/* Set rawpack */
	viif_capture_write(viif_dev, REG_IPORTM_MAIN_RAW, rawpack);

	/* Set yuv_conv; only for VAL_PREPROCCESS_FMT_YUV422 */
	viif_capture_write(viif_dev, REG_PREPROCCESS_C24M, yuv_interp ? 1 : 0);

	/* Set vsync delay */
	hw_delay = in_img->vbp_size - sysclk_to_numlines(VIIF_TABLE_LOAD_TIME, in_img) + 4U;
	hw_delay = min(hw_delay, 255U);

	sw_delay0 = hw_delay - sysclk_to_numlines(VIIF_REGBUF_ACCESS_TIME, in_img) + 2U;

	if (data_type == MIPI_CSI2_DT_RAW8 || data_type == MIPI_CSI2_DT_RAW10 ||
	    data_type == MIPI_CSI2_DT_RAW12 || data_type == MIPI_CSI2_DT_RAW14) {
		sw_delay1 = sysclk_to_numlines(VIIF_REGBUF_ACCESS_TIME, in_img) +
			    VIIF_L1_DELAY_WO_HDRC + 1U;
	} else {
		sw_delay1 = 10U;
	}
	viif_capture_write(viif_dev, REG_INT_M0_LINE, sw_delay0 << 16U);
	viif_capture_write(viif_dev, REG_INT_M1_LINE, (sw_delay1 << 16U) | hw_delay);

	/* M2_LINE is the same condition as M1_LINE */
	viif_capture_write(viif_dev, REG_INT_M2_LINE, (sw_delay1 << 16U) | hw_delay);

	/* hold pixel_clock, htotal_size for future use */
	viif_dev->img_clk.pixel_clock = in_img->pixel_clock;
	viif_dev->img_clk.htotal_size = in_img->htotal_size;

	/* initialize crop information of POSTs */
	viif_dev->l2_roi_path_info.roi_num = 0;
	for (i = 0; i < VIIF_MAX_POST_NUM; i++) {
		viif_dev->l2_roi_path_info.post_enable_flag[i] = false;
		viif_dev->l2_roi_path_info.post_crop_x[i] = 0;
		viif_dev->l2_roi_path_info.post_crop_y[i] = 0;
		viif_dev->l2_roi_path_info.post_crop_w[i] = 0;
		viif_dev->l2_roi_path_info.post_crop_h[i] = 0;
	}

	return 0;
}

/**
 * viif_sub_set_unit() - Set static configuration of SUB unit
 *
 * @viif_dev: the VIIF device
 * @dt_image: DT of image. Range: [0x1E, 0x1F, 0x22, 0x24, 0x2A-0x2D]
 * @in_img: Pointer to input image information
 * Return: 0 for success, -EINVAL for parameter error
 */
static int viif_sub_set_unit(struct viif_device *viif_dev, u32 dt_image,
			     const struct viif_input_img *in_img)
{
	u32 sysclk_num, temp_delay;

	if (dt_image < MIPI_CSI2_DT_RAW8 || dt_image > MIPI_CSI2_DT_RAW14)
		return -EINVAL;

	if (!in_img)
		return -EINVAL;

	if (in_img->hactive_size || in_img->input_num != 1 || in_img->hobc_width ||
	    in_img->hobc_margin) {
		return -EINVAL;
	}

	if (in_img->pixel_clock < VIIF_MIN_PIXEL_CLOCK ||
	    in_img->pixel_clock > VIIF_MAX_PIXEL_CLOCK ||
	    in_img->htotal_size < VIIF_MIN_HTOTAL_PIXEL ||
	    in_img->htotal_size > VIIF_MAX_HTOTAL_PIXEL ||
	    in_img->vtotal_size < VIIF_MIN_VTOTAL_LINE ||
	    in_img->vtotal_size > VIIF_MAX_VTOTAL_LINE || in_img->vbp_size < VIIF_MIN_VBP_LINE ||
	    in_img->vbp_size > VIIF_MAX_VBP_LINE ||
	    in_img->vactive_size < VIIF_MIN_VACTIVE_LINE_WO_L1ISP ||
	    in_img->vactive_size > VIIF_MAX_VACTIVE_LINE_WO_L1ISP || (in_img->vactive_size % 2U)) {
		return -EINVAL;
	}

	if (in_img->vtotal_size <= (in_img->vbp_size + in_img->vactive_size))
		return -EINVAL;

	/* Set DT of image data and DT of long packet data*/
	viif_capture_write(viif_dev, REG_IPORTS_MAIN_DT, dt_image);
	viif_capture_write(viif_dev, REG_IPORTS_OTHER, 0x00);

	/* Set line size and delay value of delayed Vsync */
	sysclk_num = lineperiod_in_sysclk(in_img->htotal_size, in_img->pixel_clock);
	viif_capture_write(viif_dev, REG_INT_SA0_LINE, sysclk_num & GENMASK(15, 0));
	temp_delay = in_img->vbp_size - 4U;
	if (temp_delay > 255U) {
		/* Replace the value with HW max spec */
		temp_delay = 255U;
	}
	viif_capture_write(viif_dev, REG_INT_SA1_LINE, temp_delay);

	return 0;
}

/**
 * viif_mux_start() - Setup CSI-2 input path
 *
 * @viif_dev: the VIIF device
 * @vc_main: VCID (0, 1, 2, 3) to capture with Main unit; VIIF_CSI2_NOT_CAPTURE to disable.
 * @vc_sub:  VCID (0, 1, 2, 3) to capture with Sub unit; VIIF_CSI2_NOT_CAPTURE to disable.
 */
static void viif_mux_start(struct viif_device *viif_dev, s32 vc_main, s32 vc_sub)
{
	bool en_vc0 = false, en_vc1 = false;

	viif_capture_write(viif_dev, REG_IPORTM, VAL_IPORTM_INPUT_CSI2);

	if (vc_main != VIIF_CSI2_NOT_CAPTURE) {
		viif_capture_write(viif_dev, REG_VCID0SELECT, (u32)vc_main);
		en_vc0 = true;
	}
	if (vc_sub != VIIF_CSI2_NOT_CAPTURE) {
		viif_capture_write(viif_dev, REG_VCID1SELECT, (u32)vc_sub);
		en_vc1 = true;
	}

	/* Control VC port enable */
	viif_capture_write(viif_dev, REG_VCPORTEN,
			   (en_vc0 ? MASK_VCPORTEN_EN_VC0 : 0) |
				   (en_vc1 ? MASK_VCPORTEN_EN_VC1 : 0));

	if (en_vc0) {
		/* Update flag information for run status of MAIN unit */
		viif_dev->run_flag_main = true;
	}
}

/**
 * viif_mux_stop() - Teardown CSI-2 input path
 *
 * @viif_dev: the VIIF device
 * Return: 0 for success, -ETIMEDOUT for timeout error
 */
static int viif_mux_stop(struct viif_device *viif_dev)
{
	u64 timeout_ns, cur_ns;

	/* Disable auto transmission of register buffer */
	viif_capture_write(viif_dev, REG_L1_CRGBF_TRN_A_CONF, 0);
	viif_capture_write(viif_dev, REG_L2_CRGBF_TRN_A_CONF, 0);

	/* Wait for completion of register buffer transmission */
	udelay(VIIF_WAIT_ISP_REGBF_TRNS_COMPLETE_TIME);

	/* Stop all VCs, long packet input and emb data input of MAIN unit */
	viif_capture_write(viif_dev, REG_VCPORTEN, 0);
	viif_capture_write(viif_dev, REG_IPORTM_OTHEREN, 0);
	viif_capture_write(viif_dev, REG_IPORTM_EMBEN, 0);

	/* Stop image data input, long packet input and emb data input of SUB unit */
	viif_capture_write(viif_dev, REG_IPORTS_OTHEREN, 0);
	viif_capture_write(viif_dev, REG_IPORTS_EMBEN, 0);
	viif_capture_write(viif_dev, REG_IPORTS_IMGEN, 0);

	/* Stop VDMAC for all table ports, input ports and write ports */
	viif_capture_write(viif_dev, REG_VDM_T_ENABLE, 0);
	viif_capture_write(viif_dev, REG_VDM_R_ENABLE, 0);
	viif_capture_write(viif_dev, REG_VDM_W_ENABLE, 0);

	/* Stop all groups(g00, g01 and g02) of VDMAC */
	viif_capture_write(viif_dev, REG_VDM_ABORTSET, 0x7);

	timeout_ns = ktime_get_ns() + VIIF_WAIT_ABORT_COMPLETE_TIME * 1000;

	do {
		u32 status_r, status_w, status_t, l2_status;

		/* Get VDMAC transfer status  */
		status_r = viif_capture_read(viif_dev, REG_VDM_R_RUN);
		status_w = viif_capture_read(viif_dev, REG_VDM_W_RUN);
		status_t = viif_capture_read(viif_dev, REG_VDM_T_RUN);
		l2_status = viif_capture_read(viif_dev, REG_L2_BUS_L2_STATUS);

		if (!status_r && !status_w && !status_t && !l2_status) {
			viif_dev->run_flag_main = false;
			return 0;
		}

		cur_ns = ktime_get_ns();
	} while (timeout_ns > cur_ns);

	return -ETIMEDOUT;
}

/*=============================================*/
/* handling V4L2 framework */
/*=============================================*/
/* ----- supported MBUS formats ----- */
static bool viif_get_mbus_rgb_out(unsigned int mbus_code)
{
	const struct viif_mbus_format *fmt;

	fmt = viif_mbus_format_from_code(mbus_code);

	return fmt ? fmt->rgb_out : false; /* YUV as default */
}

static bool viif_is_valid_mbus_code(unsigned int mbus_code)
{
	return viif_mbus_format_from_code(mbus_code) ? true : false;
}

/* ----- handling main processing path ----- */
static int viif_get_dv_timings(struct viif_device *viif_dev, struct v4l2_dv_timings *timings,
			       unsigned int *mbus_code)
{
	struct v4l2_subdev *sensor_sd = viif_dev->sensor_sd;
	struct v4l2_subdev_state *state;
	struct v4l2_subdev_format format = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
		.pad = 0,
	};
	struct v4l2_ctrl *ctrl;
	int ret;

	if (!sensor_sd)
		return -EINVAL;

	state = v4l2_subdev_lock_and_get_active_state(sensor_sd);
	if (state) {
		ret = v4l2_subdev_call(sensor_sd, pad, get_fmt, state, &format);
		v4l2_subdev_unlock_state(state);
	} else {
		struct v4l2_subdev_pad_config pad_cfg;
		struct v4l2_subdev_state pad_state = { .pads = &pad_cfg };

		ret = v4l2_subdev_call(sensor_sd, pad, get_fmt, &pad_state, &format);
	}
	if (ret)
		return ret;

	/* some video I/F support dv_timings query */
	ret = v4l2_subdev_call(sensor_sd, video, g_dv_timings, timings);
	if (!ret) {
		*mbus_code = format.format.code;
		return 0;
	}

	/* others: call some discrete APIs */
	timings->bt.width = format.format.width;
	timings->bt.height = format.format.height;
	*mbus_code = format.format.code;

	ctrl = v4l2_ctrl_find(sensor_sd->ctrl_handler, V4L2_CID_HBLANK);
	if (!ctrl) {
		dev_err(viif_dev->dev, "subdev: V4L2_CID_VBLANK error.\n");
		return -EINVAL;
	}
	timings->bt.hsync = v4l2_ctrl_g_ctrl(ctrl);

	ctrl = v4l2_ctrl_find(sensor_sd->ctrl_handler, V4L2_CID_VBLANK);
	if (!ctrl) {
		dev_err(viif_dev->dev, "subdev: V4L2_CID_VBLANK error.\n");
		return -EINVAL;
	}
	timings->bt.vsync = v4l2_ctrl_g_ctrl(ctrl);

	ctrl = v4l2_ctrl_find(sensor_sd->ctrl_handler, V4L2_CID_PIXEL_RATE);
	if (!ctrl) {
		dev_err(viif_dev->dev, "subdev: V4L2_CID_PIXEL_RATE error.\n");
		return -EINVAL;
	}
	timings->bt.pixelclock = v4l2_ctrl_g_ctrl_int64(ctrl);

	return 0;
}

static unsigned int dt_image_from_mbus_code(unsigned int mbus_code)
{
	const struct viif_mbus_format *fmt;

	fmt = viif_mbus_format_from_code(mbus_code);

	return fmt ? fmt->mipi_dt : MIPI_CSI2_DT_RGB888;
}

int visconti_viif_isp_main_set_unit(struct viif_device *viif_dev)
{
	struct v4l2_subdev *sensor_sd = viif_dev->sensor_sd;
	struct viif_input_img in_img_main = {};
	struct v4l2_dv_timings timings = {};
	unsigned int data_type, rawpack;
	bool yuv_interp = false;
	unsigned int mbus_code;
	int mag_hactive = 1;
	int ret = 0;

	if (!sensor_sd)
		return -EINVAL;

	/* check controls */
	viif_dev->isp_subdev.ctrl_rawpack_mode->ops->s_ctrl(viif_dev->isp_subdev.ctrl_rawpack_mode);
	viif_dev->isp_subdev.ctrl_input_mode->ops->s_ctrl(viif_dev->isp_subdev.ctrl_input_mode);

	ret = viif_get_dv_timings(viif_dev, &timings, &mbus_code);
	if (ret) {
		dev_err(viif_dev->dev, "could not get timing information of subdev");
		return -EINVAL;
	}

	data_type = dt_image_from_mbus_code(mbus_code);

	if (data_type == MIPI_CSI2_DT_RAW8 || data_type == MIPI_CSI2_DT_RAW10 ||
	    data_type == MIPI_CSI2_DT_RAW12) {
		rawpack = viif_dev->rawpack_mode;
		if (rawpack != VIIF_RAWPACK_DISABLE)
			mag_hactive = 2;
	} else {
		rawpack = VIIF_RAWPACK_DISABLE;
	}

	yuv_interp = (data_type == MIPI_CSI2_DT_YUV422_8B || data_type == MIPI_CSI2_DT_YUV422_10B);

	in_img_main.hactive_size = timings.bt.width;
	in_img_main.vactive_size = timings.bt.height;
	in_img_main.htotal_size = timings.bt.width * mag_hactive + timings.bt.hsync;
	in_img_main.vtotal_size = timings.bt.height + timings.bt.vsync;
	in_img_main.pixel_clock = timings.bt.pixelclock / 1000;
	in_img_main.vbp_size = timings.bt.vsync - 5;

	in_img_main.input_num = 1;
	in_img_main.hobc_width = 0;
	in_img_main.hobc_margin = 0;

	/* configuration of MAIN unit */
	ret = viif_main_set_unit(viif_dev, data_type, &in_img_main, rawpack, yuv_interp);
	if (ret) {
		dev_err(viif_dev->dev, "main_set_unit error. %d\n", ret);
		return ret;
	}

	/* Enable regbuf */
	hwd_viif_isp_set_regbuf_auto_transmission(viif_dev);

	/* L2 UNDIST Enable through mode as default  */
	ret = visconti_viif_l2_undist_through(viif_dev);
	if (ret)
		dev_err(viif_dev->dev, "l2_set_undist error. %d\n", ret);
	return ret;
}

int visconti_viif_isp_sub_set_unit(struct viif_device *viif_dev)
{
	struct v4l2_subdev *sensor_sd = viif_dev->sensor_sd;
	struct viif_input_img in_img_sub;
	struct v4l2_dv_timings timings;
	unsigned int mbus_code;
	unsigned int dt_image;
	int ret;

	if (!sensor_sd)
		return -EINVAL;

	ret = viif_get_dv_timings(viif_dev, &timings, &mbus_code);
	if (ret)
		return -EINVAL;

	dt_image = dt_image_from_mbus_code(mbus_code);

	in_img_sub.hactive_size = 0;
	in_img_sub.vactive_size = timings.bt.height;
	in_img_sub.htotal_size = timings.bt.width + timings.bt.hsync;
	in_img_sub.vtotal_size = timings.bt.height + timings.bt.vsync;
	in_img_sub.pixel_clock = timings.bt.pixelclock / 1000;
	in_img_sub.vbp_size = timings.bt.vsync - 5;
	in_img_sub.input_num = 1;
	in_img_sub.hobc_width = 0;
	in_img_sub.hobc_margin = 0;

	ret = viif_sub_set_unit(viif_dev, dt_image, &in_img_sub);
	if (ret)
		dev_err(viif_dev->dev, "sub_set_unit error. %d\n", ret);

	return ret;
};

/* ----- subdevice video operations ----- */
static int visconti_viif_isp_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct viif_device *viif_dev = ((struct isp_subdev *)sd)->viif_dev;
	int ret;

	/* disabling: stop sensor, CSI2RX -> stop MUX, ISP */
	if (!enable) {
		v4l2_subdev_call(&viif_dev->csi2rx_subdev.sd, video, s_stream, false);
		viif_mux_stop(viif_dev);
		// stop ISP
		return 0;
	}

	__v4l2_ctrl_handler_setup(&viif_dev->isp_subdev.ctrl_handler);

	/* enabling: start ISP, MUX -> start CSI2RX, sensor */
	// start ISP
	viif_dev->masked_gamma_path = 0;
	viif_mux_start(viif_dev, 0, 0);
	ret = v4l2_subdev_call(&viif_dev->csi2rx_subdev.sd, video, s_stream, true);
	if (ret)
		viif_mux_stop(viif_dev);
	return ret;
}

/* ----- subdevice pad operations ----- */
static int visconti_viif_isp_enum_mbus_code(struct v4l2_subdev *sd,
					    struct v4l2_subdev_state *sd_state,
					    struct v4l2_subdev_mbus_code_enum *code)
{
	if (!code->pad) {
		/* sink */
		const struct viif_mbus_format *fmt;

		fmt = viif_mbus_format_nth(code->index);
		if (fmt) {
			code->code = fmt->code;
			return 0;
		} else {
			return -EINVAL;
		}
	}

	/* source */
	if (code->index > 0)
		return -EINVAL;
	code->code = MEDIA_BUS_FMT_YUV8_1X24;
	return 0;
}

static struct v4l2_mbus_framefmt *visconti_viif_isp_get_pad_fmt(struct v4l2_subdev *sd,
								struct v4l2_subdev_state *sd_state,
								unsigned int pad, u32 which)
{
	struct viif_device *viif_dev = ((struct isp_subdev *)sd)->viif_dev;
	struct v4l2_subdev_state state = {
		.pads = viif_dev->isp_subdev.pad_cfg,
	};

	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_get_try_format(&viif_dev->isp_subdev.sd, sd_state, pad);
	else
		return v4l2_subdev_get_try_format(&viif_dev->isp_subdev.sd, &state, pad);
}

static struct v4l2_rect *visconti_viif_isp_get_pad_crop(struct v4l2_subdev *sd,
							struct v4l2_subdev_state *sd_state,
							unsigned int pad, u32 which)
{
	struct viif_device *viif_dev = ((struct isp_subdev *)sd)->viif_dev;
	struct v4l2_subdev_state state = {
		.pads = viif_dev->isp_subdev.pad_cfg,
	};

	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_get_try_crop(&viif_dev->isp_subdev.sd, sd_state, pad);
	else
		return v4l2_subdev_get_try_crop(&viif_dev->isp_subdev.sd, &state, pad);
}

static struct v4l2_rect *visconti_viif_isp_get_pad_compose(struct v4l2_subdev *sd,
							   struct v4l2_subdev_state *sd_state,
							   unsigned int pad, u32 which)
{
	struct viif_device *viif_dev = ((struct isp_subdev *)sd)->viif_dev;
	struct v4l2_subdev_state state = {
		.pads = viif_dev->isp_subdev.pad_cfg,
	};

	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_get_try_compose(&viif_dev->isp_subdev.sd, sd_state, pad);
	else
		return v4l2_subdev_get_try_compose(&viif_dev->isp_subdev.sd, &state, pad);
}

static int visconti_viif_isp_get_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *sd_state,
				     struct v4l2_subdev_format *fmt)
{
	struct viif_device *viif_dev = ((struct isp_subdev *)sd)->viif_dev;

	mutex_lock(&viif_dev->isp_subdev.ops_lock);
	fmt->format = *visconti_viif_isp_get_pad_fmt(sd, sd_state, fmt->pad, fmt->which);
	mutex_unlock(&viif_dev->isp_subdev.ops_lock);

	return 0;
}

static void visconti_viif_isp_set_sink_fmt(struct v4l2_subdev *sd,
					   struct v4l2_subdev_state *sd_state,
					   struct v4l2_mbus_framefmt *format, u32 which)
{
	struct v4l2_mbus_framefmt *sink_fmt, *src0_fmt, *src1_fmt, *src2_fmt;

	sink_fmt = visconti_viif_isp_get_pad_fmt(sd, sd_state, VIIF_ISP_PAD_SINK, which);
	src0_fmt = visconti_viif_isp_get_pad_fmt(sd, sd_state, VIIF_ISP_PAD_SRC_PATH0, which);
	src1_fmt = visconti_viif_isp_get_pad_fmt(sd, sd_state, VIIF_ISP_PAD_SRC_PATH1, which);
	src2_fmt = visconti_viif_isp_get_pad_fmt(sd, sd_state, VIIF_ISP_PAD_SRC_PATH2, which);

	/* update mbus code only if it's available */
	if (viif_is_valid_mbus_code(format->code))
		sink_fmt->code = format->code;

	/* sink::mbus_code is derived from src::mbus_code */
	if (viif_get_mbus_rgb_out(sink_fmt->code)) {
		src0_fmt->code = MEDIA_BUS_FMT_RGB888_1X24;
		src1_fmt->code = MEDIA_BUS_FMT_RGB888_1X24;
	} else {
		src0_fmt->code = MEDIA_BUS_FMT_YUV8_1X24;
		src1_fmt->code = MEDIA_BUS_FMT_YUV8_1X24;
	}

	/* SRC2 (RAW output) follows SINK format */
	src2_fmt->code = format->code;
	src2_fmt->width = format->width;
	src2_fmt->height = format->height;

	/* size check */
	sink_fmt->width = format->width;
	sink_fmt->height = format->height;

	*format = *sink_fmt;
}

static void visconti_viif_isp_set_src_fmt(struct v4l2_subdev *sd,
					  struct v4l2_subdev_state *sd_state,
					  struct v4l2_mbus_framefmt *format, unsigned int pad,
					  u32 which)
{
	struct v4l2_mbus_framefmt *sink_fmt, *src_fmt;
	struct v4l2_rect *src_crop;

	sink_fmt = visconti_viif_isp_get_pad_fmt(sd, sd_state, VIIF_ISP_PAD_SINK,
						 V4L2_SUBDEV_FORMAT_ACTIVE);
	src_fmt = visconti_viif_isp_get_pad_fmt(sd, sd_state, pad, which);
	src_crop = visconti_viif_isp_get_pad_crop(sd, sd_state, pad, which);

	/* sink::mbus_code is derived from src::mbus_code */
	if (viif_get_mbus_rgb_out(sink_fmt->code))
		src_fmt->code = MEDIA_BUS_FMT_RGB888_1X24;
	else
		src_fmt->code = MEDIA_BUS_FMT_YUV8_1X24;

	/*size check*/
	src_fmt->width = format->width;
	src_fmt->height = format->height;

	/*update crop*/
	src_crop->width = format->width;
	src_crop->height = format->height;

	*format = *src_fmt;
}

static void visconti_viif_isp_set_src_fmt_rawpath(struct v4l2_subdev *sd,
						  struct v4l2_subdev_state *sd_state,
						  struct v4l2_mbus_framefmt *format,
						  unsigned int pad, u32 which)
{
	struct v4l2_mbus_framefmt *sink_fmt, *src_fmt;

	sink_fmt = visconti_viif_isp_get_pad_fmt(sd, sd_state, VIIF_ISP_PAD_SINK,
						 V4L2_SUBDEV_FORMAT_ACTIVE);
	src_fmt = visconti_viif_isp_get_pad_fmt(sd, sd_state, pad, which);

	/* RAWPATH SRC pad has just the same configuration as SINK pad */
	src_fmt->code = sink_fmt->code;
	src_fmt->width = sink_fmt->width;
	src_fmt->height = sink_fmt->height;

	*format = *src_fmt;
}

static int visconti_viif_isp_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *sd_state,
				     struct v4l2_subdev_format *fmt)
{
	struct viif_device *viif_dev = ((struct isp_subdev *)sd)->viif_dev;

	mutex_lock(&viif_dev->isp_subdev.ops_lock);

	if (fmt->pad == VIIF_ISP_PAD_SINK)
		visconti_viif_isp_set_sink_fmt(sd, sd_state, &fmt->format, fmt->which);
	else if (fmt->pad == VIIF_ISP_PAD_SRC_PATH2)
		visconti_viif_isp_set_src_fmt_rawpath(sd, sd_state, &fmt->format, fmt->pad,
						      fmt->which);
	else
		visconti_viif_isp_set_src_fmt(sd, sd_state, &fmt->format, fmt->pad, fmt->which);

	mutex_unlock(&viif_dev->isp_subdev.ops_lock);

	return 0;
}

static int visconti_viif_isp_init_config(struct v4l2_subdev *sd, struct v4l2_subdev_state *sd_state)
{
	struct viif_device *viif_dev = ((struct isp_subdev *)sd)->viif_dev;
	struct v4l2_mbus_framefmt *sink_fmt, *src_fmt;
	struct v4l2_rect *src_crop, *sink_compose;

	sink_fmt =
		v4l2_subdev_get_try_format(&viif_dev->isp_subdev.sd, sd_state, VIIF_ISP_PAD_SINK);
	sink_fmt->width = VISCONTI_VIIF_ISP_DEFAULT_WIDTH;
	sink_fmt->height = VISCONTI_VIIF_ISP_DEFAULT_HEIGHT;
	sink_fmt->field = V4L2_FIELD_NONE;
	sink_fmt->code = MEDIA_BUS_FMT_SRGGB10_1X10;

	sink_compose =
		v4l2_subdev_get_try_compose(&viif_dev->isp_subdev.sd, sd_state, VIIF_ISP_PAD_SINK);
	sink_compose->top = 0;
	sink_compose->left = 0;
	sink_compose->width = VISCONTI_VIIF_ISP_DEFAULT_WIDTH;
	sink_compose->height = VISCONTI_VIIF_ISP_DEFAULT_HEIGHT;

	src_fmt = v4l2_subdev_get_try_format(&viif_dev->isp_subdev.sd, sd_state,
					     VIIF_ISP_PAD_SRC_PATH0);
	src_fmt->width = VISCONTI_VIIF_ISP_DEFAULT_WIDTH;
	src_fmt->height = VISCONTI_VIIF_ISP_DEFAULT_HEIGHT;
	src_fmt->field = V4L2_FIELD_NONE;
	src_fmt->code = MEDIA_BUS_FMT_YUV8_1X24;

	src_crop = v4l2_subdev_get_try_crop(&viif_dev->isp_subdev.sd, sd_state,
					    VIIF_ISP_PAD_SRC_PATH0);
	src_crop->top = 0;
	src_crop->left = 0;
	src_crop->width = VISCONTI_VIIF_ISP_DEFAULT_WIDTH;
	src_crop->height = VISCONTI_VIIF_ISP_DEFAULT_HEIGHT;

	src_fmt = v4l2_subdev_get_try_format(&viif_dev->isp_subdev.sd, sd_state,
					     VIIF_ISP_PAD_SRC_PATH1);
	src_fmt->width = VISCONTI_VIIF_ISP_DEFAULT_WIDTH;
	src_fmt->height = VISCONTI_VIIF_ISP_DEFAULT_HEIGHT;
	src_fmt->field = V4L2_FIELD_NONE;
	src_fmt->code = MEDIA_BUS_FMT_YUV8_1X24;

	src_crop = v4l2_subdev_get_try_crop(&viif_dev->isp_subdev.sd, sd_state,
					    VIIF_ISP_PAD_SRC_PATH1);
	src_crop->top = 0;
	src_crop->left = 0;
	src_crop->width = VISCONTI_VIIF_ISP_DEFAULT_WIDTH;
	src_crop->height = VISCONTI_VIIF_ISP_DEFAULT_HEIGHT;

	src_fmt = v4l2_subdev_get_try_format(&viif_dev->isp_subdev.sd, sd_state,
					     VIIF_ISP_PAD_SRC_PATH2);
	src_fmt->width = VISCONTI_VIIF_ISP_DEFAULT_WIDTH;
	src_fmt->height = VISCONTI_VIIF_ISP_DEFAULT_HEIGHT;
	src_fmt->field = V4L2_FIELD_NONE;
	src_fmt->code = MEDIA_BUS_FMT_SRGGB10_1X10;

	return 0;
}

static int visconti_viif_isp_get_selection(struct v4l2_subdev *sd,
					   struct v4l2_subdev_state *sd_state,
					   struct v4l2_subdev_selection *sel)
{
	struct viif_device *viif_dev = ((struct isp_subdev *)sd)->viif_dev;
	struct v4l2_mbus_framefmt *sink_fmt;
	int ret = -EINVAL;

	mutex_lock(&viif_dev->isp_subdev.ops_lock);
	if (sel->pad == VIIF_ISP_PAD_SINK) {
		/* SINK PAD */
		switch (sel->target) {
		case V4L2_SEL_TGT_CROP:
			sink_fmt = visconti_viif_isp_get_pad_fmt(sd, sd_state, VIIF_ISP_PAD_SINK,
								 sel->which);
			sel->r.top = 0;
			sel->r.left = 0;
			sel->r.width = sink_fmt->width;
			sel->r.height = sink_fmt->height;
			ret = 0;
			break;
		case V4L2_SEL_TGT_COMPOSE:
			sel->r = *visconti_viif_isp_get_pad_compose(sd, sd_state, VIIF_ISP_PAD_SINK,
								    sel->which);
			ret = 0;
			break;
		case V4L2_SEL_TGT_COMPOSE_BOUNDS:
			/* fixed value */
			sel->r.top = 0;
			sel->r.left = 0;
			sel->r.width = VISCONTI_VIIF_MAX_COMPOSED_WIDTH;
			sel->r.height = VISCONTI_VIIF_MAX_COMPOSED_HEIGHT;
			ret = 0;
			break;
		}
	} else if ((sel->pad == VIIF_ISP_PAD_SRC_PATH0) || (sel->pad == VIIF_ISP_PAD_SRC_PATH1)) {
		/* SRC PAD */
		switch (sel->target) {
		case V4L2_SEL_TGT_CROP:
			sel->r =
				*visconti_viif_isp_get_pad_crop(sd, sd_state, sel->pad, sel->which);
			ret = 0;
			break;
		}
	}
	mutex_unlock(&viif_dev->isp_subdev.ops_lock);

	return ret;
}

static int visconti_viif_isp_set_selection(struct v4l2_subdev *sd,
					   struct v4l2_subdev_state *sd_state,
					   struct v4l2_subdev_selection *sel)
{
	struct viif_device *viif_dev = ((struct isp_subdev *)sd)->viif_dev;
	struct v4l2_rect *rect, *rect_compose;
	struct v4l2_mbus_framefmt *src_fmt;
	int ret = -EINVAL;

	mutex_lock(&viif_dev->isp_subdev.ops_lock);
	/* only source::selection::crop is writable */
	if (sel->pad == VIIF_ISP_PAD_SRC_PATH0 || sel->pad == VIIF_ISP_PAD_SRC_PATH1) {
		switch (sel->target) {
		case V4L2_SEL_TGT_CROP: {
			/* check if new SRC::CROP is inside SINK::COMPOSE */
			rect_compose = visconti_viif_isp_get_pad_compose(
				sd, sd_state, VIIF_ISP_PAD_SINK, sel->which);
			if (sel->r.top < rect_compose->top || sel->r.left < rect_compose->left ||
			    (sel->r.top + sel->r.height) >
				    (rect_compose->top + rect_compose->height) ||
			    (sel->r.left + sel->r.width) >
				    (rect_compose->left + rect_compose->width)) {
				break;
			}

			rect = visconti_viif_isp_get_pad_crop(sd, sd_state, sel->pad, sel->which);
			*rect = sel->r;

			/* update SRC::FMT along with SRC::CROP */
			src_fmt = visconti_viif_isp_get_pad_fmt(sd, sd_state, sel->pad, sel->which);
			src_fmt->width = sel->r.width;
			src_fmt->height = sel->r.height;
			ret = 0;
			break;
		}
		}
	}
	mutex_unlock(&viif_dev->isp_subdev.ops_lock);

	return ret;
}

void visconti_viif_isp_set_compose_rect(struct viif_device *viif_dev,
					struct viif_l2_roi_config *roi)
{
	struct v4l2_rect *rect;

	rect = visconti_viif_isp_get_pad_compose(&viif_dev->isp_subdev.sd, NULL, VIIF_ISP_PAD_SINK,
						 V4L2_SUBDEV_FORMAT_ACTIVE);
	rect->top = 0;
	rect->left = 0;
	rect->width = roi->corrected_hsize[0];
	rect->height = roi->corrected_vsize[0];
}

static const struct media_entity_operations visconti_viif_isp_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_pad_ops visconti_viif_isp_pad_ops = {
	.enum_mbus_code = visconti_viif_isp_enum_mbus_code,
	.get_selection = visconti_viif_isp_get_selection,
	.set_selection = visconti_viif_isp_set_selection,
	.init_cfg = visconti_viif_isp_init_config,
	.get_fmt = visconti_viif_isp_get_fmt,
	.set_fmt = visconti_viif_isp_set_fmt,
	.link_validate = v4l2_subdev_link_validate_default,
};

static const struct v4l2_subdev_video_ops visconti_viif_isp_video_ops = {
	.s_stream = visconti_viif_isp_s_stream,
};

static const struct v4l2_subdev_ops visconti_viif_isp_ops = {
	.video = &visconti_viif_isp_video_ops,
	.pad = &visconti_viif_isp_pad_ops,
};

/* ----- register/remove isp subdevice node ----- */
int visconti_viif_isp_register(struct viif_device *viif_dev)
{
	struct media_pad *pads = viif_dev->isp_subdev.pads;
	struct v4l2_subdev *sd = &viif_dev->isp_subdev.sd;
	struct v4l2_subdev_state state = {
		.pads = viif_dev->isp_subdev.pad_cfg,
	};
	int ret;

	viif_dev->isp_subdev.viif_dev = viif_dev;

	v4l2_subdev_init(sd, &visconti_viif_isp_ops);
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->entity.ops = &visconti_viif_isp_media_ops;
	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_SCALER;
	sd->owner = THIS_MODULE;
	strscpy(sd->name, "visconti-viif:isp", sizeof(sd->name));

	pads[0].flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	pads[1].flags = MEDIA_PAD_FL_SOURCE | MEDIA_PAD_FL_MUST_CONNECT;
	pads[2].flags = MEDIA_PAD_FL_SOURCE | MEDIA_PAD_FL_MUST_CONNECT;
	pads[3].flags = MEDIA_PAD_FL_SOURCE | MEDIA_PAD_FL_MUST_CONNECT;

	mutex_init(&viif_dev->isp_subdev.ops_lock);

	visconti_viif_isp_init_controls(viif_dev);

	ret = media_entity_pads_init(&sd->entity, 4, pads);
	if (ret) {
		dev_err(viif_dev->dev, "Failed on media_entity_pads_init\n");
		return ret;
	}

	ret = v4l2_device_register_subdev(&viif_dev->v4l2_dev, sd);
	if (ret) {
		dev_err(viif_dev->dev, "Failed to resize ISP subdev\n");
		goto err_cleanup_media_entity;
	}

	visconti_viif_isp_init_config(sd, &state);

	return 0;

err_cleanup_media_entity:
	media_entity_cleanup(&sd->entity);
	return ret;
}

void visconti_viif_isp_unregister(struct viif_device *viif_dev)
{
	v4l2_device_unregister_subdev(&viif_dev->isp_subdev.sd);
	media_entity_cleanup(&viif_dev->isp_subdev.sd.entity);
}
