// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2020-2023 Unisoc Inc.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/iio/consumer.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/usb/otg.h>
#include <uapi/linux/usb/charger.h>

#include "phy-sprd-usb20-hs.h"

static const struct sprd_hsphy_cfg *phy_cfg;

/* phy_v1 cfg ops */
static void phy_v1_usb_enable_ctrl(struct sprd_hsphy *phy, int on) {}

static void phy_v1_usb_phy_power_ctrl(struct sprd_hsphy *phy, int on)
{
	u32 reg, msk;

	if (on == CTRL0) {
		reg = msk = phy_cfg->masks[MASK_AON_APB_USB_PHY_PD_L] |
			phy_cfg->masks[MASK_AON_APB_USB_PHY_PD_S];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_PWR_CTRL], msk, reg);

		if (phy_cfg->owner == PIKE2) {
			reg = readl_relaxed(phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL0]);
			reg &= ~0x1;
			writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL0]);
		}
	} else if (on == CTRL1) {
		if (phy_cfg->owner == PIKE2) {
			reg = readl_relaxed(phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL0]);
			reg |= 0x1;
			writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL0]);
		}

		msk = phy_cfg->masks[MASK_AON_APB_USB_PHY_PD_L] |
			phy_cfg->masks[MASK_AON_APB_USB_PHY_PD_S];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_PWR_CTRL], msk, 0);
	} else if (on == CTRL2) {
		reg = msk = phy_cfg->masks[MASK_AON_APB_USB_PHY_PD_L] |
			phy_cfg->masks[MASK_AON_APB_USB_PHY_PD_S];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_PWR_CTRL], msk, reg);
	}
}

static void phy_v1_usb_vbus_ctrl(struct sprd_hsphy *phy, int on)
{
	u32 reg;

	reg = readl_relaxed(phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_TEST]);
	if (on) {
		reg |= (phy_cfg->masks[MASK_AP_AHB_OTG_VBUS_VALID_EXT] |
			phy_cfg->masks[MASK_AP_AHB_OTG_VBUS_VALID_PHYREG]);
	} else {
		reg &= ~(phy_cfg->masks[MASK_AP_AHB_OTG_VBUS_VALID_EXT] |
			phy_cfg->masks[MASK_AP_AHB_OTG_VBUS_VALID_PHYREG]);
	}
	writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_TEST]);
}

static void phy_v1_utmi_width_sel(struct sprd_hsphy *phy)
{
	u32 reg;

	reg = readl_relaxed(phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_CTRL]);
	reg &= ~(phy_cfg->masks[MASK_AP_AHB_UTMI_WIDTH_SEL] |
		phy_cfg->masks[MASK_AP_AHB_USB2_DATABUS16_8]);
	writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_CTRL]);
}

static void phy_v1_reset_core(struct sprd_hsphy *phy)
{
	u32 reg;

	reg = readl_relaxed(phy->base + phy_cfg->regs[REG_AP_AHB_AHB_RST]);
	reg = phy_cfg->masks[MASK_AP_AHB_OTG_PHY_SOFT_RST] |
		phy_cfg->masks[MASK_AP_AHB_OTG_UTMI_SOFT_RST] |
		phy_cfg->masks[MASK_AP_AHB_OTG_SOFT_RST];
	writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_AHB_RST] + 0x1000);

	usleep_range(20000, 30000);
	writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_AHB_RST] + 0x2000);
}

static int phy_v1_set_mode(struct sprd_hsphy *phy, int on)
{
	u32 reg;

	if (on) {
		reg = phy->host_otg_ctrl0;
		writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL0]);
		reg = phy->host_otg_ctrl1;
		writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL1]);

		reg = readl_relaxed(phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_CTRL]);
		reg &= ~phy_cfg->masks[MASK_AP_AHB_USB2_PHY_IDDIG];
		reg |= phy_cfg->masks[MASK_AP_AHB_OTG_DPPULLDOWN] |
			phy_cfg->masks[MASK_AP_AHB_OTG_DMPULLDOWN];
		writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_CTRL]);

		reg = readl_relaxed(phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL0]);
		reg |= 0x200;
		writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL0]);
		phy->is_host = true;
	} else {
		reg = phy->device_otg_ctrl0;
		writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL0]);
		reg = phy->device_otg_ctrl1;
		writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL1]);

		reg = readl_relaxed(phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_CTRL]);
		reg |= phy_cfg->masks[MASK_AP_AHB_USB2_PHY_IDDIG];
		reg &= ~(phy_cfg->masks[MASK_AP_AHB_OTG_DPPULLDOWN] |
			phy_cfg->masks[MASK_AP_AHB_OTG_DMPULLDOWN]);
		writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_CTRL]);

		reg = readl_relaxed(phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL0]);
		reg &= ~0x200;
		writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL0]);
		phy->is_host = false;
	}
	return 0;
}

static const struct sprd_hsphy_cfg_ops phy_v1_cfg_ops = {
	.usb_enable_ctrl = phy_v1_usb_enable_ctrl,
	.usb_phy_power_ctrl = phy_v1_usb_phy_power_ctrl,
	.usb_vbus_ctrl = phy_v1_usb_vbus_ctrl,
	.utmi_width_sel = phy_v1_utmi_width_sel,
	.reset_core = phy_v1_reset_core,
	.set_mode = phy_v1_set_mode,
};

/* phy_v2 cfg ops */
static void phy_v2_usb_enable_ctrl(struct sprd_hsphy *phy, int on)
{
	u32 reg, msk;

	if (on == CTRL0) {
		msk = phy_cfg->masks[MASK_AON_APB_OTG_REF_EB];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_APB_EB2], msk, 0);
	} else if (on == CTRL1) {
		reg = msk = phy_cfg->masks[MASK_AON_APB_OTG_REF_EB];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_APB_EB2], msk, reg);
	} else if (on == CTRL2) {
		reg = msk = phy_cfg->masks[MASK_AP_AHB_OTG_EB];
		regmap_update_bits(phy->ap_ahb, phy_cfg->regs[REG_AP_AHB_AHB_EB], msk, reg);
		reg = msk = phy_cfg->masks[MASK_AON_APB_ANLG_APB_EB] |
			phy_cfg->masks[MASK_AON_APB_ANLG_EB] |
			phy_cfg->masks[MASK_AON_APB_OTG_REF_EB];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_APB_EB2], msk, reg);
	}
}

static void phy_v2_usb_phy_power_ctrl(struct sprd_hsphy *phy, int on)
{
	u32 reg, msk;

	if (on == CTRL0) {
		reg = msk = phy_cfg->masks[MASK_AON_APB_USB_ISO_SW_EN];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_PWR_CTRL], msk, reg);
		usleep_range(10000, 15000);
		reg = msk = phy_cfg->masks[MASK_AON_APB_USB_PHY_PD_L] |
			phy_cfg->masks[MASK_AON_APB_USB_PHY_PD_S];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_PWR_CTRL], msk, reg);
	} else if (on == CTRL1) {
		msk = phy_cfg->masks[MASK_AON_APB_USB_PHY_PD_L] |
			phy_cfg->masks[MASK_AON_APB_USB_PHY_PD_S] |
			phy_cfg->masks[MASK_AON_APB_USB_ISO_SW_EN];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_PWR_CTRL], msk, 0);
	} else if (on == CTRL2) {
		reg = msk = phy_cfg->masks[MASK_AON_APB_USB_PHY_PD_L] |
			phy_cfg->masks[MASK_AON_APB_USB_PHY_PD_S];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_PWR_CTRL], msk, reg);
	}
}

static void phy_v2_usb_vbus_ctrl(struct sprd_hsphy *phy, int on)
{
	u32 reg, msk;

	if (on) {
		reg = readl_relaxed(phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_TEST]);
		reg |= phy_cfg->masks[MASK_AP_AHB_OTG_VBUS_VALID_PHYREG];
		writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_TEST]);

		reg = msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_VBUSVLDEXT];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL1], msk, reg);
	} else {
		reg = readl_relaxed(phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_TEST]);
		reg &= ~phy_cfg->masks[MASK_AP_AHB_OTG_VBUS_VALID_PHYREG];
		writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_TEST]);

		msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_VBUSVLDEXT];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL1], msk, 0);
	}
}

static void phy_v2_utmi_width_sel(struct sprd_hsphy *phy)
{
	u32 reg, msk;

	reg = msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_DATABUS16_8];
	regmap_update_bits(phy->analog,
		phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL1], msk, reg);

	reg = readl_relaxed(phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_CTRL]);
	reg |= phy_cfg->masks[MASK_AP_AHB_UTMI_WIDTH_SEL];
	writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_CTRL]);
}

static void phy_v2_reset_core(struct sprd_hsphy *phy)
{
	u32 msk1, msk2;

	msk1 = phy_cfg->masks[MASK_AP_AHB_OTG_UTMI_SOFT_RST] |
		phy_cfg->masks[MASK_AP_AHB_OTG_SOFT_RST];
	regmap_update_bits(phy->ap_ahb, phy_cfg->regs[REG_AP_AHB_AHB_RST], msk1, msk1);
	msk2 = phy_cfg->masks[MASK_AON_APB_OTG_PHY_SOFT_RST];
	regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_APB_RST2], msk2, msk2);

	usleep_range(20000, 30000);
	regmap_update_bits(phy->ap_ahb, phy_cfg->regs[REG_AP_AHB_AHB_RST], msk1, 0);
	regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_APB_RST2], msk2, 0);
}

static int phy_v2_set_mode(struct sprd_hsphy *phy, int on)
{
	u32 reg, msk;

	if (on) {
		reg = phy->host_eye_pattern;
		regmap_write(phy->analog, phy_cfg->regs[REG_ANALOG_USB20_USB20_TRIMMING], reg);

		msk = phy_cfg->masks[MASK_ANALOG_USB20_UTMIOTG_IDDG];
		regmap_update_bits(phy->analog, phy_cfg->regs[REG_ANALOG_USB20_IDDG], msk, 0);

		reg = msk = phy_cfg->masks[MASK_DBG_SEL_ANALOG_USB20_USB20_DMPULLDOWN] |
			phy_cfg->masks[MASK_DBG_SEL_ANALOG_USB20_USB20_DPPULLDOWN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_REG_SEL_CFG_0], msk, reg);
		reg = msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_DMPULLDOWN] |
			phy_cfg->masks[MASK_ANALOG_USB20_USB20_DPPULLDOWN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL2], msk, reg);

		msk = 0x200;
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL1], msk, msk);
		phy->is_host = true;
	} else {
		reg = phy->device_eye_pattern;
		regmap_write(phy->analog, phy_cfg->regs[REG_ANALOG_USB20_USB20_TRIMMING], reg);

		reg = msk = phy_cfg->masks[MASK_ANALOG_USB20_UTMIOTG_IDDG];
		regmap_update_bits(phy->analog, phy_cfg->regs[REG_ANALOG_USB20_IDDG], msk, reg);

		reg = msk = phy_cfg->masks[MASK_DBG_SEL_ANALOG_USB20_USB20_DMPULLDOWN] |
			phy_cfg->masks[MASK_DBG_SEL_ANALOG_USB20_USB20_DPPULLDOWN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_REG_SEL_CFG_0], msk, reg);
		msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_DMPULLDOWN] |
			phy_cfg->masks[MASK_ANALOG_USB20_USB20_DPPULLDOWN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL2], msk, 0);

		msk = 0x200;
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL1], msk, 0);
		phy->is_host = false;
	}
	return 0;
}

static const struct sprd_hsphy_cfg_ops phy_v2_cfg_ops = {
	.usb_enable_ctrl = phy_v2_usb_enable_ctrl,
	.usb_phy_power_ctrl = phy_v2_usb_phy_power_ctrl,
	.usb_vbus_ctrl = phy_v2_usb_vbus_ctrl,
	.utmi_width_sel = phy_v2_utmi_width_sel,
	.reset_core = phy_v2_reset_core,
	.set_mode = phy_v2_set_mode,
};

/* phy_v3 cfg ops */
static void phy_v3_usb_enable_ctrl(struct sprd_hsphy *phy, int on)
{
	u32 reg, msk;

	if (on == CTRL0) {
		msk = phy_cfg->masks[MASK_AON_APB_CGM_OTG_REF_EN] |
			phy_cfg->masks[MASK_AON_APB_CGM_DPHY_REF_EN];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_CGM_REG1], msk, 0);
	} else if (on == CTRL1) {
		reg = msk = phy_cfg->masks[MASK_AON_APB_OTG_UTMI_EB];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_APB_EB1], msk, reg);
		reg = msk = phy_cfg->masks[MASK_AON_APB_CGM_OTG_REF_EN] |
			phy_cfg->masks[MASK_AON_APB_CGM_DPHY_REF_EN];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_CGM_REG1], msk, reg);
	} else if (on == CTRL2) {
		reg = msk = phy_cfg->masks[MASK_AON_APB_OTG_UTMI_EB] |
			phy_cfg->masks[MASK_AON_APB_ANA_EB];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_APB_EB1], msk, reg);
		reg = msk = phy_cfg->masks[MASK_AON_APB_CGM_OTG_REF_EN] |
			phy_cfg->masks[MASK_AON_APB_CGM_DPHY_REF_EN];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_CGM_REG1], msk, reg);
	}
}

static void phy_v3_usb_phy_power_ctrl(struct sprd_hsphy *phy, int on)
{
	u32 reg, msk;

	if (on == CTRL0) {
		reg = msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_PS_PD_L] |
			phy_cfg->masks[MASK_ANALOG_USB20_USB20_PS_PD_S];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_BATTER_PLL], msk, reg);
		reg = msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_ISO_SW_EN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_ISO_SW], msk, reg);
	} else if (on == CTRL1) {
		msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_ISO_SW_EN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_ISO_SW], msk, 0);
		msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_PS_PD_L] |
			phy_cfg->masks[MASK_ANALOG_USB20_USB20_PS_PD_S];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_BATTER_PLL], msk, 0);
	} else if (on == CTRL2) {
		reg = msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_PS_PD_L] |
			phy_cfg->masks[MASK_ANALOG_USB20_USB20_PS_PD_S];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_BATTER_PLL], msk, reg);
	}
}

static void phy_v3_usb_vbus_ctrl(struct sprd_hsphy *phy, int on)
{
	u32 reg, msk;

	msk = phy_cfg->masks[MASK_AON_APB_OTG_VBUS_VALID_PHYREG];
	reg = on ? msk : 0;
	regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_OTG_PHY_TEST], msk, reg);

	msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_VBUSVLDEXT];
	reg = on ? msk : 0;
	regmap_update_bits(phy->analog,
		phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL1], msk, reg);
}

static void phy_v3_utmi_width_sel(struct sprd_hsphy *phy)
{
	u32 reg, msk;

	reg = msk = phy_cfg->masks[MASK_AON_APB_UTMI_WIDTH_SEL];
	regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_OTG_PHY_CTRL], msk, reg);

	reg = msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_DATABUS16_8];
	regmap_update_bits(phy->analog,
		phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL1], msk, reg);
}

static void phy_v3_reset_core(struct sprd_hsphy *phy)
{
	u32 reg, msk;

	reg = msk = phy_cfg->masks[MASK_AON_APB_OTG_PHY_SOFT_RST] |
		phy_cfg->masks[MASK_AON_APB_OTG_UTMI_SOFT_RST];
	regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_APB_RST1], msk, reg);

	usleep_range(20000, 30000);
	regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_APB_RST1], msk, 0);
}

static int phy_v3_set_mode(struct sprd_hsphy *phy, int on)
{
	u32 reg, msk;

	if (on) {
		reg = phy->host_eye_pattern;
		regmap_write(phy->analog, phy_cfg->regs[REG_ANALOG_USB20_USB20_TRIMMING], reg);

		msk = phy_cfg->masks[MASK_AON_APB_USB2_PHY_IDDIG];
		regmap_update_bits(phy->aon_apb,
			phy_cfg->regs[REG_AON_APB_OTG_PHY_CTRL], msk, 0);

		reg = msk = phy_cfg->masks[MASK_DBG_SEL_ANALOG_USB20_USB20_DMPULLDOWN] |
			phy_cfg->masks[MASK_DBG_SEL_ANALOG_USB20_USB20_DPPULLDOWN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_REG_SEL_CFG_0], msk, reg);
		reg = msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_DMPULLDOWN] |
			phy_cfg->masks[MASK_ANALOG_USB20_USB20_DPPULLDOWN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL2], msk, reg);

		reg = 0x200;
		msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_RESERVED];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL1], msk, reg);
		phy->is_host = true;
	} else {
		reg = phy->device_eye_pattern;
		regmap_write(phy->analog, phy_cfg->regs[REG_ANALOG_USB20_USB20_TRIMMING], reg);

		reg = msk = phy_cfg->masks[MASK_AON_APB_USB2_PHY_IDDIG];
		regmap_update_bits(phy->aon_apb,
			phy_cfg->regs[REG_AON_APB_OTG_PHY_CTRL], msk, reg);

		reg = msk = phy_cfg->masks[MASK_DBG_SEL_ANALOG_USB20_USB20_DMPULLDOWN] |
			phy_cfg->masks[MASK_DBG_SEL_ANALOG_USB20_USB20_DPPULLDOWN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_REG_SEL_CFG_0], msk, reg);
		msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_DMPULLDOWN] |
			phy_cfg->masks[MASK_ANALOG_USB20_USB20_DPPULLDOWN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL2], msk, 0);

		msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_RESERVED];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL1], msk, 0);
		phy->is_host = false;
	}
	return 0;
}

static const struct sprd_hsphy_cfg_ops phy_v3_cfg_ops = {
	.usb_enable_ctrl = phy_v3_usb_enable_ctrl,
	.usb_phy_power_ctrl = phy_v3_usb_phy_power_ctrl,
	.usb_vbus_ctrl = phy_v3_usb_vbus_ctrl,
	.utmi_width_sel = phy_v3_utmi_width_sel,
	.reset_core = phy_v3_reset_core,
	.set_mode = phy_v3_set_mode,
};

/* phy_v4 cfg ops */
static void phy_v4_usb_enable_ctrl(struct sprd_hsphy *phy, int on)
{
	u32 reg, msk;

	if (on == CTRL0) {
		if (phy_cfg->owner == UIS8520)
			msk = phy_cfg->masks[MASK_AON_APB_CGM_OTG_REF_EN];
		else
			msk = phy_cfg->masks[MASK_AON_APB_CGM_OTG_REF_EN] |
				phy_cfg->masks[MASK_AON_APB_CGM_DPHY_REF_EN];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_CGM_REG1], msk, 0);
		msk = phy_cfg->masks[MASK_AON_APB_AON_USB2_TOP_EB] |
			phy_cfg->masks[MASK_AON_APB_OTG_PHY_EB];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_APB_EB1], msk, 0);
	} else if (on == CTRL1) {
		reg = msk = phy_cfg->masks[MASK_AON_APB_AON_USB2_TOP_EB] |
			phy_cfg->masks[MASK_AON_APB_OTG_PHY_EB];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_APB_EB1], msk, reg);
		if (phy_cfg->owner == UIS8520)
			reg = msk = phy_cfg->masks[MASK_AON_APB_CGM_OTG_REF_EN];
		else
			reg = msk = phy_cfg->masks[MASK_AON_APB_CGM_OTG_REF_EN] |
				phy_cfg->masks[MASK_AON_APB_CGM_DPHY_REF_EN];
		regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_CGM_REG1], msk, reg);
	}
}

static void phy_v4_usb_phy_power_ctrl(struct sprd_hsphy *phy, int on)
{
	u32 reg, msk;

	if (on == CTRL0) {
		reg = msk = phy_cfg->masks[MASK_AON_APB_USB20_ISO_SW_EN];
		regmap_update_bits(phy->aon_apb,
			phy_cfg->regs[REG_AON_APB_AON_SOC_USB_CTRL], msk, reg);
		reg = msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_ISO_SW_EN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_PHY], msk, reg);

		reg = msk = phy_cfg->masks[MASK_AON_APB_C2G_ANALOG_USB20_USB20_PS_PD_L] |
			phy_cfg->masks[MASK_AON_APB_C2G_ANALOG_USB20_USB20_PS_PD_S];
		regmap_update_bits(phy->aon_apb,
			phy_cfg->regs[REG_AON_APB_MIPI_CSI_POWER_CTRL], msk, reg);
	} else if (on == CTRL1) {
		msk = phy_cfg->masks[MASK_AON_APB_C2G_ANALOG_USB20_USB20_PS_PD_L] |
			phy_cfg->masks[MASK_AON_APB_C2G_ANALOG_USB20_USB20_PS_PD_S];
		regmap_update_bits(phy->aon_apb,
			phy_cfg->regs[REG_AON_APB_MIPI_CSI_POWER_CTRL], msk, 0);

		msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_ISO_SW_EN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_PHY], msk, 0);
		msk = phy_cfg->masks[MASK_AON_APB_USB20_ISO_SW_EN];
		regmap_update_bits(phy->aon_apb,
			phy_cfg->regs[REG_AON_APB_AON_SOC_USB_CTRL], msk, 0);
	}
}

static void phy_v4_usb_vbus_ctrl(struct sprd_hsphy *phy, int on)
{
	u32 reg, msk;

	msk = phy_cfg->masks[MASK_AON_APB_OTG_VBUS_VALID_PHYREG];
	reg = on ? msk : 0;
	regmap_update_bits(phy->aon_apb,
		phy_cfg->regs[REG_AON_APB_OTG_PHY_TEST], msk, reg);

	msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_VBUSVLDEXT];
	reg = on ? msk : 0;
	regmap_update_bits(phy->analog,
		phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL1], msk, reg);
}

static void phy_v4_utmi_width_sel(struct sprd_hsphy *phy)
{
	u32 reg, msk;

	if (phy_cfg->owner == UIS8520)
		return;

	reg = msk = phy_cfg->masks[MASK_AON_APB_UTMI_WIDTH_SEL];
	regmap_update_bits(phy->aon_apb,
		phy_cfg->regs[REG_AON_APB_OTG_PHY_CTRL], msk, reg);

	reg = msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_DATABUS16_8];
	regmap_update_bits(phy->analog,
		phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL1], msk, reg);
}

static void phy_v4_reset_core(struct sprd_hsphy *phy)
{
	u32 reg, msk;

	reg = msk = phy_cfg->masks[MASK_AON_APB_OTG_PHY_SOFT_RST] |
		phy_cfg->masks[MASK_AON_APB_OTG_UTMI_SOFT_RST];

	regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_APB_RST1], msk, reg);

	usleep_range(20000, 30000);
	regmap_update_bits(phy->aon_apb, phy_cfg->regs[REG_AON_APB_APB_RST1], msk, 0);
}

static int phy_v4_set_mode(struct sprd_hsphy *phy, int on)
{
	u32 reg, msk;

	if (on) {
		reg = phy->host_eye_pattern;
		regmap_write(phy->analog, phy_cfg->regs[REG_ANALOG_USB20_USB20_TRIMMING], reg);

		msk = phy_cfg->masks[MASK_AON_APB_USB2_PHY_IDDIG];
		regmap_update_bits(phy->aon_apb,
			phy_cfg->regs[REG_AON_APB_OTG_PHY_CTRL], msk, 0);

		reg = msk = phy_cfg->masks[MASK_DBG_SEL_ANALOG_USB20_USB20_DMPULLDOWN] |
			phy_cfg->masks[MASK_DBG_SEL_ANALOG_USB20_USB20_DPPULLDOWN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_REG_SEL_CFG_0], msk, reg);
		reg = msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_DMPULLDOWN] |
			phy_cfg->masks[MASK_ANALOG_USB20_USB20_DPPULLDOWN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL2], msk, reg);

		reg = 0x200;
		msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_RESERVED];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL1], msk, reg);
		phy->is_host = true;
	} else {
		reg = phy->device_eye_pattern;
		regmap_write(phy->analog, phy_cfg->regs[REG_ANALOG_USB20_USB20_TRIMMING], reg);

		reg = msk = phy_cfg->masks[MASK_AON_APB_USB2_PHY_IDDIG];
		regmap_update_bits(phy->aon_apb,
			phy_cfg->regs[REG_AON_APB_OTG_PHY_CTRL], msk, reg);

		reg = msk = phy_cfg->masks[MASK_DBG_SEL_ANALOG_USB20_USB20_DMPULLDOWN] |
			phy_cfg->masks[MASK_DBG_SEL_ANALOG_USB20_USB20_DPPULLDOWN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_REG_SEL_CFG_0], msk, reg);
		msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_DMPULLDOWN] |
			phy_cfg->masks[MASK_ANALOG_USB20_USB20_DPPULLDOWN];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL2], msk, 0);

		msk = phy_cfg->masks[MASK_ANALOG_USB20_USB20_RESERVED];
		regmap_update_bits(phy->analog,
			phy_cfg->regs[REG_ANALOG_USB20_USB20_UTMI_CTL1], msk, 0);
		phy->is_host = false;
	}
	return 0;
}

static const struct sprd_hsphy_cfg_ops phy_v4_cfg_ops = {
	.usb_enable_ctrl = phy_v4_usb_enable_ctrl,
	.usb_phy_power_ctrl = phy_v4_usb_phy_power_ctrl,
	.usb_vbus_ctrl = phy_v4_usb_vbus_ctrl,
	.utmi_width_sel = phy_v4_utmi_width_sel,
	.reset_core = phy_v4_reset_core,
	.set_mode = phy_v4_set_mode,
};

static const struct sprd_hsphy_cfg pike2_phy_cfg = {
	.regs				= pike2_regs_layout,
	.masks				= pike2_masks_layout,
	.cfg_ops			= &phy_v1_cfg_ops,
	.parameters			= phy_pike2_parameters,
	.phy_version			= VERSION1,
	.owner				= PIKE2,
};

static const struct sprd_hsphy_cfg sharkle_phy_cfg = {
	.regs				= sharkle_regs_layout,
	.masks				= sharkle_masks_layout,
	.cfg_ops			= &phy_v1_cfg_ops,
	.parameters			= phy_sharkle_parameters,
	.phy_version			= VERSION1,
	.owner				= SHARKLE,
};

static const struct sprd_hsphy_cfg sharkl3_phy_cfg = {
	.regs				= sharkl3_regs_layout,
	.masks				= sharkl3_masks_layout,
	.cfg_ops			= &phy_v2_cfg_ops,
	.parameters			= phy_sharkl3_parameters,
	.phy_version			= VERSION2,
	.owner				= SHARKL3,
};

static const struct sprd_hsphy_cfg sharkl5_phy_cfg = {
	.regs				= sharkl5_regs_layout,
	.masks				= sharkl5_masks_layout,
	.cfg_ops			= &phy_v3_cfg_ops,
	.parameters			= phy_sharkl5_parameters,
	.phy_version			= VERSION3,
	.owner				= SHARKL5,
};

static const struct sprd_hsphy_cfg sharkl5pro_phy_cfg = {
	.regs				= sharkl5pro_regs_layout,
	.masks				= sharkl5pro_masks_layout,
	.cfg_ops			= &phy_v3_cfg_ops,
	.parameters			= phy_sharkl5pro_parameters,
	.phy_version			= VERSION3,
	.owner				= SHARKL5PRO,
};

static const struct sprd_hsphy_cfg qogirl6_phy_cfg = {
	.regs				= qogirl6_regs_layout,
	.masks				= qogirl6_masks_layout,
	.cfg_ops			= &phy_v3_cfg_ops,
	.parameters			= phy_qogirl6_parameters,
	.phy_version			= VERSION3,
	.owner				= QOGIRL6,
};

static const struct sprd_hsphy_cfg qogirn6lite_phy_cfg = {
	.regs				= qogirn6lite_regs_layout,
	.masks				= qogirn6lite_masks_layout,
	.cfg_ops			= &phy_v4_cfg_ops,
	.parameters			= phy_qogirn6lite_parameters,
	.phy_version			= VERSION4,
	.owner				= QOGIRN6LITE,
};

static const struct sprd_hsphy_cfg uis8520_phy_cfg = {
	.regs				= uis8520_regs_layout,
	.masks				= uis8520_masks_layout,
	.cfg_ops			= &phy_v4_cfg_ops,
	.parameters			= phy_uis8520_parameters,
	.phy_version			= VERSION4,
	.owner				= UIS8520,
};

static void sprd_hsphy_charger_detect_work(struct work_struct *work)
{
	struct sprd_hsphy *phy = container_of(work, struct sprd_hsphy, work);
	struct usb_phy *usb_phy = &phy->phy;

	__pm_stay_awake(phy->wake_lock);
	if (phy->event)
		usb_phy_set_charger_state(usb_phy, USB_CHARGER_PRESENT);
	else
		usb_phy_set_charger_state(usb_phy, USB_CHARGER_ABSENT);
	__pm_relax(phy->wake_lock);
}

static int sprd_hsphy_vbus_notify(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	struct usb_phy *usb_phy = container_of(nb, struct usb_phy, vbus_nb);
	struct sprd_hsphy *phy = container_of(usb_phy, struct sprd_hsphy, phy);
	const struct sprd_hsphy_cfg_ops *cfg_ops = phy_cfg->cfg_ops;

	if (phy->is_host || usb_phy->last_event == USB_EVENT_ID) {
		dev_info(phy->dev, "USB PHY is host mode\n");
		return 0;
	}
	dev_info(usb_phy->dev, "[%s] enters!\n", __func__);

	pm_wakeup_event(phy->dev, 400);

	if (event)
		/* usb vbus valid */
		cfg_ops->usb_vbus_ctrl(phy, CTRL1);
	else
		cfg_ops->usb_vbus_ctrl(phy, CTRL0);

	phy->event = event;
	queue_work(system_unbound_wq, &phy->work);

	return 0;
}

static int sprd_hostphy_set(struct usb_phy *x, int on)
{
	struct sprd_hsphy *phy = container_of(x, struct sprd_hsphy, phy);
	const struct sprd_hsphy_cfg_ops *cfg_ops = phy_cfg->cfg_ops;

	dev_info(x->dev, "[%s] enters!\n", __func__);
	return cfg_ops->set_mode(phy, on);
}

static int sprd_hsphy_init(struct usb_phy *x)
{
	struct sprd_hsphy *phy = container_of(x, struct sprd_hsphy, phy);
	const struct sprd_hsphy_cfg_ops *cfg_ops = phy_cfg->cfg_ops;
	u32 reg = 0;
	int ret = 0;

	if (atomic_read(&phy->inited)) {
		dev_dbg(x->dev, "%s is already inited!\n", __func__);
		return 0;
	}
	dev_info(x->dev, "[%s] enters!\n", __func__);

	/* Turn On VDD */
	regulator_set_voltage(phy->vdd, phy->vdd_vol, phy->vdd_vol);
	ret = regulator_enable(phy->vdd);
	if (ret)
		return ret;

	/* usb enable */
	cfg_ops->usb_enable_ctrl(phy, CTRL1);

	/* usb phy power on */
	cfg_ops->usb_phy_power_ctrl(phy, CTRL1);

	/* Restore PHY tunes */
	if (phy_cfg->phy_version == VERSION1) {
		writel_relaxed(phy->phy_tune, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_TUNE]);
		/* USB PHY write register need to delay 2ms~3ms */
		usleep_range(2000, 3000);
	}

	/* usb vbus valid */
	cfg_ops->usb_vbus_ctrl(phy, CTRL1);

	/* for SPRD phy utmi_width sel */
	cfg_ops->utmi_width_sel(phy);

	/* for SPRD phy sampler sel */
	if (phy_cfg->phy_version == VERSION1) {
		reg = readl_relaxed(phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL1]);
		reg |= phy_cfg->masks[MASK_AP_AHB_USB20_SAMPLER_SEL];
		writel_relaxed(reg, phy->base + phy_cfg->regs[REG_AP_AHB_OTG_CTRL1]);
	}

	if (!atomic_read(&phy->reset)) {
		/* USB PHY write register need to delay 2ms~3ms */
		if (phy_cfg->phy_version == VERSION1)
			usleep_range(2000, 3000);

		cfg_ops->reset_core(phy);
		atomic_set(&phy->reset, 1);
	}

	atomic_set(&phy->inited, 1);
	return 0;
}

static void sprd_hsphy_shutdown(struct usb_phy *x)
{
	struct sprd_hsphy *phy = container_of(x, struct sprd_hsphy, phy);
	const struct sprd_hsphy_cfg_ops *cfg_ops = phy_cfg->cfg_ops;

	if (!atomic_read(&phy->inited)) {
		dev_info(x->dev, "%s is already shut down\n", __func__);
		return;
	}
	dev_info(x->dev, "[%s] enters!\n", __func__);

	/* usb vbus invalid*/
	cfg_ops->usb_vbus_ctrl(phy, CTRL0);

	/* usb power down */
	cfg_ops->usb_phy_power_ctrl(phy, CTRL0);

	/* Backup PHY Tune value */
	if (phy_cfg->phy_version == VERSION1)
		phy->phy_tune = readl(phy->base + phy_cfg->regs[REG_AP_AHB_OTG_PHY_TUNE]);

	cfg_ops->usb_enable_ctrl(phy, CTRL0);

	regulator_disable(phy->vdd);

	atomic_set(&phy->inited, 0);
	atomic_set(&phy->reset, 0);
}

static ssize_t phy_tune_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct sprd_hsphy *phy = dev_get_drvdata(dev);

	if (!phy)
		return -EINVAL;

	return snprintf(buf, PAGE_SIZE, "0x%x\n", phy->phy_tune);
}

static ssize_t phy_tune_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t size)
{
	struct sprd_hsphy *phy = dev_get_drvdata(dev);

	if (!phy)
		return -EINVAL;

	if (kstrtouint(buf, 16, &phy->phy_tune) < 0)
		return -EINVAL;

	return size;
}
static DEVICE_ATTR_RW(phy_tune);

static ssize_t vdd_voltage_show(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	struct sprd_hsphy *phy = dev_get_drvdata(dev);

	if (!phy)
		return -EINVAL;

	return snprintf(buf, PAGE_SIZE, "%d\n", phy->vdd_vol);
}

static ssize_t vdd_voltage_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t size)
{
	struct sprd_hsphy *phy = dev_get_drvdata(dev);
	u32 vol;

	if (!phy)
		return -EINVAL;

	if (kstrtouint(buf, 10, &vol) < 0)
		return -EINVAL;

	if (vol < 1200000 || vol > 3750000) {
		dev_err(dev, "Invalid voltage value %d\n", vol);
		return -EINVAL;
	}
	phy->vdd_vol = vol;

	return size;
}
static DEVICE_ATTR_RW(vdd_voltage);

static ssize_t hsphy_device_eye_pattern_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct sprd_hsphy *phy = dev_get_drvdata(dev);

	if (!phy)
		return -EINVAL;

	return snprintf(buf, PAGE_SIZE, "0x%x\n", phy->device_eye_pattern);
}

static ssize_t hsphy_device_eye_pattern_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t size)
{
	struct sprd_hsphy *phy = dev_get_drvdata(dev);

	if (!phy)
		return -EINVAL;

	if (kstrtouint(buf, 16, &phy->device_eye_pattern) < 0)
		return -EINVAL;

	return size;
}
static DEVICE_ATTR_RW(hsphy_device_eye_pattern);

static ssize_t hsphy_host_eye_pattern_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct sprd_hsphy *phy = dev_get_drvdata(dev);

	if (!phy)
		return -EINVAL;

	return snprintf(buf, PAGE_SIZE, "0x%x\n", phy->host_eye_pattern);
}

static ssize_t hsphy_host_eye_pattern_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t size)
{
	struct sprd_hsphy *phy = dev_get_drvdata(dev);

	if (!phy)
		return -EINVAL;

	if (kstrtouint(buf, 16, &phy->host_eye_pattern) < 0)
		return -EINVAL;

	return size;
}

static DEVICE_ATTR_RW(hsphy_host_eye_pattern);

static struct attribute *usb_hsphy_attrs[] = {
	&dev_attr_phy_tune.attr,
	&dev_attr_vdd_voltage.attr,
	&dev_attr_hsphy_device_eye_pattern.attr,
	&dev_attr_hsphy_host_eye_pattern.attr,
	NULL
};
ATTRIBUTE_GROUPS(usb_hsphy);

static int sprd_eyepatt_tunehsamp_set(struct sprd_hsphy *phy, struct device *dev)
{
	int ret = 0;
	u8 val[2];

	ret = of_property_read_u8_array(dev->of_node, "sprd,hsphy-tunehsamp", val, 2);

	if (ret < 0) {
		dev_err(dev, "unable to get hsphy-device-tunehsamp\n");
		return ret;
	}

	if (phy_cfg->phy_version == VERSION1) {
		/* device setting */
		phy->device_otg_ctrl0 &= ~phy_cfg->masks[MASK_AP_AHB_USB20_TUNEHSAMP];
		phy->device_otg_ctrl0 |= (u32)val[0] << phy_cfg->parameters[TUNEHSAMP_SHIFT];

		/* host setting */
		phy->host_otg_ctrl0 &= ~phy_cfg->masks[MASK_AP_AHB_USB20_TUNEHSAMP];
		phy->host_otg_ctrl0 |= (u32)val[1] << phy_cfg->parameters[TUNEHSAMP_SHIFT];

	} else if (phy_cfg->phy_version == VERSION2 ||
		phy_cfg->phy_version == VERSION3 ||
		phy_cfg->phy_version == VERSION4) {
		/* device setting */
		phy->device_eye_pattern &= ~phy_cfg->masks[MASK_ANALOG_USB20_USB20_TUNEHSAMP];
		phy->device_eye_pattern |= (u32)val[0] << phy_cfg->parameters[TUNEHSAMP_SHIFT];

		/* host setting */
		phy->host_eye_pattern &= ~phy_cfg->masks[MASK_ANALOG_USB20_USB20_TUNEHSAMP];
		phy->host_eye_pattern |= (u32)val[1] << phy_cfg->parameters[TUNEHSAMP_SHIFT];
	}

	return 0;
}

static int sprd_eyepatt_tuneeq_set(struct sprd_hsphy *phy, struct device *dev)
{
	int ret = 0;
	u8 val[2];

	ret = of_property_read_u8_array(dev->of_node, "sprd,hsphy-tuneeq", val, 2);

	if (ret < 0) {
		dev_err(dev, "unable to get hsphy-tuneeq\n");
		return ret;
	}

	if (phy_cfg->phy_version == VERSION1) {
		/* device setting */
		phy->device_otg_ctrl1 &= ~phy_cfg->masks[MASK_AP_AHB_USB20_TUNEEQ];
		phy->device_otg_ctrl1 |= (u32)val[0] << phy_cfg->parameters[TUNEEQ_SHIFT];

		/* host setting */
		phy->host_otg_ctrl1 &= ~phy_cfg->masks[MASK_AP_AHB_USB20_TUNEEQ];
		phy->host_otg_ctrl1 |= (u32)val[1] << phy_cfg->parameters[TUNEEQ_SHIFT];

	} else if (phy_cfg->phy_version == VERSION2 ||
		phy_cfg->phy_version == VERSION3 ||
		phy_cfg->phy_version == VERSION4) {
		/* device setting */
		phy->device_eye_pattern &= ~phy_cfg->masks[MASK_ANALOG_USB20_USB20_TUNEEQ];
		phy->device_eye_pattern |= (u32)val[0] << phy_cfg->parameters[TUNEEQ_SHIFT];

		/* host setting */
		phy->host_eye_pattern &= ~phy_cfg->masks[MASK_ANALOG_USB20_USB20_TUNEEQ];
		phy->host_eye_pattern |= (u32)val[1] << phy_cfg->parameters[TUNEEQ_SHIFT];
	}

	return 0;
}

static int sprd_eyepatt_tfregres_set(struct sprd_hsphy *phy, struct device *dev)
{
	int ret = 0;
	u8 val[2];

	ret = of_property_read_u8_array(dev->of_node, "sprd,hsphy-tfregres", val, 2);

	if (ret < 0) {
		dev_err(dev, "unable to get hsphy-tfregres\n");
		return ret;
	}

	if (phy_cfg->phy_version == VERSION1) {
		/* device setting */
		phy->device_otg_ctrl1 &= ~phy_cfg->masks[MASK_AP_AHB_USB20_TFREGRES];
		phy->device_otg_ctrl1 |= (u32)val[0] << phy_cfg->parameters[TFREGRES_SHIFT];

		/* host setting */
		phy->host_otg_ctrl1 &= ~phy_cfg->masks[MASK_AP_AHB_USB20_TFREGRES];
		phy->host_otg_ctrl1 |= (u32)val[1] << phy_cfg->parameters[TFREGRES_SHIFT];
	} else if (phy_cfg->phy_version == VERSION2 ||
		phy_cfg->phy_version == VERSION3 ||
		phy_cfg->phy_version == VERSION4) {
		/* device setting */
		phy->device_eye_pattern &= ~phy_cfg->masks[MASK_ANALOG_USB20_USB20_TFREGRES];
		phy->device_eye_pattern |= (u32)val[0] << phy_cfg->parameters[TFREGRES_SHIFT];

		/* host setting */
		phy->host_eye_pattern &= ~phy_cfg->masks[MASK_ANALOG_USB20_USB20_TFREGRES];
		phy->host_eye_pattern |= (u32)val[1] << phy_cfg->parameters[TFREGRES_SHIFT];
	}

	return 0;
}

static int sprd_eye_pattern_prepared(struct sprd_hsphy *phy, struct device *dev)
{
	int ret = 0;

	if (phy_cfg->phy_version == VERSION1) {
		/* set default OTG_CTRL */
		phy->device_otg_ctrl0 = readl_relaxed(phy->base +
				phy_cfg->regs[REG_AP_AHB_OTG_CTRL0]);
		phy->device_otg_ctrl1 = readl_relaxed(phy->base +
				phy_cfg->regs[REG_AP_AHB_OTG_CTRL1]);

		phy->host_otg_ctrl0 = readl_relaxed(phy->base +
				phy_cfg->regs[REG_AP_AHB_OTG_CTRL0]);
		phy->host_otg_ctrl1 = readl_relaxed(phy->base +
				phy_cfg->regs[REG_AP_AHB_OTG_CTRL1]);
	} else if (phy_cfg->phy_version == VERSION2 ||
		phy_cfg->phy_version == VERSION3 ||
		phy_cfg->phy_version == VERSION4) {
		/* set default eyepatt */
		regmap_read(phy->analog, phy_cfg->regs[REG_ANALOG_USB20_USB20_TRIMMING],
					&phy->device_eye_pattern);

		regmap_read(phy->analog, phy_cfg->regs[REG_ANALOG_USB20_USB20_TRIMMING],
					&phy->host_eye_pattern);
	}

	/* set eyepatt tunehsamp */
	ret |= sprd_eyepatt_tunehsamp_set(phy, dev);

	/* set eyepatt tuneeq */
	ret |= sprd_eyepatt_tuneeq_set(phy, dev);

	/* set eyepatt tfregres */
	ret |= sprd_eyepatt_tfregres_set(phy, dev);

	return ret;
}

static int sprd_hsphy_cali_mode(void)
{
	struct device_node *cmdline_node;
	const char *cmdline, *mode;
	int ret;

	cmdline_node = of_find_node_by_path("/chosen");
	ret = of_property_read_string(cmdline_node, "bootargs", &cmdline);

	if (ret) {
		pr_err("Can't not parse bootargs\n");
		return 0;
	}

	mode = strstr(cmdline, "androidboot.mode=cali");
	if (mode)
		return 1;

	mode = strstr(cmdline, "sprdboot.mode=cali");
	if (mode)
		return 1;

	return 0;
}

static int sprd_hsphy_probe(struct platform_device *pdev)
{
	struct sprd_hsphy *phy;
	struct resource *res;
	struct device *dev = &pdev->dev;
	int ret = 0, calimode = 0;
	struct usb_otg *otg;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	otg = devm_kzalloc(dev, sizeof(*otg), GFP_KERNEL);
	if (!otg)
		return -ENOMEM;

	/* phy cfg data */
	phy_cfg = of_device_get_match_data(dev);
	if (!phy_cfg) {
		dev_err(dev, "no matching driver data found\n");
		return -EINVAL;
	}

	/* set vdd */
	ret = of_property_read_u32(dev->of_node, "sprd,vdd-voltage",
				   &phy->vdd_vol);
	if (ret < 0) {
		dev_err(dev, "unable to read hsphy vdd voltage\n");
		return ret;
	}

	calimode = sprd_hsphy_cali_mode();
	if (calimode) {
		phy->vdd_vol = phy_cfg->parameters[FULLSPEED_USB33_TUNE];
		dev_info(dev, "calimode vdd_vol:%d\n", phy->vdd_vol);
	}

	phy->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(phy->vdd)) {
		dev_err(dev, "unable to get hsphy vdd supply\n");
		return PTR_ERR(phy->vdd);
	}

	ret = regulator_set_voltage(phy->vdd, phy->vdd_vol, phy->vdd_vol);
	if (ret < 0) {
		dev_err(dev, "fail to set hsphy vdd voltage at %dmV\n",
			phy->vdd_vol);
		return ret;
	}

	/* phy tune */
	if (phy_cfg->phy_version == VERSION1) {
		ret = of_property_read_u32(dev->of_node, "sprd,tune-value",
					&phy->phy_tune);
		if (ret < 0) {
			dev_err(dev, "unable to read hsphy usb phy tune\n");
			return ret;
		}
	}

	/* phy base */
	if (phy_cfg->phy_version == VERSION1 ||
		phy_cfg->phy_version == VERSION2) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy_glb_regs");
		if (!res) {
			dev_err(dev, "missing USB PHY registers resource\n");
			return -ENODEV;
		}

		phy->base = devm_ioremap(dev, res->start, resource_size(res));
		if (IS_ERR(phy->base)) {
			dev_err(dev, "unable to get phy base!\n");
			return PTR_ERR(phy->base);
		}
	}

	/* analog & aoapb & apahb regmap */
	phy->aon_apb = syscon_regmap_lookup_by_phandle(dev->of_node,
				 "sprd,syscon-enable");
	if (IS_ERR(phy->aon_apb)) {
		dev_err(dev, "USB aon apb syscon failed!\n");
		return PTR_ERR(phy->aon_apb);
	}

	if (phy_cfg->phy_version == VERSION2) {
		phy->ap_ahb = syscon_regmap_lookup_by_phandle(dev->of_node,
				 "sprd,syscon-apahb");
		if (IS_ERR(phy->ap_ahb)) {
			dev_err(dev, "USB apahb syscon failed!\n");
			return PTR_ERR(phy->ap_ahb);
		}
	}

	if (phy_cfg->phy_version != VERSION1) {
		phy->analog = syscon_regmap_lookup_by_phandle(dev->of_node,
				 "sprd,syscon-ana");
		if (IS_ERR(phy->analog)) {
			dev_err(dev, "USB analog syscon failed!\n");
			return PTR_ERR(phy->analog);
		}
	}

	/* prepare eye pattern */
	ret = sprd_eye_pattern_prepared(phy, dev);
	if (ret < 0)
		dev_warn(dev, "sprd_eye_pattern_prepared failed, ret = %d\n", ret);

	/* enable usb module */
	if (phy_cfg->phy_version == VERSION2 ||
		phy_cfg->phy_version == VERSION3) {
		phy_cfg->cfg_ops->usb_enable_ctrl(phy, CTRL2);
	}

	/* usb phy power down */
	if (phy_cfg->phy_version != VERSION4)
		phy_cfg->cfg_ops->usb_phy_power_ctrl(phy, CTRL2);

	phy->dev = dev;
	phy->phy.dev = dev;
	phy->phy.label = "sprd-hsphy";
	phy->phy.otg = otg;
	phy->phy.init = sprd_hsphy_init;
	phy->phy.shutdown = sprd_hsphy_shutdown;
	phy->phy.set_vbus = sprd_hostphy_set;
	phy->phy.type = USB_PHY_TYPE_USB2;
	phy->phy.vbus_nb.notifier_call = sprd_hsphy_vbus_notify;
	otg->usb_phy = &phy->phy;

	device_init_wakeup(phy->dev, true);
	phy->wake_lock = wakeup_source_register(phy->dev, "sprd-hsphy");
	if (!phy->wake_lock) {
		dev_err(dev, "fail to register wakeup lock.\n");
		return -ENOMEM;
	}
	INIT_WORK(&phy->work, sprd_hsphy_charger_detect_work);
	platform_set_drvdata(pdev, phy);

	ret = usb_add_phy_dev(&phy->phy);
	if (ret) {
		dev_err(dev, "fail to add phy\n");
		return ret;
	}

	ret = sysfs_create_groups(&dev->kobj, usb_hsphy_groups);
	if (ret)
		dev_warn(dev, "failed to create usb hsphy attributes\n");

	if (extcon_get_state(phy->phy.edev, EXTCON_USB) > 0)
		usb_phy_set_charger_state(&phy->phy, USB_CHARGER_PRESENT);

	dev_info(dev, "sprd usb phy probe ok !\n");

	return ret;
}

static int sprd_hsphy_remove(struct platform_device *pdev)
{
	struct sprd_hsphy *phy = platform_get_drvdata(pdev);

	sysfs_remove_groups(&pdev->dev.kobj, usb_hsphy_groups);
	usb_remove_phy(&phy->phy);
	if (regulator_is_enabled(phy->vdd))
		regulator_disable(phy->vdd);

	return 0;
}

static const struct of_device_id sprd_hsphy_match[] = {
	{ .compatible = "sprd,pike2-phy", .data = &pike2_phy_cfg,},
	{ .compatible = "sprd,sharkle-phy", .data = &sharkle_phy_cfg,},
	{ .compatible = "sprd,sharkl3-phy", .data = &sharkl3_phy_cfg,},
	{ .compatible = "sprd,sharkl5-phy", .data = &sharkl5_phy_cfg,},
	{ .compatible = "sprd,sharkl5pro-phy", .data = &sharkl5pro_phy_cfg,},
	{ .compatible = "sprd,qogirl6-phy", .data = &qogirl6_phy_cfg,},
	{ .compatible = "sprd,qogirn6lite-phy", .data = &qogirn6lite_phy_cfg,},
	{ .compatible = "sprd,uis8520-phy", .data = &uis8520_phy_cfg,},
	{},
};
MODULE_DEVICE_TABLE(of, sprd_hsphy_match);

static struct platform_driver sprd_hsphy_driver = {
	.probe = sprd_hsphy_probe,
	.remove = sprd_hsphy_remove,
	.driver = {
		.name = "sprd-hsphy",
		.of_match_table = sprd_hsphy_match,
	},
};

static int __init sprd_hsphy_driver_init(void)
{
	return platform_driver_register(&sprd_hsphy_driver);
}

static void __exit sprd_hsphy_driver_exit(void)
{
	platform_driver_unregister(&sprd_hsphy_driver);
}

late_initcall(sprd_hsphy_driver_init);
module_exit(sprd_hsphy_driver_exit);

MODULE_ALIAS("platform:spreadtrum-usb20-hsphy");
MODULE_AUTHOR("Pu Li <lip308226@gmail.com>");
MODULE_DESCRIPTION("Spreadtrum USB20 HSPHY driver");
MODULE_LICENSE("GPL");
