/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#ifndef QCOM_JENC_DEFS_H
#define QCOM_JENC_DEFS_H

#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/types.h>
#include <linux/videodev2.h>
#include <media/videobuf2-core.h>

/* Offline JPEG encoder constraints */
#define QCOM_JPEG_HW_MAX_WIDTH	8192
#define QCOM_JPEG_HW_MAX_HEIGHT	8192
#define QCOM_JPEG_HW_MIN_WIDTH	256
#define QCOM_JPEG_HW_MIN_HEIGHT	256

#define QCOM_JPEG_HW_DEF_HSTEP	16
#define QCOM_JPEG_HW_DEF_VSTEP	16

#define QCOM_JPEG_HW_DEF_WIDTH	1920
#define QCOM_JPEG_HW_DEF_HEIGHT	1088

#define QCOM_JPEG_MAX_PLANES	3

#define QCOM_JPEG_QUALITY_MIN	1
#define QCOM_JPEG_QUALITY_DEF	98
#define QCOM_JPEG_QUALITY_MAX	100
#define QCOM_JPEG_QUALITY_MID	(QCOM_JPEG_QUALITY_MAX / 2)
#define QCOM_JPEG_QUALITY_UNT	1

/*
 * V4L2_CID_QCOM_JPEG_PERF_LEVEL_AUTO - enable adaptive performance scaling.
 *
 * When set to 1 the driver selects the core clock OPP level based on the
 * encoded frame resolution and quality factor.  When set to 0 (default) the
 * driver always runs at NOMINAL (highest) OPP level, trading power for
 * deterministic latency.
 *
 * Accessible via v4l2-ctl:
 *   v4l2-ctl --set-ctrl=perf_level_auto=1
 */
#define V4L2_CID_QCOM_JPEG_PERF_LEVEL_AUTO \
	(V4L2_CID_USER_BASE + 0x1240)

/*
 * V4L2_CID_QCOM_JPEG_FPS_TARGET - target encode rate in frames per second.
 *
 * Used together with V4L2_CID_QCOM_JPEG_PERF_LEVEL_AUTO to select the lowest
 * OPP level whose throughput is sufficient for the requested frame rate.
 * Has no effect when perf_level_auto is 0.
 *
 * Range: 1 to 240 fps. Default: 30.
 *
 * Accessible via v4l2-ctl:
 *   v4l2-ctl --set-ctrl=fps_target=60
 */
#define V4L2_CID_QCOM_JPEG_FPS_TARGET \
	(V4L2_CID_USER_BASE + 0x1241)

#define QCOM_JPEG_FPS_MIN	1
#define QCOM_JPEG_FPS_MAX	240
#define QCOM_JPEG_FPS_DEF	30
#define QCOM_JPEG_FPS_UNT	1

#define JPEG_CLK_LOWSVS_HZ	300000000ULL
#define JPEG_CLK_SVS_HZ		400000000ULL
#define JPEG_CLK_SVS_L1_HZ	480000000ULL
#define JPEG_CLK_NOMINAL_HZ	600000000ULL

/*
 * Reference measurements:
 *
 *  - JPEG clock      : 600 MHz
 *  - Input           : Worst-case (NOISE)
 *
 * Although ~120 MPixel/s was measured, use a conservative value of
 * 110 MPixel/s to provide operating margin.
 */
#define JPEG_REF_CLK_HZ             JPEG_CLK_NOMINAL_HZ
#define JPEG_REF_THROUGHPUT_MPPS    110ULL
#define JPEG_REF_PIXEL_RATE         (JPEG_REF_THROUGHPUT_MPPS * 1000000ULL)

/*
 * Performance levels for the JPEG core clock, used as level values.
 * The numeric values must match the opp-level entries in the DTS node:
 *
 *   LOWSVS  = 0  -> opp-level = <0>  (e.g. 300 MHz on SM8250)
 *   SVS     = 1  -> opp-level = <1>  (e.g. 400 MHz on SM8250)
 *   SVS_L1  = 2  -> opp-level = <2>  (e.g. 480 MHz on SM8250)
 *   NOMINAL = 3  -> opp-level = <3>  (e.g. 600 MHz on SM8250)
 */
enum qcom_soc_perf_level {
	QCOM_SOC_PERF_LOWSVS = 0,
	QCOM_SOC_PERF_SVS,
	QCOM_SOC_PERF_SVS_L1,
	QCOM_SOC_PERF_NOMINAL
};

/* hardware register field mask identifiers */
enum qcom_jpeg_mask_id {
	/* hW version fields */
	JMSK_HW_VER_STEP,
	JMSK_HW_VER_MINOR,
	JMSK_HW_VER_MAJOR,

	/* hW capability fields */
	JMSK_HW_CAP_ENCODE,
	JMSK_HW_CAP_DECODE,
	JMSK_HW_CAP_UPSCALE,
	JMSK_HW_CAP_DOWNSCALE,

	/* reset command fields for individual JPEG subsystems */
	JMSK_RST_CMD_COMMON,
	JMSK_RST_CMD_FE_RESET,
	JMSK_RST_CMD_WE_RESET,
	JMSK_RST_CMD_ENCODER_RESET,
	JMSK_RST_CMD_DECODER_RESET,
	JMSK_RST_CMD_BLOCK_FORMATTER_RST,
	JMSK_RST_CMD_SCALE_RESET,
	JMSK_RST_CMD_REGISTER_RESET,
	JMSK_RST_CMD_MISR_RESET,
	JMSK_RST_CMD_CORE_RESET,
	JMSK_RST_CMD_JMSK_DOMAIN_RESET,
	JMSK_RST_CMD_RESET_BYPASS,

	/* hW command fields */
	JMSK_CMD_HW_START,
	JMSK_CMD_HW_STOP,
	JMSK_CMD_CLR_RD_PLNS_QUEUE,
	JMSK_CMD_CLR_WR_PLNS_QUEUE,
	JMSK_CMD_APPLY_SWC_RD_PARAMS,

	/* core configuration fields */
	JMSK_CORE_CFG_FE_ENABLE,
	JMSK_CORE_CFG_WE_ENABLE,
	JMSK_CORE_CFG_ENC_ENABLE,
	JMSK_CORE_CFG_SCALE_ENABLE,
	JMSK_CORE_CFG_TESTBUS_ENABLE,
	JMSK_CORE_CFG_MODE,
	JMSK_CORE_CFG_CGC_DISABLE,

	/* core status fields */
	JMSK_CORE_STATUS_ENCODE_STATE,
	JMSK_CORE_STATUS_SCALE_STATE,
	JMSK_CORE_STATUS_RT_STATE,
	JMSK_CORE_STATUS_BUS_STATE,
	JMSK_CORE_STATUS_CGC_STATE,

	/* interrupt status fields */
	JMSK_IRQ_STATUS_SESSION_DONE,
	JMSK_IRQ_STATUS_RD_BUF_PLN0_DONE,
	JMSK_IRQ_STATUS_RD_BUF_PLN1_DONE,
	JMSK_IRQ_STATUS_RD_BUF_PLN2_DONE,
	JMSK_IRQ_STATUS_RD_BUF_PLNS_ATTN,
	JMSK_IRQ_STATUS_WR_BUF_PLN0_DONE,
	JMSK_IRQ_STATUS_WR_BUF_PLN1_DONE,
	JMSK_IRQ_STATUS_WR_BUF_PLN2_DONE,
	JMSK_IRQ_STATUS_WR_BUF_PLNS_ATTN,
	JMSK_IRQ_STATUS_SESSION_ERROR,
	JMSK_IRQ_STATUS_STOP_ACK,
	JMSK_IRQ_STATUS_RESET_ACK,

	/* combined mask for clearing all interrupt status bits */
	JMSK_IRQ_STATUS_ALL_BITS,

	/* fetch engine (FE) configuration fields */
	JMSK_FE_CFG_BYTE_ORDERING,
	JMSK_FE_CFG_BURST_LENGTH_MAX,
	JMSK_FE_CFG_MEMORY_FORMAT,
	JMSK_FE_CFG_CBCR_ORDER,
	JMSK_FE_CFG_BOTTOM_VPAD_EN,
	JMSK_FE_CFG_PLN0_EN,
	JMSK_FE_CFG_PLN1_EN,
	JMSK_FE_CFG_PLN2_EN,
	JMSK_FE_CFG_SIXTEEN_MCU_EN,
	JMSK_FE_CFG_MCUS_PER_BLOCK,
	JMSK_FE_CFG_MAL_BOUNDARY,
	JMSK_FE_CFG_MAL_EN,

	JMSK_FE_VBPAD_CFG_BLOCK_ROW,
	JMSK_FE_VBPAD_CFG,

	/* fetch engine plane address/geometry fields */
	JMSK_PLNS_RD_OFFSET,
	JMSK_PLNS_RD_BUF_SIZE_WIDTH,
	JMSK_PLNS_RD_BUF_SIZE_HEIGHT,
	JMSK_PLNS_RD_STRIDE,
	JMSK_PLNS_RD_HINIT,
	JMSK_PLNS_RD_VINIT,
	JMSK_PLN0_RD_HINIT_INT,
	JMSK_PLN1_RD_HINIT_INT,
	JMSK_PLN2_RD_HINIT_INT,
	JMSK_PLN0_RD_VINIT_INT,
	JMSK_PLN1_RD_VINIT_INT,
	JMSK_PLN2_RD_VINIT_INT,

	/* write engine (WE) configuration fields */
	JMSK_WE_CFG_BYTE_ORDERING,
	JMSK_WE_CFG_BURST_LENGTH_MAX,
	JMSK_WE_CFG_MEMORY_FORMAT,
	JMSK_WE_CFG_CBCR_ORDER,
	JMSK_WE_CFG_PLN0_EN,
	JMSK_WE_CFG_PLN1_EN,
	JMSK_WE_CFG_PLN2_EN,
	JMSK_WE_CFG_MAL_BOUNDARY,
	JMSK_WE_CFG_MAL_EN,
	JMSK_WE_CFG_POP_BUFF_ON_EOS,

	/* write engine plane address/geometry fields */
	JMSK_PLNS_WR_BUF_SIZE_WIDTH,
	JMSK_PLNS_WR_BUF_SIZE_HEIGHT,
	JMSK_PLNS_WR_STRIDE,
	JMSK_PLNS_WR_HINIT,
	JMSK_PLNS_WR_VINIT,
	JMSK_PLNS_WR_HSTEP,
	JMSK_PLNS_WR_VSTEP,
	JMSK_PLNS_WR_BLOCK_CFG_PER_COL,
	JMSK_PLNS_WR_BLOCK_CFG_PER_RAW,

	/* encoder configuration fields */
	JMSK_ENC_CFG_IMAGE_FORMAT,
	JMSK_ENC_CFG_APPLY_EOI,
	JMSK_ENC_CFG_HUFFMAN_SEL,
	JMSK_ENC_CFG_FSC_ENABLE,
	JMSK_ENC_CFG_OUTPUT_DISABLE,
	JMSK_ENC_CFG_RST_MARKER_PERIOD,
	JMSK_ENC_IMAGE_SIZE_WIDTH,
	JMSK_ENC_IMAGE_SIZE_HEIGHT,

	/* scaler configuration fields */
	JMSK_SCALE_CFG_HSCALE_ENABLE,
	JMSK_SCALE_CFG_VSCALE_ENABLE,
	JMSK_SCALE_CFG_UPSAMPLE_EN,
	JMSK_SCALE_CFG_SUBSAMPLE_EN,
	JMSK_SCALE_CFG_HSCALE_ALGO,
	JMSK_SCALE_CFG_VSCALE_ALGO,
	JMSK_SCALE_CFG_H_SCALE_FIR_ALGO,
	JMSK_SCALE_CFG_V_SCALE_FIR_ALGO,
	JMSK_SCALE_PLNS_OUT_CFG_BLK_WIDTH,
	JMSK_SCALE_PLNS_OUT_CFG_BLK_HEIGHT,
	JMSK_SCALE_PLNS_HSTEP_FRACTIONAL,
	JMSK_SCALE_PLNS_HSTEP_INTEGER,
	JMSK_SCALE_PLNS_VSTEP_FRACTIONAL,
	JMSK_SCALE_PLNS_VSTEP_INTEGER,

	/* dMI table configuration and write fields */
	JMSK_DMI_CFG,
	JMSK_DMI_ADDR,
	JMSK_DMI_DATA,

	JMSK_ID_MAX
};

struct qcom_jpeg_reg_offs {
	u32 hw_version;
	u32 hw_capability;
	u32 reset_cmd;
	u32 core_cfg;
	u32 int_mask;
	u32 int_clr;
	u32 int_status;
	u32 hw_cmd;
	u32 enc_core_state;

	struct {
		u32 pntr[QCOM_JPEG_MAX_PLANES];
		u32 offs[QCOM_JPEG_MAX_PLANES];
		u32 bsize[QCOM_JPEG_MAX_PLANES];
		u32 stride[QCOM_JPEG_MAX_PLANES];
		u32 hinit[QCOM_JPEG_MAX_PLANES];
		u32 vinit[QCOM_JPEG_MAX_PLANES];
		u32 pntr_cnt;
		u32 vbpad_cfg;
	} fe;
	u32 fe_cfg;

	struct {
		u32 pntr[QCOM_JPEG_MAX_PLANES];
		u32 bsize[QCOM_JPEG_MAX_PLANES];
		u32 stride[QCOM_JPEG_MAX_PLANES];
		u32 hinit[QCOM_JPEG_MAX_PLANES];
		u32 hstep[QCOM_JPEG_MAX_PLANES];
		u32 vinit[QCOM_JPEG_MAX_PLANES];
		u32 vstep[QCOM_JPEG_MAX_PLANES];
		u32 blocks[QCOM_JPEG_MAX_PLANES];
		u32 pntr_cnt;
	} we;
	u32 we_cfg;

	struct {
		u32 hstep[QCOM_JPEG_MAX_PLANES];
		u32 vstep[QCOM_JPEG_MAX_PLANES];
	} scale;
	u32 scale_cfg;
	u32 scale_out_cfg[QCOM_JPEG_MAX_PLANES];

	u32 enc_cfg;
	u32 enc_img_size;
	u32 enc_out_size;

	u32 dmi_cfg;
	u32 dmi_data;
	u32 dmi_addr;
};

#endif /* QCOM_JENC_DEFS_H */
