// SPDX-License-Identifier: GPL-2.0
/*
 * camss-csid-1080.c
 *
 * Qualcomm MSM Camera Subsystem - CSID (CSI Decoder) Module
 *
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/types.h>
#include <linux/v4l2-controls.h>
#include "camss.h"
#include "camss-csid.h"
#include "camss-csid-1080.h"

/* Reset and Command Registers */
#define CSID_RST_CFG				0x108
#define		RST_MODE				BIT(0)
#define		RST_LOCATION				BIT(4)

/* Reset and Command Registers */
#define CSID_RST_CMD				0x10C
#define		SELECT_HW_RST				BIT(0)
#define		SELECT_IRQ_RST				BIT(2)
#define CSID_IRQ_CMD				0x110
#define		IRQ_CMD_CLEAR				BIT(0)

/* Register Update Commands, RUP/AUP */
#define CSID_RUP_CMD				0x114
#define		RUP_RDIN				BIT(8)
#define CSID_AUP_CMD				0x118
#define		AUP_RDIN				BIT(8)
#define CSID_RUP_AUP_CMD			0x11C
#define		RUP_SET					BIT(0)
#define		MUP					BIT(4)

/* Top level interrupt registers */
#define CSID_TOP_IRQ_STATUS			0x180
#define CSID_TOP_IRQ_MASK			0x184
#define CSID_TOP_IRQ_CLEAR			0x188
#define CSID_TOP_IRQ_SET			0x18C
#define		INFO_RST_DONE				BIT(0)
#define		CSI2_RX_IRQ_STATUS			BIT(2)
#define		BUF_DONE_IRQ_STATUS			BIT(3)
#define		RDIn_IRQ_STATUS_OFFSET			16
#define		TOP_IRQ_STATUS_2			BIT(31)

/* Buffer done interrupt registers */
#define CSID_BUF_DONE_IRQ_STATUS		0x1A0
#define		BUF_DONE_IRQ_STATUS_RDI_OFFSET		16
#define CSID_BUF_DONE_IRQ_MASK			0x1A4
#define CSID_BUF_DONE_IRQ_CLEAR			0x1A8
#define CSID_BUF_DONE_IRQ_SET			0x1AC

/* CSI2 RX interrupt registers */
#define CSID_CSI2_RX_IRQ_STATUS			0x1B0
#define CSID_CSI2_RX_IRQ_MASK			0x1B4
#define CSID_CSI2_RX_IRQ_CLEAR			0x1B8
#define CSID_CSI2_RX_IRQ_SET			0x1BC

/* CSI2 RX Configuration */
#define CSID_CSI2_RX_CFG0			0x880
#define		CSI2_RX_CFG0_NUM_ACTIVE_LANES		0
#define		CSI2_RX_CFG0_DL0_INPUT_SEL		4
#define		CSI2_RX_CFG0_PHY_NUM_SEL		20
#define CSID_CSI2_RX_CFG1			0x884
#define		CSI2_RX_CFG1_ECC_CORRECTION_EN		BIT(0)
#define		CSI2_RX_CFG1_VC_MODE			BIT(2)

/* CSIPHY to hardware PHY selector mapping */
#define CSID_CSIPHY_ID_BASE_OFFSET		1

#define MSM_CSID_MAX_SRC_STREAMS_1080		(csid_is_lite(csid) ? 4 : 5)

/* RDI Configuration */
#define CSID_RDI_CFG0(rdi) \
	((csid_is_lite(csid) ? 0x3080 : 0x5480) + 0x200 * (rdi))
#define		RDI_CFG0_RETIME_BS			BIT(5)
#define		RDI_CFG0_TIMESTAMP_EN			BIT(6)
#define		RDI_CFG0_TIMESTAMP_STB_SEL		BIT(8)
#define		RDI_CFG0_DECODE_FORMAT			12
#define		RDI_CFG0_DT				16
#define		RDI_CFG0_VC				22
#define		RDI_CFG0_EN				BIT(31)

/* RDI Control and Configuration */
#define CSID_RDI_CTRL(rdi) \
	((csid_is_lite(csid) ? 0x3088 : 0x5488) + 0x200 * (rdi))
#define		RDI_CTRL_START_CMD			BIT(0)

#define CSID_RDI_CFG1(rdi) \
	((csid_is_lite(csid) ? 0x3094 : 0x5494) + 0x200 * (rdi))
#define		RDI_CFG1_DROP_H_EN			BIT(5)
#define		RDI_CFG1_DROP_V_EN			BIT(6)
#define		RDI_CFG1_CROP_H_EN			BIT(7)
#define		RDI_CFG1_CROP_V_EN			BIT(8)
#define		RDI_CFG1_PACKING_FORMAT_MIPI		BIT(15)

/* RDI Pixel Store Configuration */
#define CSID_RDI_PIX_STORE_CFG0(rdi)		(0x5498 + 0x200 * (rdi))
#define		RDI_PIX_STORE_CFG0_EN			BIT(0)
#define		RDI_PIX_STORE_CFG0_MIN_HBI		1

/* RDI IRQ Status in wrapper */
#define CSID_RDIN_IRQ_STATUS(rdi)		(0x224 + (0x10 * (rdi)))
#define CSID_RDIN_IRQ_MASK(rdi)			(0x228 + (0x10 * (rdi)))
#define CSID_RDIN_IRQ_CLEAR(rdi)		(0x22C + (0x10 * (rdi)))
#define		INFO_RUP_DONE				BIT(23)

static void __csid_aup_rup_trigger(struct csid_device *csid)
{
	/* trigger SET in combined register */
	writel(RUP_SET, csid->base + CSID_RUP_AUP_CMD);
}

static void __csid_aup_update(struct csid_device *csid, int port_id)
{
	csid->aup_update |= AUP_RDIN << port_id;
	writel(csid->aup_update, csid->base + CSID_AUP_CMD);

	__csid_aup_rup_trigger(csid);
}

static void __csid_reg_update(struct csid_device *csid, int port_id)
{
	csid->rup_update |= RUP_RDIN << port_id;
	writel(csid->rup_update, csid->base + CSID_RUP_CMD);

	__csid_aup_rup_trigger(csid);
}

static void __csid_configure_rx(struct csid_device *csid,
				struct csid_phy_config *phy)
{
	int val;

	val = (phy->lane_cnt - 1) << CSI2_RX_CFG0_NUM_ACTIVE_LANES;
	val |= phy->lane_assign << CSI2_RX_CFG0_DL0_INPUT_SEL;
	val |= (phy->csiphy_id + CSID_CSIPHY_ID_BASE_OFFSET)
	       << CSI2_RX_CFG0_PHY_NUM_SEL;
	writel(val, csid->base + CSID_CSI2_RX_CFG0);

	val = CSI2_RX_CFG1_ECC_CORRECTION_EN;
	writel(val, csid->base + CSID_CSI2_RX_CFG1);
}

static void __csid_configure_rx_vc(struct csid_device *csid,
				   struct csid_phy_config *phy, int vc)
{
	int val;

	if (vc > 3) {
		val = readl(csid->base + CSID_CSI2_RX_CFG1);
		val |= CSI2_RX_CFG1_VC_MODE;
		writel(val, csid->base + CSID_CSI2_RX_CFG1);
	}
}

static void __csid_ctrl_rdi(struct csid_device *csid, int enable, u8 rdi)
{
	int val = 0;

	if (enable)
		val = RDI_CTRL_START_CMD;

	writel(val, csid->base + CSID_RDI_CTRL(rdi));
}

static void __csid_configure_rdi_pix_store(struct csid_device *csid, u8 rdi)
{
	u32 val;

	/* Configure pixel store to allow absorption of hblanking or idle time.
	 * This helps with horizontal crop and prevents line buffer conflicts.
	 * Reset state is 0x8 which has MIN_HBI=4, we keep the default MIN_HBI
	 * and just enable the pixel store functionality.
	 */
	val = (4 << RDI_PIX_STORE_CFG0_MIN_HBI) | RDI_PIX_STORE_CFG0_EN;
	writel(val, csid->base + CSID_RDI_PIX_STORE_CFG0(rdi));
}

static void __csid_configure_rdi_stream(struct csid_device *csid, u8 enable, u8 vc)
{
	u32 val;
	u8 lane_cnt = csid->phy.lane_cnt;

	/* Source pads matching RDI channels on hardware.
	 * E.g. Pad 1 -> RDI0, Pad 2 -> RDI1, etc.
	 */
	struct v4l2_mbus_framefmt *input_format = &csid->fmt[MSM_CSID_PAD_FIRST_SRC + vc];
	const struct csid_format_info *format = csid_get_fmt_entry(csid->res->formats->formats,
								   csid->res->formats->nformats,
								   input_format->code);

	if (!lane_cnt)
		lane_cnt = 4;

	val = RDI_CFG0_TIMESTAMP_EN;
	val |= RDI_CFG0_TIMESTAMP_STB_SEL;
	val |= RDI_CFG0_RETIME_BS;

	/* note: for non-RDI path, this should be format->decode_format */
	val |= DECODE_FORMAT_PAYLOAD_ONLY << RDI_CFG0_DECODE_FORMAT;
	val |= vc << RDI_CFG0_VC;
	val |= format->data_type << RDI_CFG0_DT;
	writel(val, csid->base + CSID_RDI_CFG0(vc));

	val = RDI_CFG1_PACKING_FORMAT_MIPI;
	writel(val, csid->base + CSID_RDI_CFG1(vc));

	/* Configure pixel store using dedicated register in 1080 */
	if (!csid_is_lite(csid))
		__csid_configure_rdi_pix_store(csid, vc);

	val = 0;
	writel(val, csid->base + CSID_RDI_CTRL(vc));

	val = readl(csid->base + CSID_RDI_CFG0(vc));

	if (enable)
		val |= RDI_CFG0_EN;

	writel(val, csid->base + CSID_RDI_CFG0(vc));
}

static void csid_configure_stream_1080(struct csid_device *csid, u8 enable)
{
	u8 i;
	u8 vc;

	__csid_configure_rx(csid, &csid->phy);

	for (vc = 0; vc < MSM_CSID_MAX_SRC_STREAMS_1080; vc++) {
		if (csid->phy.en_vc & BIT(vc)) {
			__csid_configure_rdi_stream(csid, enable, vc);
			__csid_configure_rx_vc(csid, &csid->phy, vc);

			for (i = 0; i < CAMSS_INIT_BUF_COUNT; i++)
				__csid_aup_update(csid, vc);

			__csid_reg_update(csid, vc);

			__csid_ctrl_rdi(csid, enable, vc);
		}
	}
}

static int csid_configure_testgen_pattern_1080(struct csid_device *csid,
					       s32 val)
{
	return 0;
}

static void csid_subdev_reg_update_1080(struct csid_device *csid, int port_id,
					bool clear)
{
	/* No explicit clear required */
	if (!clear)
		__csid_aup_update(csid, port_id);
}

/**
 * csid_isr - CSID module interrupt service routine
 * @irq: Interrupt line
 * @dev: CSID device
 *
 * Return IRQ_HANDLED on success
 */
static irqreturn_t csid_isr_1080(int irq, void *dev)
{
	struct csid_device *csid = dev;
	u32 val, buf_done_val;
	u8 reset_done;
	int i;

	val = readl(csid->base + CSID_TOP_IRQ_STATUS);
	writel(val, csid->base + CSID_TOP_IRQ_CLEAR);

	reset_done = val & INFO_RST_DONE;

	buf_done_val = readl(csid->base + CSID_BUF_DONE_IRQ_STATUS);
	writel(buf_done_val, csid->base + CSID_BUF_DONE_IRQ_CLEAR);

	for (i = 0; i < MSM_CSID_MAX_SRC_STREAMS_1080; i++)
		if (csid->phy.en_vc & BIT(i)) {
			val = readl(csid->base + CSID_RDIN_IRQ_STATUS(i));
			writel(val, csid->base + CSID_RDIN_IRQ_CLEAR(i));

			if (buf_done_val & BIT(BUF_DONE_IRQ_STATUS_RDI_OFFSET + i)) {
				/*
				 * buf done and RUP IRQ have been moved to CSID from VFE.
				 * Once CSID received buf done, need notify VFE of this
				 * event and trigger VFE to handle buf done process.
				 */
				camss_buf_done(csid->camss, csid->id, i);
			}
		}

	val = IRQ_CMD_CLEAR;
	writel(val, csid->base + CSID_IRQ_CMD);

	if (reset_done)
		complete(&csid->reset_complete);

	return IRQ_HANDLED;
}

/**
 * csid_reset - Trigger reset on CSID module and wait to complete
 * @csid: CSID device
 *
 * Return 0 on success or a negative error code otherwise
 */
static int csid_reset_1080(struct csid_device *csid)
{
	unsigned long time;
	u32 val;
	int i;

	reinit_completion(&csid->reset_complete);

	val = INFO_RST_DONE | BUF_DONE_IRQ_STATUS;
	writel(val, csid->base + CSID_TOP_IRQ_CLEAR);
	writel(val, csid->base + CSID_TOP_IRQ_MASK);

	val = 0;
	for (i = 0; i < MSM_CSID_MAX_SRC_STREAMS_1080; i++) {
		if (csid->phy.en_vc & BIT(i)) {
			/* only need to clear Buffer Done IRQ Status here,
			 * RUP Done IRQ Status will be cleared once isr
			 * strobe generated by CSID_RST_CMD
			 */
			val |= BIT(BUF_DONE_IRQ_STATUS_RDI_OFFSET + i);
		}
	}
	writel(val, csid->base + CSID_BUF_DONE_IRQ_CLEAR);
	writel(val, csid->base + CSID_BUF_DONE_IRQ_MASK);

	/* Clear all IRQ status with CLEAR bits set */
	val = IRQ_CMD_CLEAR;
	writel(val, csid->base + CSID_IRQ_CMD);

	val = RST_LOCATION | RST_MODE;
	writel(val, csid->base + CSID_RST_CFG);

	val = SELECT_HW_RST | SELECT_IRQ_RST;
	writel(val, csid->base + CSID_RST_CMD);

	time = wait_for_completion_timeout(&csid->reset_complete,
					   msecs_to_jiffies(CSID_RESET_TIMEOUT_MS));

	if (!time) {
		dev_err(csid->camss->dev, "CSID reset timeout\n");
		return -EIO;
	}

	return 0;
}

static void csid_subdev_init_1080(struct csid_device *csid)
{
	csid->testgen.nmodes = CSID_PAYLOAD_MODE_DISABLED;
}

const struct csid_hw_ops csid_ops_1080 = {
	.configure_stream = csid_configure_stream_1080,
	.configure_testgen_pattern = csid_configure_testgen_pattern_1080,
	.hw_version = csid_hw_version,
	.isr = csid_isr_1080,
	.reset = csid_reset_1080,
	.src_pad_code = csid_src_pad_code,
	.subdev_init = csid_subdev_init_1080,
	.reg_update = csid_subdev_reg_update_1080,
};
