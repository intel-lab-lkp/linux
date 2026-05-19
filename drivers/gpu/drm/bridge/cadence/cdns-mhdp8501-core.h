/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Cadence MHDP 8501 Common head file
 *
 * Copyright (C) 2019-2024 NXP Semiconductor, Inc.
 *
 */

#ifndef _CDNS_MHDP8501_CORE_H_
#define _CDNS_MHDP8501_CORE_H_

#include <drm/drm_bridge.h>
#include <drm/drm_connector.h>
#include <drm/display/drm_dp_helper.h>
#include <linux/bitops.h>
#include <linux/i2c.h>
#include <soc/cadence/cdns-mhdp-helper.h>

#define ADDR_IMEM			0x10000
#define ADDR_DMEM			0x20000

/* APB CFG addr */
#define APB_CTRL			0
#define XT_INT_CTRL			0x04
#define MAILBOX_FULL_ADDR		0x08
#define MAILBOX_EMPTY_ADDR		0x0c
#define MAILBOX0_WR_DATA		0x10
#define MAILBOX0_RD_DATA		0x14
#define KEEP_ALIVE			0x18
#define VER_L				0x1c
#define VER_H				0x20
#define VER_LIB_L_ADDR			0x24
#define VER_LIB_H_ADDR			0x28
#define SW_DEBUG_L			0x2c
#define SW_DEBUG_H			0x30
#define MAILBOX_INT_MASK		0x34
#define MAILBOX_INT_STATUS		0x38
#define SW_CLK_L			0x3c
#define SW_CLK_H			0x40
#define SW_EVENTS0			0x44
#define SW_EVENTS1			0x48
#define SW_EVENTS2			0x4c
#define SW_EVENTS3			0x50
#define XT_OCD_CTRL			0x60
#define APB_INT_MASK			0x6c
#define APB_STATUS_MASK			0x70

/* Source phy comp */
#define PHY_DATA_SEL			0x0818
#define LANES_CONFIG			0x0814

/* Source CAR Addr */
#define SOURCE_HDTX_CAR			0x0900
#define SOURCE_DPTX_CAR			0x0904
#define SOURCE_PHY_CAR			0x0908
#define SOURCE_CEC_CAR			0x090c
#define SOURCE_CBUS_CAR			0x0910
#define SOURCE_PKT_CAR			0x0918
#define SOURCE_AIF_CAR			0x091c
#define SOURCE_CIPHER_CAR		0x0920
#define SOURCE_CRYPTO_CAR		0x0924

/* clock meters addr */
#define CM_CTRL				0x0a00
#define CM_I2S_CTRL			0x0a04
#define CM_SPDIF_CTRL			0x0a08
#define CM_VID_CTRL			0x0a0c
#define CM_LANE_CTRL			0x0a10
#define I2S_NM_STABLE			0x0a14
#define I2S_NCTS_STABLE			0x0a18
#define SPDIF_NM_STABLE			0x0a1c
#define SPDIF_NCTS_STABLE		0x0a20
#define NMVID_MEAS_STABLE		0x0a24
#define I2S_MEAS			0x0a40
#define SPDIF_MEAS			0x0a80
#define NMVID_MEAS			0x0ac0

/* source vif addr */
#define BND_HSYNC2VSYNC			0x0b00
#define HSYNC2VSYNC_F1_L1		0x0b04
#define HSYNC2VSYNC_STATUS		0x0b0c
#define HSYNC2VSYNC_POL_CTRL		0x0b10

/* MHDP TX_top_comp */
#define SCHEDULER_H_SIZE		0x1000
#define SCHEDULER_V_SIZE		0x1004
#define HDTX_SIGNAL_FRONT_WIDTH		0x100c
#define HDTX_SIGNAL_SYNC_WIDTH		0x1010
#define HDTX_SIGNAL_BACK_WIDTH		0x1014
#define HDTX_CONTROLLER			0x1018
#define HDTX_HPD			0x1020
#define HDTX_CLOCK_REG_0		0x1024
#define HDTX_CLOCK_REG_1		0x1028

/* DPTX hpd addr */
#define HPD_IRQ_DET_MIN_TIMER		0x2100
#define HPD_IRQ_DET_MAX_TIMER		0x2104
#define HPD_UNPLGED_DET_MIN_TIMER	0x2108
#define HPD_STABLE_TIMER		0x210c
#define HPD_FILTER_TIMER		0x2110
#define HPD_EVENT_MASK			0x211c
#define HPD_EVENT_DET			0x2120

/* DPTX framer addr */
#define DP_FRAMER_GLOBAL_CONFIG		0x2200
#define DP_SW_RESET			0x2204
#define DP_FRAMER_TU			0x2208
#define DP_FRAMER_PXL_REPR		0x220c
#define DP_FRAMER_SP			0x2210
#define AUDIO_PACK_CONTROL		0x2214
#define DP_VC_TABLE(x)			(0x2218 + ((x) << 2))
#define DP_VB_ID			0x2258
#define DP_MTPH_LVP_CONTROL		0x225c
#define DP_MTPH_SYMBOL_VALUES		0x2260
#define DP_MTPH_ECF_CONTROL		0x2264
#define DP_MTPH_ACT_CONTROL		0x2268
#define DP_MTPH_STATUS			0x226c
#define DP_INTERRUPT_SOURCE		0x2270
#define DP_INTERRUPT_MASK		0x2274
#define DP_FRONT_BACK_PORCH		0x2278
#define DP_BYTE_COUNT			0x227c

/* DPTX stream addr */
#define MSA_HORIZONTAL_0		0x2280
#define MSA_HORIZONTAL_1		0x2284
#define MSA_VERTICAL_0			0x2288
#define MSA_VERTICAL_1			0x228c
#define MSA_MISC			0x2290
#define STREAM_CONFIG			0x2294
#define AUDIO_PACK_STATUS		0x2298
#define VIF_STATUS			0x229c
#define PCK_STUFF_STATUS_0		0x22a0
#define PCK_STUFF_STATUS_1		0x22a4
#define INFO_PACK_STATUS		0x22a8
#define RATE_GOVERNOR_STATUS		0x22ac
#define DP_HORIZONTAL			0x22b0
#define DP_VERTICAL_0			0x22b4
#define DP_VERTICAL_1			0x22b8
#define DP_BLOCK_SDP			0x22bc

/* DPTX glbl addr */
#define DPTX_LANE_EN			0x2300
#define DPTX_ENHNCD			0x2304
#define DPTX_INT_MASK			0x2308
#define DPTX_INT_STATUS			0x230c

/* DP AUX Addr */
#define DP_AUX_HOST_CONTROL		0x2800
#define DP_AUX_INTERRUPT_SOURCE		0x2804
#define DP_AUX_INTERRUPT_MASK		0x2808
#define DP_AUX_SWAP_INVERSION_CONTROL	0x280c
#define DP_AUX_SEND_NACK_TRANSACTION	0x2810
#define DP_AUX_CLEAR_RX			0x2814
#define DP_AUX_CLEAR_TX			0x2818
#define DP_AUX_TIMER_STOP		0x281c
#define DP_AUX_TIMER_CLEAR		0x2820
#define DP_AUX_RESET_SW			0x2824
#define DP_AUX_DIVIDE_2M		0x2828
#define DP_AUX_TX_PREACHARGE_LENGTH	0x282c
#define DP_AUX_FREQUENCY_1M_MAX		0x2830
#define DP_AUX_FREQUENCY_1M_MIN		0x2834
#define DP_AUX_RX_PRE_MIN		0x2838
#define DP_AUX_RX_PRE_MAX		0x283c
#define DP_AUX_TIMER_PRESET		0x2840
#define DP_AUX_NACK_FORMAT		0x2844
#define DP_AUX_TX_DATA			0x2848
#define DP_AUX_RX_DATA			0x284c
#define DP_AUX_TX_STATUS		0x2850
#define DP_AUX_RX_STATUS		0x2854
#define DP_AUX_RX_CYCLE_COUNTER		0x2858
#define DP_AUX_MAIN_STATES		0x285c
#define DP_AUX_MAIN_TIMER		0x2860
#define DP_AUX_AFE_OUT			0x2864

/* source pif addr */
#define SOURCE_PIF_WR_ADDR		0x30800
#define SOURCE_PIF_WR_REQ		0x30804
#define SOURCE_PIF_RD_ADDR		0x30808
#define SOURCE_PIF_RD_REQ		0x3080c
#define SOURCE_PIF_DATA_WR		0x30810
#define SOURCE_PIF_DATA_RD		0x30814
#define SOURCE_PIF_FIFO1_FLUSH		0x30818
#define SOURCE_PIF_FIFO2_FLUSH		0x3081c
#define SOURCE_PIF_STATUS		0x30820
#define SOURCE_PIF_INTERRUPT_SOURCE	0x30824
#define SOURCE_PIF_INTERRUPT_MASK	0x30828
#define SOURCE_PIF_PKT_ALLOC_REG	0x3082c
#define SOURCE_PIF_PKT_ALLOC_WR_EN	0x30830
#define SOURCE_PIF_SW_RESET		0x30834

#define LINK_TRAINING_NOT_ACTIV		0
#define LINK_TRAINING_RUN		1
#define LINK_TRAINING_RESTART		2

#define CONTROL_VIDEO_IDLE		0
#define CONTROL_VIDEO_VALID		1

#define INTERLACE_FMT_DET		BIT(12)
#define VIF_BYPASS_INTERLACE		BIT(13)
#define TU_CNT_RST_EN			BIT(15)
#define INTERLACE_DTCT_WIN		0x20

#define DP_FRAMER_SP_INTERLACE_EN	BIT(2)
#define DP_FRAMER_SP_HSP		BIT(1)
#define DP_FRAMER_SP_VSP		BIT(0)

/* Capability */
#define AUX_HOST_INVERT			3
#define FAST_LT_SUPPORT			1
#define FAST_LT_NOT_SUPPORT		0
#define LANE_MAPPING_FLIPPED		0xe4
#define ENHANCED			1
#define SCRAMBLER_EN			BIT(4)

#define FULL_LT_STARTED			BIT(0)
#define FASE_LT_STARTED			BIT(1)
#define CLK_RECOVERY_FINISHED		BIT(2)
#define EQ_PHASE_FINISHED		BIT(3)
#define FASE_LT_START_FINISHED		BIT(4)
#define CLK_RECOVERY_FAILED		BIT(5)
#define EQ_PHASE_FAILED			BIT(6)
#define FASE_LT_FAILED			BIT(7)

#define TU_SIZE				30
#define CDNS_DP_MAX_LINK_RATE		540000

#define F_HDMI2_CTRL_IL_MODE(x)		(((x) & ((1 << 1) - 1)) << 19)
#define F_HDMI2_PREAMBLE_EN(x)		(((x) & ((1 << 1) - 1)) << 18)
#define F_HDMI_ENCODING(x)		(((x) & ((1 << 2) - 1)) << 16)
#define F_DATA_EN(x)			(((x) & ((1 << 1) - 1)) << 15)
#define F_CLEAR_AVMUTE(x)		(((x) & ((1 << 1) - 1)) << 14)
#define F_SET_AVMUTE(x)			(((x) & ((1 << 1) - 1)) << 13)
#define F_GCP_EN(x)			(((x) & ((1 << 1) - 1)) << 12)
#define F_BCH_EN(x)			(((x) & ((1 << 1) - 1)) << 11)
#define F_PIC_3D(x)			(((x) & ((1 << 4) - 1)) << 7)
#define F_VIF_DATA_WIDTH(x)		(((x) & ((1 << 2) - 1)) << 2)
#define F_HDMI_MODE(x)			(((x) & ((1 << 2) - 1)) << 0)

#define F_SOURCE_PHY_MHDP_SEL(x)	(((x) & ((1 << 2) - 1)) << 3)

#define F_HPD_GLITCH_WIDTH(x)		(((x) & ((1 << 8) - 1)) << 12)
#define F_PACKET_TYPE(x)		(((x) & ((1 << 8) - 1)) << 8)
#define F_HPD_VALID_WIDTH(x)		(((x) & ((1 << 12) - 1)) << 0)

#define F_SOURCE_PHY_LANE3_SWAP(x)	(((x) & ((1 << 2) - 1)) << 6)
#define F_SOURCE_PHY_LANE2_SWAP(x)	(((x) & ((1 << 2) - 1)) << 4)
#define F_SOURCE_PHY_LANE1_SWAP(x)	(((x) & ((1 << 2) - 1)) << 2)
#define F_SOURCE_PHY_LANE0_SWAP(x)	(((x) & ((1 << 2) - 1)) << 0)

#define F_ACTIVE_IDLE_TYPE(x)		(((x) & ((1 << 1) - 1)) << 17)
#define F_TYPE_VALID(x)			(((x) & ((1 << 1) - 1)) << 16)
#define F_PKT_ALLOC_ADDRESS(x)		(((x) & ((1 << 4) - 1)) << 0)

#define F_FIFO1_FLUSH(x)		(((x) & ((1 << 1) - 1)) << 0)
#define F_PKT_ALLOC_WR_EN(x)		(((x) & ((1 << 1) - 1)) << 0)
#define F_DATA_WR(x)			(x)
#define F_WR_ADDR(x)			(((x) & ((1 << 4) - 1)) << 0)
#define F_HOST_WR(x)			(((x) & ((1 << 1) - 1)) << 0)

/* Reference cycles when using lane clock as reference */
#define LANE_REF_CYC			0x8000

/* HPD Debounce */
#define HOTPLUG_DEBOUNCE_MS		200

/* HPD IRQ Index */
#define IRQ_IN    0
#define IRQ_OUT   1
#define IRQ_NUM   2

/* FW check alive timeout */
#define CDNS_KEEP_ALIVE_TIMEOUT		2000
#define CDNS_KEEP_ALIVE_MASK		GENMASK(7, 0)

enum voltage_swing_level {
	VOLTAGE_LEVEL_0,
	VOLTAGE_LEVEL_1,
	VOLTAGE_LEVEL_2,
	VOLTAGE_LEVEL_3,
};

enum pre_emphasis_level {
	PRE_EMPHASIS_LEVEL_0,
	PRE_EMPHASIS_LEVEL_1,
	PRE_EMPHASIS_LEVEL_2,
	PRE_EMPHASIS_LEVEL_3,
};

enum pattern_set {
	PTS1 = BIT(0),
	PTS2 = BIT(1),
	PTS3 = BIT(2),
	PTS4 = BIT(3),
	DP_NONE	= BIT(4)
};

enum vic_color_depth {
	BCS_6 = 0x1,
	BCS_8 = 0x2,
	BCS_10 = 0x4,
	BCS_12 = 0x8,
	BCS_16 = 0x10,
};

enum vic_bt_type {
	BT_601 = 0x0,
	BT_709 = 0x1,
};

enum {
	MODE_DVI,
	MODE_HDMI_1_4,
	MODE_HDMI_2_0,
};

struct video_info {
	int bpc;
	int color_fmt;
};

struct cdns_hdmi_i2c {
	struct i2c_adapter	adap;

	struct mutex		lock;	/* used to serialize data transfers */
	struct completion	cmp;
	u8			stat;

	u8			slave_reg;
	bool			is_regaddr;
	bool			is_segment;
};

struct cdns_mhdp8501_device {
	struct cdns_mhdp_base base;

	struct device *dev;
	void __iomem *regs;
	struct drm_crtc *curr_crtc;
	struct drm_bridge bridge;
	struct clk *apb_clk;
	struct phy *phy;
	bool phy_powered;

	struct video_info video_info;

	struct mutex link_mutex; /* serialises PHY/FW ops vs. hotplug retrain */

	int irq[IRQ_NUM];
	struct delayed_work hotplug_work;
	bool removing;
	int bridge_type;
	u32 lane_mapping;

	union {
		struct _dp_data {
			u32 rate;
			u8 num_lanes;
			struct drm_dp_aux aux;
			u8 dpcd[DP_RECEIVER_CAP_SIZE];
		} dp;
		struct _hdmi_data {
			u32 hdmi_type;
			struct cdns_hdmi_i2c *i2c;
		} hdmi;
	};

	struct dentry *debugfs;
};

static inline struct cdns_mhdp8501_device *
bridge_to_mhdp(const struct drm_bridge *bridge)
{
	return container_of(bridge, struct cdns_mhdp8501_device, bridge);
}

extern const struct drm_bridge_funcs cdns_dp_bridge_funcs;
extern const struct drm_bridge_funcs cdns_hdmi_bridge_funcs;

enum drm_connector_status
cdns_mhdp8501_detect(struct drm_bridge *bridge, struct drm_connector *connector);
enum drm_mode_status
cdns_mhdp8501_mode_valid(struct drm_bridge *bridge,
			 const struct drm_display_info *info,
			 const struct drm_display_mode *mode);

ssize_t cdns_dp_aux_transfer(struct drm_dp_aux *aux, struct drm_dp_aux_msg *msg);
void cdns_dp_check_link_state(struct cdns_mhdp8501_device *mhdp);

void cdns_hdmi_handle_hotplug(struct cdns_mhdp8501_device *mhdp);
struct i2c_adapter *cdns_hdmi_i2c_adapter(struct cdns_mhdp8501_device *mhdp);
#endif
