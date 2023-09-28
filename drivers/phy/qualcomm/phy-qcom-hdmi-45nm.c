// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 * Copyright (c) 2023, Linaro Ltd.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iopoll.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

#include "phy-qcom-hdmi-preqmp.h"

#define REG_HDMI_8x60_PHY_REG0					0x00000000
#define HDMI_8x60_PHY_REG0_DESER_DEL_CTRL__MASK			0x0000001c

#define REG_HDMI_8x60_PHY_REG1					0x00000004
#define HDMI_8x60_PHY_REG1_DTEST_MUX_SEL__MASK			0x000000f0
#define HDMI_8x60_PHY_REG1_OUTVOL_SWING_CTRL__MASK		0x0000000f

#define REG_HDMI_8x60_PHY_REG2					0x00000008
#define HDMI_8x60_PHY_REG2_PD_DESER				0x00000001
#define HDMI_8x60_PHY_REG2_PD_DRIVE_1				0x00000002
#define HDMI_8x60_PHY_REG2_PD_DRIVE_2				0x00000004
#define HDMI_8x60_PHY_REG2_PD_DRIVE_3				0x00000008
#define HDMI_8x60_PHY_REG2_PD_DRIVE_4				0x00000010
#define HDMI_8x60_PHY_REG2_PD_PLL				0x00000020
#define HDMI_8x60_PHY_REG2_PD_PWRGEN				0x00000040
#define HDMI_8x60_PHY_REG2_RCV_SENSE_EN				0x00000080

#define REG_HDMI_8x60_PHY_REG3					0x0000000c
#define HDMI_8x60_PHY_REG3_PLL_ENABLE				0x00000001

#define REG_HDMI_8x60_PHY_REG4					0x00000010

#define REG_HDMI_8x60_PHY_REG5					0x00000014

#define REG_HDMI_8x60_PHY_REG6					0x00000018

#define REG_HDMI_8x60_PHY_REG7					0x0000001c

#define REG_HDMI_8x60_PHY_REG8					0x00000020

#define REG_HDMI_8x60_PHY_REG9					0x00000024

#define REG_HDMI_8x60_PHY_REG10					0x00000028

#define REG_HDMI_8x60_PHY_REG11					0x0000002c

#define REG_HDMI_8x60_PHY_REG12					0x00000030
#define HDMI_8x60_PHY_REG12_RETIMING_EN				0x00000001
#define HDMI_8x60_PHY_REG12_PLL_LOCK_DETECT_EN			0x00000002
#define HDMI_8x60_PHY_REG12_FORCE_LOCK				0x00000010

static int qcom_hdmi_msm8x60_phy_power_on(struct qcom_hdmi_preqmp_phy *hdmi_phy)
{
	unsigned long pixclock = hdmi_phy->hdmi_opts.pixel_clk_rate;

	/* De-serializer delay D/C for non-lbk mode: */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG0,
		       FIELD_PREP(HDMI_8x60_PHY_REG0_DESER_DEL_CTRL__MASK, 3));

	if (pixclock == 27000) {
		/* video_format == HDMI_VFRMT_720x480p60_16_9 */
		hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG1,
			       FIELD_PREP(HDMI_8x60_PHY_REG1_DTEST_MUX_SEL__MASK, 5) |
			       FIELD_PREP(HDMI_8x60_PHY_REG1_OUTVOL_SWING_CTRL__MASK, 3));
	} else {
		hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG1,
			       FIELD_PREP(HDMI_8x60_PHY_REG1_DTEST_MUX_SEL__MASK, 5) |
			       FIELD_PREP(HDMI_8x60_PHY_REG1_OUTVOL_SWING_CTRL__MASK, 4));
	}

	/* No matter what, start from the power down mode: */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG2,
		       HDMI_8x60_PHY_REG2_PD_PWRGEN |
		       HDMI_8x60_PHY_REG2_PD_PLL |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_4 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_3 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_2 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_1 |
		       HDMI_8x60_PHY_REG2_PD_DESER);

	/* Turn PowerGen on: */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG2,
		       HDMI_8x60_PHY_REG2_PD_PLL |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_4 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_3 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_2 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_1 |
		       HDMI_8x60_PHY_REG2_PD_DESER);

	/* Turn PLL power on: */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG2,
		       HDMI_8x60_PHY_REG2_PD_DRIVE_4 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_3 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_2 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_1 |
		       HDMI_8x60_PHY_REG2_PD_DESER);

	/* Write to HIGH after PLL power down de-assert: */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG3,
		       HDMI_8x60_PHY_REG3_PLL_ENABLE);

	/* ASIC power on; PHY REG9 = 0 */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG9, 0);

	/* Enable PLL lock detect, PLL lock det will go high after lock
	 * Enable the re-time logic
	 */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG12,
		       HDMI_8x60_PHY_REG12_RETIMING_EN |
		       HDMI_8x60_PHY_REG12_PLL_LOCK_DETECT_EN);

	/* Drivers are on: */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG2,
		       HDMI_8x60_PHY_REG2_PD_DESER);

	/* If the RX detector is needed: */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG2,
		       HDMI_8x60_PHY_REG2_RCV_SENSE_EN |
		       HDMI_8x60_PHY_REG2_PD_DESER);

	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG4, 0);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG5, 0);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG6, 0);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG7, 0);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG8, 0);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG9, 0);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG10, 0);
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG11, 0);

	/* If we want to use lock enable based on counting: */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG12,
		       HDMI_8x60_PHY_REG12_RETIMING_EN |
		       HDMI_8x60_PHY_REG12_PLL_LOCK_DETECT_EN |
		       HDMI_8x60_PHY_REG12_FORCE_LOCK);

	return 0;
}

static int qcom_hdmi_msm8x60_phy_power_off(struct qcom_hdmi_preqmp_phy *hdmi_phy)
{
	/* Turn off Driver */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG2,
		       HDMI_8x60_PHY_REG2_PD_DRIVE_4 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_3 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_2 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_1 |
		       HDMI_8x60_PHY_REG2_PD_DESER);
	udelay(10);
	/* Disable PLL */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG3, 0);
	/* Power down PHY, but keep RX-sense: */
	hdmi_phy_write(hdmi_phy, REG_HDMI_8x60_PHY_REG2,
		       HDMI_8x60_PHY_REG2_RCV_SENSE_EN |
		       HDMI_8x60_PHY_REG2_PD_PWRGEN |
		       HDMI_8x60_PHY_REG2_PD_PLL |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_4 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_3 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_2 |
		       HDMI_8x60_PHY_REG2_PD_DRIVE_1 |
		       HDMI_8x60_PHY_REG2_PD_DESER);

	return 0;
}

const struct qcom_hdmi_preqmp_cfg msm8x60_hdmi_phy_cfg = {
	.clk_names = { "slave_iface" },
	.num_clks = 1,

	.reg_names = { "core-vdda" },
	.num_regs = 1,

	.power_on = qcom_hdmi_msm8x60_phy_power_on,
	.power_off = qcom_hdmi_msm8x60_phy_power_off,

	/* FIXME: no PLL support */
};
