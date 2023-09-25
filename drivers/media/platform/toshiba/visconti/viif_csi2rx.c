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
#include "viif_csi2rx.h"
#include "viif_csi2rx_regs.h"
#include "viif_regs.h"

/* specifies which D-PHY line(L0-L3) is assigned to D0-D3 */
enum viif_csi2rx_dphy_lane {
	VIIF_CSI2RX_DPHY_L0L1L2L3 = 0U,
	VIIF_CSI2RX_DPHY_L0L3L1L2 = 1U,
	VIIF_CSI2RX_DPHY_L0L2L3L1 = 2U,
	VIIF_CSI2RX_DPHY_L0L1L3L2 = 4U,
	VIIF_CSI2RX_DPHY_L0L3L2L1 = 5U,
	VIIF_CSI2RX_DPHY_L0L2L1L3 = 6U
};

#define VIIF_CSI2RX_ERROR_MONITORS_NUM 8

/**
 * struct viif_csi2rx_line_err_target
 *
 * Virtual Channel and Data Type pair for CSI2RX line error monitor
 *
 * When 0 is set to dt, line error detection is disabled.
 *
 * @vc: Virtual Channel to monitor; Range 0..3
 * @dt: Data Type to monitor; Range 0, 0x10..0x3F
 */
struct viif_csi2rx_line_err_target {
	u32 vc[VIIF_CSI2RX_ERROR_MONITORS_NUM];
	u32 dt[VIIF_CSI2RX_ERROR_MONITORS_NUM];
};

#define VIIF_CSI2RX_MAX_VC	3U
#define VIIF_DPHY_MIN_DATA_RATE 80U
#define VIIF_DPHY_MAX_DATA_RATE 1500U
#define VIIF_DPHY_CFG_CLK_25M	32U

#define VIIF_CSI2RX_DEF_WIDTH  1920
#define VIIF_CSI2RX_DEF_HEIGHT 1080
#define VIIF_CSI2RX_DEF_FMT    MEDIA_BUS_FMT_SRGGB10_1X10
#define VIIF_ISP_MIN_WIDTH     640
#define VIIF_ISP_MAX_WIDTH     3840
#define VIIF_ISP_MIN_HEIGHT    480
#define VIIF_ISP_MAX_HEIGHT    2160

/*=============================================*/
/* Register Access */
/*=============================================*/
static inline void viif_csi2rx_write(struct viif_device *viif_dev, u32 regid, u32 val)
{
	writel(val, viif_dev->csi2host_reg + regid);
}

static inline u32 viif_csi2rx_read(struct viif_device *viif_dev, u32 regid)
{
	return readl(viif_dev->csi2host_reg + regid);
}

/*=============================================*/
/* DPHY control commands via test register */
/*=============================================*/
static inline void tick_testclk(struct viif_device *viif_dev)
{
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_TESTCTRL0, BIT_TESTCTRL0_CLK_1);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_TESTCTRL0, BIT_TESTCTRL0_CLK_0);
}

static inline void set_dphy_addr(struct viif_device *viif_dev, u32 test_mode)
{
	/* select testcode Ex space with top 4bits of test_mode */
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_TESTCTRL1,
			  BIT_TESTCTRL1_ADDR | DIG_TESTCODE_EXT);
	tick_testclk(viif_dev);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_TESTCTRL1, FIELD_GET(0xF00, test_mode));
	tick_testclk(viif_dev);

	/* set bottom 8bit of test_mode */
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_TESTCTRL1,
			  BIT_TESTCTRL1_ADDR | FIELD_GET(0xFF, test_mode));
	tick_testclk(viif_dev);
}

static void write_dphy_param(struct viif_device *viif_dev, u32 test_mode, u8 test_in)
{
	set_dphy_addr(viif_dev, test_mode);

	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_TESTCTRL1, (u32)test_in);
	tick_testclk(viif_dev);
}

static u8 read_dphy_param(u32 test_mode, struct viif_device *viif_dev)
{
	u32 read_data;

	set_dphy_addr(viif_dev, test_mode);

	read_data = viif_csi2rx_read(viif_dev, REG_CSI2RX_PHY_TESTCTRL1);
	return (u8)(FIELD_GET(MASK_TESTCTRL1_DIN, read_data));
}

/*=============================================*/
/* DPHY configuration */
/*=============================================*/
/**
 * struct viif_dphy_hs_info - dphy hs information
 *
 * @rate: Data rate [Mbps]
 * @hsfreqrange: IP operating frequency(hsfreqrange)
 * @osc_freq_target: DDL target oscillation frequency(osc_freq_target)
 */
struct viif_dphy_hs_info {
	u32 rate;
	u32 hsfreqrange;
	u32 osc_freq_target;
};

static const struct viif_dphy_hs_info dphy_hs_info[] = {
	{ 80, 0x0, 0x1cc },   { 85, 0x10, 0x1cc },   { 95, 0x20, 0x1cc },   { 105, 0x30, 0x1cc },
	{ 115, 0x1, 0x1cc },  { 125, 0x11, 0x1cc },  { 135, 0x21, 0x1cc },  { 145, 0x31, 0x1cc },
	{ 155, 0x2, 0x1cc },  { 165, 0x12, 0x1cc },  { 175, 0x22, 0x1cc },  { 185, 0x32, 0x1cc },
	{ 198, 0x3, 0x1cc },  { 213, 0x13, 0x1cc },  { 228, 0x23, 0x1cc },  { 243, 0x33, 0x1cc },
	{ 263, 0x4, 0x1cc },  { 288, 0x14, 0x1cc },  { 313, 0x25, 0x1cc },  { 338, 0x35, 0x1cc },
	{ 375, 0x5, 0x1cc },  { 425, 0x16, 0x1cc },  { 475, 0x26, 0x1cc },  { 525, 0x37, 0x1cc },
	{ 575, 0x7, 0x1cc },  { 625, 0x18, 0x1cc },  { 675, 0x28, 0x1cc },  { 725, 0x39, 0x1cc },
	{ 775, 0x9, 0x1cc },  { 825, 0x19, 0x1cc },  { 875, 0x29, 0x1cc },  { 925, 0x3a, 0x1cc },
	{ 975, 0xa, 0x1cc },  { 1025, 0x1a, 0x1cc }, { 1075, 0x2a, 0x1cc }, { 1125, 0x3b, 0x1cc },
	{ 1175, 0xb, 0x1cc }, { 1225, 0x1b, 0x1cc }, { 1275, 0x2b, 0x1cc }, { 1325, 0x3c, 0x1cc },
	{ 1375, 0xc, 0x1cc }, { 1425, 0x1c, 0x1cc }, { 1475, 0x2c, 0x1cc }
};

/**
 * get_dphy_hs_transfer_info() - Get DPHY HS info from table
 *
 * @dphy_rate: DPHY clock in MHz
 * @hsfreqrange: HS Frequency Range
 * @osc_freq_target: OSC Frequency Target
 */
static void get_dphy_hs_transfer_info(u32 dphy_rate, u32 *hsfreqrange, u32 *osc_freq_target)
{
	unsigned int i;

	for (i = 1; i < ARRAY_SIZE(dphy_hs_info); i++) {
		if (dphy_rate < dphy_hs_info[i].rate) {
			*hsfreqrange = dphy_hs_info[i - 1].hsfreqrange;
			*osc_freq_target = dphy_hs_info[i - 1].osc_freq_target;
			return;
		}
	}

	/* not found; return the largest entry */
	*hsfreqrange = dphy_hs_info[ARRAY_SIZE(dphy_hs_info) - 1].hsfreqrange;
	*osc_freq_target = dphy_hs_info[ARRAY_SIZE(dphy_hs_info) - 1].osc_freq_target;
}

/**
 * viif_csi2rx_set_dphy_rate() - Set D-PHY rate
 *
 * @viif_dev: the VIIF device
 * @dphy_rate: D-PHY rate of 1 Lane [Unit: Mbps]. Range: [80..1500]
 */
static void viif_csi2rx_set_dphy_rate(struct viif_device *viif_dev, u32 dphy_rate)
{
	u32 hsfreqrange, osc_freq_target;

	get_dphy_hs_transfer_info(dphy_rate, &hsfreqrange, &osc_freq_target);

	write_dphy_param(viif_dev, DIG_SYS_1, (u8)hsfreqrange);
	write_dphy_param(viif_dev, DIG_SYS_0, SYS_0_HSFREQRANGE_OVR);
	write_dphy_param(viif_dev, DIG_RX_STARTUP_OVR_5, STARTUP_OVR_5_BYPASS);
	write_dphy_param(viif_dev, DIG_RX_STARTUP_OVR_4, STARTUP_OVR_4_CNTVAL);
	write_dphy_param(viif_dev, DIG_CB_2, CB_2_LPRX_BIAS | CB_2_RESERVED);
	write_dphy_param(viif_dev, DIG_SYS_7, SYS_7_DESKEW_POL | SYS_7_RESERVED);
	write_dphy_param(viif_dev, DIG_CLKLANE_LANE_6, CLKLANE_RXHS_PULL_LONG);
	write_dphy_param(viif_dev, DIG_RX_STARTUP_OVR_2, FIELD_GET(0xFF, osc_freq_target));
	write_dphy_param(viif_dev, DIG_RX_STARTUP_OVR_3, FIELD_GET(0xF00, osc_freq_target));
	write_dphy_param(viif_dev, DIG_RX_STARTUP_OVR_4,
			 STARTUP_OVR_4_CNTVAL | STARTUP_OVR_4_DDL_EN);

	viif_capture_write(viif_dev, REG_DPHY_FREQRANGE, VIIF_DPHY_CFG_CLK_25M);
}

/**
 * check_dphy_calibration_status() - Check D-PHY calibration status
 *
 * @viif_dev: the VIIF device
 * @test_mode: test code related to calibration information
 * @mask_err: mask for error bit (0 for absence)
 * @mask_done: mask for done bit
 * Return: 0 for success, -EAGAIN for not done, -EIO for failure
 */
static int check_dphy_calibration_status(struct viif_device *viif_dev, u32 test_mode, u32 mask_err,
					 u32 mask_done)
{
	u32 read_data = (u32)read_dphy_param(test_mode, viif_dev);

	if (!(read_data & mask_done))
		return -EAGAIN;

	/* done with error */
	if (read_data & mask_err)
		return -EIO;

	return 0;
}

/*=============================================*/
/* Low Layer Implementation */
/*=============================================*/
/**
 * viif_csi2rx_initialize() - Initialize CSI-2 RX driver
 *
 * @viif_dev: the VIIF device
 * @num_lane: Range: [1..4]
 * @lane_assign: lane connection. For more refer @ref viif_dphy_lane_assignment
 * @dphy_rate: D-PHY rate of 1 Lane [Unit: Mbps]. Range: [80..1500]
 * @rext_calibration: set True to enable rext calibration, False to disable.
 * @err_target: Pointer to configuration for Line error detection.
 * Return: 0 for success, -EINVAL for parameter error
 */
static int viif_csi2rx_initialize(struct viif_device *viif_dev, u32 num_lane,
				  enum viif_csi2rx_dphy_lane lane_assign, u32 dphy_rate,
				  bool rext_calibration,
				  const struct viif_csi2rx_line_err_target *err_target)
{
	u32 i, val;

	if (num_lane == 0U || num_lane > 4U || lane_assign > VIIF_CSI2RX_DPHY_L0L2L1L3)
		return -EINVAL;

	if (dphy_rate < VIIF_DPHY_MIN_DATA_RATE || dphy_rate > VIIF_DPHY_MAX_DATA_RATE ||
	    !err_target) {
		return -EINVAL;
	}

	for (i = 0; i < VIIF_CSI2RX_ERROR_MONITORS_NUM; i++) {
		if (err_target->vc[i] > VIIF_CSI2RX_MAX_VC ||
		    err_target->dt[i] > MIPI_CSI2_DT_USER_DEFINED(7) ||
		    (err_target->dt[i] < MIPI_CSI2_DT_NULL && err_target->dt[i])) {
			return -EINVAL;
		}
	}

	/* 1st phase of initialization */
	viif_csi2rx_write(viif_dev, REG_CSI2RX_RESETN, 1);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_RSTZ, 0);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_SHUTDOWNZ, 0);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_TESTCTRL0, 1);
	ndelay(15U);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_TESTCTRL0, 0);

	/* Configure D-PHY frequency range */
	viif_csi2rx_set_dphy_rate(viif_dev, dphy_rate);

	/* 2nd phase of initialization */
	viif_csi2rx_write(viif_dev, REG_CSI2RX_NLANES, (num_lane - 1U));
	ndelay(5U);

	/* configuration not to use rext */
	if (!rext_calibration) {
		write_dphy_param(viif_dev, DIG_SYS_3, SYS_3_NO_REXT);
		ndelay(5U);
	}

	/* Release D-PHY from Reset */
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_SHUTDOWNZ, 1);
	ndelay(5U);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_RSTZ, 1);

	/* configuration of line error target */
	val = (err_target->vc[3] << 30U) | (err_target->dt[3] << 24U) | (err_target->vc[2] << 22U) |
	      (err_target->dt[2] << 16U) | (err_target->vc[1] << 14U) | (err_target->dt[1] << 8U) |
	      (err_target->vc[0] << 6U) | (err_target->dt[0]);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_DATA_IDS_1, val);
	val = (err_target->vc[7] << 30U) | (err_target->dt[7] << 24U) | (err_target->vc[6] << 22U) |
	      (err_target->dt[6] << 16U) | (err_target->vc[5] << 14U) | (err_target->dt[5] << 8U) |
	      (err_target->vc[4] << 6U) | (err_target->dt[4]);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_DATA_IDS_2, val);

	/* configuration of mask */
	viif_csi2rx_write(viif_dev, REG_CSI2RX_INT_MSK_PHY_FATAL, MASK_PHY_FATAL_ALL);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_INT_MSK_PKT_FATAL, MASK_PKT_FATAL_ALL);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_INT_MSK_FRAME_FATAL, MASK_FRAME_FATAL_ALL);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_INT_MSK_PHY, MASK_PHY_ERROR_ALL);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_INT_MSK_PKT, MASK_PKT_ERROR_ALL);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_INT_MSK_LINE, MASK_LINE_ERROR_ALL);

	/* configuration of lane assignment */
	viif_capture_write(viif_dev, REG_DPHY_LANE, lane_assign);

	return 0;
}

/**
 * viif_csi2rx_uninitialize() - Uninitialize CSI-2 RX driver
 *
 * @viif_dev: the VIIF device
 */
static int viif_csi2rx_uninitialize(struct viif_device *viif_dev)
{
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_SHUTDOWNZ, 0);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_RSTZ, 0);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_PHY_TESTCTRL0, 1);
	viif_csi2rx_write(viif_dev, REG_CSI2RX_RESETN, 0);

	return 0;
}

/*=============================================*/
/* handling vendor specific control requests */
/*=============================================*/
/**
 * visconti_viif_csi2rx_get_calibration_status() - Get CSI-2 RX calibration status
 *
 * @viif_dev: the VIIF device
 * @status: Pointer to D-PHY calibration status information
 * Return: 0 Operation completes successfully
 */
int visconti_viif_csi2rx_get_calibration_status(struct viif_device *viif_dev,
						struct viif_csi2rx_dphy_calibration_status *status)
{
	/* termination calibration with REXT */
	status->term_cal_with_rext = check_dphy_calibration_status(
		viif_dev, DIG_TERM_CAL_1, MASK_TERM_CAL_ERR, MASK_TERM_CAL_DONE);

	/* offset calibration */
	status->clock_lane_offset_cal = check_dphy_calibration_status(
		viif_dev, DIG_CLKLANE_OFFSET_CAL_0, MASK_CLK_CAL_ERR, MASK_CLK_CAL_DONE);
	status->data_lane0_offset_cal = check_dphy_calibration_status(
		viif_dev, DIG_LANE0_OFFSET_CAL_0, MASK_CAL_ERR, MASK_CAL_DONE);
	status->data_lane1_offset_cal = check_dphy_calibration_status(
		viif_dev, DIG_LANE1_OFFSET_CAL_0, MASK_CAL_ERR, MASK_CAL_DONE);
	status->data_lane2_offset_cal = check_dphy_calibration_status(
		viif_dev, DIG_LANE2_OFFSET_CAL_0, MASK_CAL_ERR, MASK_CAL_DONE);
	status->data_lane3_offset_cal = check_dphy_calibration_status(
		viif_dev, DIG_LANE3_OFFSET_CAL_0, MASK_CAL_ERR, MASK_CAL_DONE);

	/* Digital Delay Line calibration */
	status->data_lane0_ddl_tuning_cal = check_dphy_calibration_status(
		viif_dev, DIG_LANE0_DDL_0, MASK_DDL_ERR, MASK_DDL_DONE);
	status->data_lane1_ddl_tuning_cal = check_dphy_calibration_status(
		viif_dev, DIG_LANE1_DDL_0, MASK_DDL_ERR, MASK_DDL_DONE);
	status->data_lane2_ddl_tuning_cal = check_dphy_calibration_status(
		viif_dev, DIG_LANE2_DDL_0, MASK_DDL_ERR, MASK_DDL_DONE);
	status->data_lane3_ddl_tuning_cal = check_dphy_calibration_status(
		viif_dev, DIG_LANE3_DDL_0, MASK_DDL_ERR, MASK_DDL_DONE);

	return 0;
}

/**
 * visconti_viif_csi2rx_get_err_status() - Get CSI-2 RX error status
 *
 * @viif_dev: the VIIF device
 * @csi_err: error information
 * Return: 0 Operation completes successfully
 */
int visconti_viif_csi2rx_get_err_status(struct viif_device *viif_dev,
					struct viif_csi2rx_err_status *csi_err)
{
	csi_err->err_phy_fatal = viif_csi2rx_read(viif_dev, REG_CSI2RX_INT_ST_PHY_FATAL);
	csi_err->err_pkt_fatal = viif_csi2rx_read(viif_dev, REG_CSI2RX_INT_ST_PKT_FATAL);
	csi_err->err_frame_fatal = viif_csi2rx_read(viif_dev, REG_CSI2RX_INT_ST_FRAME_FATAL);
	csi_err->err_phy = viif_csi2rx_read(viif_dev, REG_CSI2RX_INT_ST_PHY);
	csi_err->err_pkt = viif_csi2rx_read(viif_dev, REG_CSI2RX_INT_ST_PKT);
	csi_err->err_line = viif_csi2rx_read(viif_dev, REG_CSI2RX_INT_ST_LINE);

	return 0;
}

/* IRQ handler: reports CSI-2 RX error status */
u32 visconti_viif_csi2rx_err_irq_handler(struct viif_device *viif_dev)
{
	return viif_csi2rx_read(viif_dev, REG_CSI2RX_INT_ST_MAIN);
}

/*=============================================*/
/* handling V4L2 framework */
/*=============================================*/
static inline struct csi2rx_subdev *to_csi2rx_subdev(struct v4l2_subdev *sd)
{
	return container_of(sd, struct csi2rx_subdev, sd);
}

static int64_t get_pixelclock(struct v4l2_subdev *sd)
{
	struct v4l2_ctrl *ctrl;

	ctrl = v4l2_ctrl_find(sd->ctrl_handler, V4L2_CID_PIXEL_RATE);
	if (!ctrl)
		return -EINVAL;

	return v4l2_ctrl_g_ctrl_int64(ctrl);
}

static unsigned int viif_get_mbus_bpp(unsigned int mbus_code)
{
	const struct viif_mbus_format *fmt;

	fmt = viif_mbus_format_from_code(mbus_code);

	return fmt ? fmt->bpp : 24; /* default bpp */
}

/* ----- handling CSI2RX hardware ----- */
static const struct viif_csi2rx_line_err_target err_target_vc0_alldt = {
	/* select VC=0 */
	/* select all supported DataTypes */
	.dt = {
		MIPI_CSI2_DT_RGB565,
		MIPI_CSI2_DT_YUV422_8B,
		MIPI_CSI2_DT_YUV422_10B,
		MIPI_CSI2_DT_RGB888,
		MIPI_CSI2_DT_RAW8,
		MIPI_CSI2_DT_RAW10,
		MIPI_CSI2_DT_RAW12,
		MIPI_CSI2_DT_RAW14,
	}
};

static int viif_csi2rx_start(struct viif_device *viif_dev)
{
	struct v4l2_subdev *sensor_sd = viif_dev->sensor_sd;
	struct v4l2_mbus_config cfg = { 0 };
	struct v4l2_subdev_format fmt = {
		.pad = 0,
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};
	int num_lane, dphy_rate;
	s64 pixelclock;
	int ret;

	if (!sensor_sd)
		return -EINVAL;

	ret = v4l2_subdev_call(sensor_sd, pad, get_mbus_config, 0, &cfg);
	if (ret) {
		dev_dbg(viif_dev->dev, "subdev: g_mbus_config error. %d\n", ret);
		num_lane = viif_dev->sensor_num_lane;
	} else {
		if (cfg.type != V4L2_MBUS_CSI2_DPHY)
			return -EINVAL;
		num_lane = cfg.bus.mipi_csi2.num_data_lanes;
	}

	ret = v4l2_subdev_call(sensor_sd, pad, get_fmt, 0, &fmt);
	if (ret)
		return -EINVAL;

	pixelclock = get_pixelclock(sensor_sd);
	if (pixelclock < 0)
		return -EINVAL;

	dphy_rate = pixelclock * viif_get_mbus_bpp(fmt.format.code) / num_lane / 1000000;

	ret = viif_csi2rx_initialize(viif_dev, num_lane, VIIF_CSI2RX_DPHY_L0L1L2L3, dphy_rate, true,
				     &err_target_vc0_alldt);
	return ret;
}

static int viif_csi2rx_stop(struct viif_device *viif_dev)
{
	viif_csi2rx_uninitialize(viif_dev);

	return 0;
}

/* ----- V4L2 subdevice APIs (csi2rx subdevice) ----- */
static struct v4l2_mbus_framefmt *
visconti_viif_csi2rx_get_pad_fmt(struct csi2rx_subdev *csi2rx, struct v4l2_subdev_state *sd_state,
				 unsigned int pad, u32 which)
{
	struct v4l2_subdev_state state = {
		.pads = csi2rx->pad_cfg,
	};

	if (which == V4L2_SUBDEV_FORMAT_TRY)
		return v4l2_subdev_get_try_format(&csi2rx->sd, sd_state, pad);
	else
		return v4l2_subdev_get_try_format(&csi2rx->sd, &state, pad);
}

static int visconti_viif_csi2rx_enum_mbus_code(struct v4l2_subdev *sd,
					       struct v4l2_subdev_state *sd_state,
					       struct v4l2_subdev_mbus_code_enum *code)
{
	struct csi2rx_subdev *csi2rx = to_csi2rx_subdev(sd);
	const struct viif_mbus_format *fmt;

	if (code->pad == VIIF_CSI2RX_PAD_SRC) {
		const struct v4l2_mbus_framefmt *sink_fmt;

		/* should be equal to current settings of sink pad */
		if (code->index)
			return -EINVAL;

		mutex_lock(&csi2rx->ops_lock);

		sink_fmt = visconti_viif_csi2rx_get_pad_fmt(csi2rx, sd_state, VIIF_CSI2RX_PAD_SINK,
							    code->which);
		code->code = sink_fmt->code;

		mutex_unlock(&csi2rx->ops_lock);

		return 0;
	}

	/* find specified format */
	fmt = viif_mbus_format_nth(code->index);
	if (!fmt)
		return -EINVAL;

	code->code = fmt->code;
	return 0;
}

static int visconti_viif_csi2rx_init_config(struct v4l2_subdev *sd,
					    struct v4l2_subdev_state *sd_state)
{
	struct v4l2_mbus_framefmt *sink_fmt, *src_fmt;

	sink_fmt = v4l2_subdev_get_try_format(sd, sd_state, VIIF_CSI2RX_PAD_SINK);
	src_fmt = v4l2_subdev_get_try_format(sd, sd_state, VIIF_CSI2RX_PAD_SRC);

	sink_fmt->width = VIIF_CSI2RX_DEF_WIDTH;
	sink_fmt->height = VIIF_CSI2RX_DEF_HEIGHT;
	sink_fmt->field = V4L2_FIELD_NONE;
	sink_fmt->code = VIIF_CSI2RX_DEF_FMT;

	*src_fmt = *sink_fmt;

	return 0;
}

static int visconti_viif_csi2rx_get_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *sd_state,
					struct v4l2_subdev_format *fmt)
{
	struct csi2rx_subdev *csi2rx = to_csi2rx_subdev(sd);

	mutex_lock(&csi2rx->ops_lock);
	fmt->format = *visconti_viif_csi2rx_get_pad_fmt(csi2rx, sd_state, fmt->pad, fmt->which);
	mutex_unlock(&csi2rx->ops_lock);

	return 0;
}

static int visconti_viif_csi2rx_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *sd_state,
					struct v4l2_subdev_format *fmt)
{
	struct csi2rx_subdev *csi2rx = to_csi2rx_subdev(sd);
	struct v4l2_mbus_framefmt *sink_fmt, *src_fmt;

	if (fmt->pad == VIIF_CSI2RX_PAD_SRC)
		return visconti_viif_csi2rx_get_fmt(sd, sd_state, fmt);

	mutex_lock(&csi2rx->ops_lock);

	sink_fmt = visconti_viif_csi2rx_get_pad_fmt(csi2rx, sd_state, VIIF_CSI2RX_PAD_SINK,
						    fmt->which);

	sink_fmt->code = viif_mbus_format_from_code(fmt->format.code) ? fmt->format.code :
									      VIIF_CSI2RX_DEF_FMT;
	sink_fmt->width = clamp_t(u32, fmt->format.width, VIIF_ISP_MIN_WIDTH, VIIF_ISP_MAX_WIDTH);
	sink_fmt->height =
		clamp_t(u32, fmt->format.height, VIIF_ISP_MIN_HEIGHT, VIIF_ISP_MAX_HEIGHT);

	fmt->format = *sink_fmt;

	/* sourcep pad should have the same format */
	src_fmt =
		visconti_viif_csi2rx_get_pad_fmt(csi2rx, sd_state, VIIF_CSI2RX_PAD_SRC, fmt->which);
	*src_fmt = *sink_fmt;

	mutex_unlock(&csi2rx->ops_lock);

	return 0;
}

static int visconti_viif_csi2rx_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct csi2rx_subdev *csi2rx = to_csi2rx_subdev(sd);
	struct viif_device *viif_dev = csi2rx->viif_dev;
	struct v4l2_subdev *sensor_sd = viif_dev->sensor_sd;
	int ret;

	if (!sensor_sd)
		return -EINVAL;

	/* disabling: turn off sensor -> turn off CSI2RX */
	if (!enable) {
		v4l2_subdev_call(sensor_sd, video, s_stream, false);
		return viif_csi2rx_stop(viif_dev);
	}

	/* enabling: turn on CSI2RX -> turn on sensor -> (error handling) */
	ret = viif_csi2rx_start(viif_dev);
	if (ret)
		return ret;

	ret = v4l2_subdev_call(sensor_sd, video, s_stream, true);
	if (ret) {
		viif_csi2rx_stop(viif_dev);
		return ret;
	}
	return 0;
}

/* ----- register/remove csi2rx subdevice node ----- */
static const struct media_entity_operations visconti_viif_csi2rx_media_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static const struct v4l2_subdev_video_ops visconti_viif_csi2rx_video_ops = {
	.s_stream = visconti_viif_csi2rx_s_stream,
};

static const struct v4l2_subdev_pad_ops visconti_viif_csi2rx_pad_ops = {
	.enum_mbus_code = visconti_viif_csi2rx_enum_mbus_code,
	.init_cfg = visconti_viif_csi2rx_init_config,
	.get_fmt = visconti_viif_csi2rx_get_fmt,
	.set_fmt = visconti_viif_csi2rx_set_fmt,
};

static const struct v4l2_subdev_ops visconti_viif_csi2rx_ops = {
	.video = &visconti_viif_csi2rx_video_ops,
	.pad = &visconti_viif_csi2rx_pad_ops,
};

int visconti_viif_csi2rx_register(struct viif_device *viif_dev)
{
	struct v4l2_subdev_state state = {
		.pads = viif_dev->csi2rx_subdev.pad_cfg,
	};
	struct media_pad *pads = viif_dev->csi2rx_subdev.pads;
	struct v4l2_subdev *sd = &viif_dev->csi2rx_subdev.sd;
	int ret;

	viif_dev->csi2rx_subdev.viif_dev = viif_dev;

	v4l2_subdev_init(sd, &visconti_viif_csi2rx_ops);
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->entity.ops = &visconti_viif_csi2rx_media_ops;
	sd->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd->owner = THIS_MODULE;
	strscpy(sd->name, "visconti-viif:csi2rx", sizeof(sd->name));

	pads[VIIF_CSI2RX_PAD_SINK].flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	pads[VIIF_CSI2RX_PAD_SRC].flags = MEDIA_PAD_FL_SOURCE | MEDIA_PAD_FL_MUST_CONNECT;

	mutex_init(&viif_dev->csi2rx_subdev.ops_lock);

	ret = media_entity_pads_init(&sd->entity, VIIF_CSI2RX_PAD_NUM, pads);
	if (ret) {
		dev_err(viif_dev->dev, "Failed on media_entity_pads_init\n");
		return ret;
	}

	ret = v4l2_device_register_subdev(&viif_dev->v4l2_dev, sd);
	if (ret) {
		dev_err(viif_dev->dev, "Failed to register CSI2RX subdev\n");
		goto err_cleanup_media_entity;
	}

	visconti_viif_csi2rx_init_config(sd, &state);

	return 0;

err_cleanup_media_entity:
	media_entity_cleanup(&sd->entity);
	mutex_destroy(&viif_dev->csi2rx_subdev.ops_lock);
	viif_dev->csi2rx_subdev.viif_dev = NULL;
	return ret;
}

void visconti_viif_csi2rx_unregister(struct viif_device *viif_dev)
{
	v4l2_device_unregister_subdev(&viif_dev->csi2rx_subdev.sd);
	media_entity_cleanup(&viif_dev->csi2rx_subdev.sd.entity);
}
