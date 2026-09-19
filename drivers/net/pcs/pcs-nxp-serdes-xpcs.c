// SPDX-License-Identifier: GPL-2.0
/* NXP SerDes XPCS / XPCS-PHY shared core
 *
 * Transport agnostic phylink PCS driver for the Ethernet XPCS and Ethernet
 * PHY (XPCS-PHY) of the SerDes subsystems found on NXP S32G and i.MX SoCs.
 * The platform glue provides the regmaps and a per revision descriptor
 * with the compatibility table, quirks and PMA programming sequences.
 *
 * The XPCS is a Synopsys DesignWare core, so the Clause 37 / 2500BASE-X /
 * USXGMII / 10GBASE-R register sequences below are shared with the Synopsys
 * pcs-xpcs driver and were derived from it. This is a separate driver rather
 * than an extension of pcs-xpcs because struct dw_xpcs is built around an
 * mdio_device: it owns the transport, the reset and the identification of a
 * standalone PCS. That model does not fit these SerDes subsystems, where the
 * PMA belongs to a combo PHY shared with a PCIe controller, the bring-up is
 * sequenced across both XPCS instances by the PHY driver, and the register
 * transport is either indirect MMIO or Clause 45 MDIO. What is left here is
 * the phylink PCS layer on top of a regmap.
 *
 * Keep the register sequences in sync with drivers/net/pcs/pcs-xpcs.c when
 * fixes apply to both.
 *
 * Copyright 2023-2026 NXP
 * Copyright (c) 2020 Synopsys, Inc. and/or its affiliates.
 */

#include <linux/device.h>
#include <linux/ethtool.h>
#include <linux/iopoll.h>
#include <linux/mdio.h>
#include <linux/mii.h>
#include <linux/module.h>
#include <linux/pcs/pcs-nxp-serdes-xpcs.h>
#include <linux/phylink.h>

/* SR_MII USXGMII speed selection */
#define DW_USXGMII_RST			BIT(10)
#define DW_USXGMII_EN			BIT(9)
#define DW_USXGMII_FULL			BIT(8)
#define DW_USXGMII_SS_MASK		(BIT(13) | BIT(6) | BIT(5))
#define DW_USXGMII_10000		(BIT(13) | BIT(6))
#define DW_USXGMII_5000			(BIT(13) | BIT(5))
#define DW_USXGMII_2500			(BIT(5))
#define DW_USXGMII_1000			(BIT(6))
#define DW_USXGMII_100			(BIT(13))
#define DW_USXGMII_10			(0)

/* VR MII MMD registers offsets */
#define DW_VR_MII_DIG_CTRL1		0x8000
#define DW_VR_MII_DIG_CTRL1_MAC_AUTO_SW		BIT(9)
#define DW_VR_MII_DIG_CTRL1_2G5_EN		BIT(2)

#define DW_VR_MII_AN_CTRL		0x8001
#define DW_VR_MII_TX_CONFIG_MASK		BIT(3)
#define DW_VR_MII_TX_CONFIG_MAC_SIDE_SGMII	0x0
#define DW_VR_MII_PCS_MODE_MASK			GENMASK(2, 1)
#define DW_VR_MII_PCS_MODE_C37_SGMII		0x2
#define DW_VR_MII_AN_INTR_EN			BIT(0)

#define DW_VR_MII_AN_INTR_STS		0x8002
#define DW_VR_MII_AN_STS_C37_ANCMPLT_INTR	BIT(0)
#define DW_VR_MII_AN_STS_C37_ANSGM_FD		BIT(1)
#define DW_VR_MII_AN_STS_C37_ANSGM_SP		GENMASK(3, 2)
#define DW_VR_MII_C37_ANSGM_SP_10		0x0
#define DW_VR_MII_C37_ANSGM_SP_100		0x1
#define DW_VR_MII_C37_ANSGM_SP_1000		0x2
#define DW_VR_MII_C37_ANSGM_SP_LNKSTS		BIT(4)
#define DW_VR_MII_USX_AN_STS_MASK		GENMASK(14, 8)

#define DW_VR_MII_EEE_MCTRL0		0x8006
#define DW_VR_MII_EEE_LTX_EN			BIT(0)  /* LPI Tx Enable */
#define DW_VR_MII_EEE_LRX_EN			BIT(1)  /* LPI Rx Enable */
#define DW_VR_MII_EEE_TX_QUIET_EN		BIT(2)  /* Tx Quiet Enable */
#define DW_VR_MII_EEE_RX_QUIET_EN		BIT(3)  /* Rx Quiet Enable */
#define DW_VR_MII_EEE_TX_EN_CTRL		BIT(4)  /* Tx Control Enable */
#define DW_VR_MII_EEE_RX_EN_CTRL		BIT(7)  /* Rx Control Enable */
#define DW_VR_MII_EEE_MULT_FACT_100NS		GENMASK(11, 8)

#define DW_VR_MII_EEE_MCTRL1		0x800b
#define DW_VR_MII_EEE_TRN_LPI		BIT(0)	/* Transparent Mode Enable */

const int nxp_serdes_xpcs_usxgmii_features[] = {
	ETHTOOL_LINK_MODE_Pause_BIT,
	ETHTOOL_LINK_MODE_Asym_Pause_BIT,
	ETHTOOL_LINK_MODE_Autoneg_BIT,
	ETHTOOL_LINK_MODE_1000baseKX_Full_BIT,
	ETHTOOL_LINK_MODE_10000baseKX4_Full_BIT,
	ETHTOOL_LINK_MODE_10000baseKR_Full_BIT,
	ETHTOOL_LINK_MODE_2500baseX_Full_BIT,
	__ETHTOOL_LINK_MODE_MASK_NBITS,
};
EXPORT_SYMBOL_GPL(nxp_serdes_xpcs_usxgmii_features);

const int nxp_serdes_xpcs_2500basex_features[] = {
	ETHTOOL_LINK_MODE_Pause_BIT,
	ETHTOOL_LINK_MODE_Asym_Pause_BIT,
	ETHTOOL_LINK_MODE_Autoneg_BIT,
	ETHTOOL_LINK_MODE_2500baseX_Full_BIT,
	ETHTOOL_LINK_MODE_2500baseT_Full_BIT,
	__ETHTOOL_LINK_MODE_MASK_NBITS,
};
EXPORT_SYMBOL_GPL(nxp_serdes_xpcs_2500basex_features);

const int nxp_serdes_xpcs_sgmii_features[] = {
	ETHTOOL_LINK_MODE_Pause_BIT,
	ETHTOOL_LINK_MODE_Asym_Pause_BIT,
	ETHTOOL_LINK_MODE_Autoneg_BIT,
	ETHTOOL_LINK_MODE_10baseT_Half_BIT,
	ETHTOOL_LINK_MODE_10baseT_Full_BIT,
	ETHTOOL_LINK_MODE_100baseT_Half_BIT,
	ETHTOOL_LINK_MODE_100baseT_Full_BIT,
	ETHTOOL_LINK_MODE_1000baseT_Half_BIT,
	ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
	__ETHTOOL_LINK_MODE_MASK_NBITS,
};
EXPORT_SYMBOL_GPL(nxp_serdes_xpcs_sgmii_features);

static const struct nxp_serdes_xpcs_compat *
nxp_serdes_xpcs_find_compat(struct nxp_serdes_xpcs *xpcs,
			    phy_interface_t interface)
{
	const struct nxp_serdes_xpcs_compat *compat;

	for (compat = xpcs->desc->compat; compat->supported; compat++)
		if (compat->interface == interface)
			return compat;

	return NULL;
}

static void nxp_serdes_xpcs_get_interfaces(struct nxp_serdes_xpcs *xpcs,
					   unsigned long *interfaces)
{
	const struct nxp_serdes_xpcs_compat *compat;

	for (compat = xpcs->desc->compat; compat->supported; compat++)
		__set_bit(compat->interface, interfaces);
}

int nxp_serdes_xpcs_read(struct nxp_serdes_xpcs *xpcs, u8 devad, u32 reg)
{
	unsigned int val;
	int ret;

	ret = regmap_read(xpcs->xpcs_regmap, (devad << 16) | (reg & 0xffff),
			  &val);
	if (ret < 0)
		return ret;

	return val;
}
EXPORT_SYMBOL_GPL(nxp_serdes_xpcs_read);

int nxp_serdes_xpcs_write(struct nxp_serdes_xpcs *xpcs, u8 devad, u32 reg,
			  u16 val)
{
	return regmap_write(xpcs->xpcs_regmap, (devad << 16) | (reg & 0xffff),
			    val);
}
EXPORT_SYMBOL_GPL(nxp_serdes_xpcs_write);

int nxp_serdes_xpcs_modify(struct nxp_serdes_xpcs *xpcs, u8 devad, u32 reg,
			   u16 mask, u16 set)
{
	return regmap_update_bits(xpcs->xpcs_regmap,
				  (devad << 16) | (reg & 0xffff), mask, set);
}
EXPORT_SYMBOL_GPL(nxp_serdes_xpcs_modify);

static int nxp_serdes_xpcs_poll_reset(struct nxp_serdes_xpcs *xpcs, int dev)
{
	int ret, val;

	ret = read_poll_timeout(nxp_serdes_xpcs_read, val,
				val < 0 || !(val & BMCR_RESET),
				50000, 600000, true, xpcs, dev, MII_BMCR);
	if (val < 0)
		ret = val;

	return ret;
}

static int nxp_serdes_xpcs_soft_reset(struct nxp_serdes_xpcs *xpcs,
				      const struct nxp_serdes_xpcs_compat *compat)
{
	int ret, dev;

	switch (compat->an_mode) {
	case NXP_SERDES_10GBASER:
		dev = MDIO_MMD_PCS;
		break;
	case NXP_SERDES_AN_C37_SGMII:
	case NXP_SERDES_AN_C37_USXGMII:
	case NXP_SERDES_2500BASEX:
		dev = MDIO_MMD_VEND2;
		break;
	default:
		return -EINVAL;
	}

	ret = nxp_serdes_xpcs_write(xpcs, dev, MII_BMCR, BMCR_RESET);
	if (ret < 0)
		return ret;

	return nxp_serdes_xpcs_poll_reset(xpcs, dev);
}

static int nxp_serdes_xpcs_validate(struct phylink_pcs *pcs,
				    unsigned long *supported,
				    const struct phylink_link_state *state)
{
	__ETHTOOL_DECLARE_LINK_MODE_MASK(xpcs_supported) = { 0, };
	const struct nxp_serdes_xpcs_compat *compat;
	struct nxp_serdes_xpcs *xpcs;
	int i;

	xpcs = phylink_pcs_to_nxp_serdes_xpcs(pcs);
	compat = nxp_serdes_xpcs_find_compat(xpcs, state->interface);
	if (!compat)
		return -EINVAL;

	for (i = 0; compat->supported[i] != __ETHTOOL_LINK_MODE_MASK_NBITS; i++)
		set_bit(compat->supported[i], xpcs_supported);

	linkmode_and(supported, supported, xpcs_supported);

	return 0;
}

static unsigned int nxp_serdes_xpcs_inband_caps(struct phylink_pcs *pcs,
						phy_interface_t interface)
{
	struct nxp_serdes_xpcs *xpcs = phylink_pcs_to_nxp_serdes_xpcs(pcs);
	const struct nxp_serdes_xpcs_compat *compat;

	compat = nxp_serdes_xpcs_find_compat(xpcs, interface);
	if (!compat)
		return 0;

	switch (compat->an_mode) {
	case NXP_SERDES_AN_C37_USXGMII:
		return LINK_INBAND_ENABLE;

	case NXP_SERDES_AN_C37_SGMII:
		return LINK_INBAND_DISABLE | LINK_INBAND_ENABLE;

	case NXP_SERDES_10GBASER:
	case NXP_SERDES_2500BASEX:
		return LINK_INBAND_DISABLE;

	default:
		return 0;
	}
}

static void nxp_serdes_xpcs_disable(struct phylink_pcs *pcs)
{
	struct nxp_serdes_xpcs *xpcs = phylink_pcs_to_nxp_serdes_xpcs(pcs);

	if (xpcs->desc->reset)
		xpcs->desc->reset(xpcs);
}

static void nxp_serdes_xpcs_pre_config(struct phylink_pcs *pcs,
				       phy_interface_t interface)
{
	struct nxp_serdes_xpcs *xpcs = phylink_pcs_to_nxp_serdes_xpcs(pcs);
	const struct nxp_serdes_xpcs_compat *compat;
	int ret;

	if (xpcs->interface != interface) {
		if (interface == PHY_INTERFACE_MODE_SGMII)
			xpcs->need_reset = true;
		xpcs->interface = interface;
	}

	if (xpcs->desc->quirks & NXP_SERDES_QUIRK_NO_POLL)
		xpcs->pcs.poll = false;

	if (xpcs->desc->quirks & NXP_SERDES_QUIRK_NO_SOFT_RESET)
		xpcs->need_reset = false;

	if (!xpcs->need_reset)
		return;

	compat = nxp_serdes_xpcs_find_compat(xpcs, interface);
	if (!compat) {
		dev_err(xpcs->dev, "unsupported interface %s\n",
			phy_modes(interface));
		return;
	}

	ret = nxp_serdes_xpcs_soft_reset(xpcs, compat);
	if (ret)
		dev_err(xpcs->dev, "soft reset failed: %pe\n", ERR_PTR(ret));

	xpcs->need_reset = false;
}

static int nxp_serdes_xpcs_config_aneg_c37_sgmii(struct nxp_serdes_xpcs *xpcs,
						 unsigned int neg_mode)
{
	int ret, mdio_ctrl;
	u16 mask, val;

	/* Disable AN while PCS_MODE and TX_CONFIG are switched to C37 SGMII on
	 * the MAC side, then re-enable it for in-band mode. MAC_AUTO_SW lets
	 * the hardware apply the negotiated speed and duplex.
	 */
	mdio_ctrl = nxp_serdes_xpcs_read(xpcs, MDIO_MMD_VEND2, MII_BMCR);
	if (mdio_ctrl < 0)
		return mdio_ctrl;

	if (mdio_ctrl & BMCR_ANENABLE) {
		ret = nxp_serdes_xpcs_write(xpcs, MDIO_MMD_VEND2, MII_BMCR,
					    mdio_ctrl & ~BMCR_ANENABLE);
		if (ret < 0)
			return ret;
	}

	mask = DW_VR_MII_PCS_MODE_MASK | DW_VR_MII_TX_CONFIG_MASK;
	val = FIELD_PREP(DW_VR_MII_PCS_MODE_MASK,
			 DW_VR_MII_PCS_MODE_C37_SGMII);
	val |= FIELD_PREP(DW_VR_MII_TX_CONFIG_MASK,
			  DW_VR_MII_TX_CONFIG_MAC_SIDE_SGMII);

	ret = nxp_serdes_xpcs_modify(xpcs, MDIO_MMD_VEND2, DW_VR_MII_AN_CTRL,
				     mask, val);
	if (ret < 0)
		return ret;

	val = 0;
	mask = DW_VR_MII_DIG_CTRL1_2G5_EN | DW_VR_MII_DIG_CTRL1_MAC_AUTO_SW;

	if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED)
		val = DW_VR_MII_DIG_CTRL1_MAC_AUTO_SW;

	ret = nxp_serdes_xpcs_modify(xpcs, MDIO_MMD_VEND2, DW_VR_MII_DIG_CTRL1,
				     mask, val);
	if (ret < 0)
		return ret;

	if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED)
		ret = nxp_serdes_xpcs_write(xpcs, MDIO_MMD_VEND2, MII_BMCR,
					    mdio_ctrl | BMCR_ANENABLE);

	return ret;
}

static int nxp_serdes_xpcs_config_2500basex(struct nxp_serdes_xpcs *xpcs)
{
	int ret;

	ret = nxp_serdes_xpcs_modify(xpcs, MDIO_MMD_VEND2, DW_VR_MII_DIG_CTRL1,
				     DW_VR_MII_DIG_CTRL1_2G5_EN |
				     DW_VR_MII_DIG_CTRL1_MAC_AUTO_SW,
				     DW_VR_MII_DIG_CTRL1_2G5_EN);
	if (ret < 0)
		return ret;

	return nxp_serdes_xpcs_modify(xpcs, MDIO_MMD_VEND2, MII_BMCR,
				      BMCR_ANENABLE | BMCR_SPEED1000 |
				      BMCR_SPEED100, BMCR_SPEED1000);
}

static int nxp_serdes_xpcs_config_10gbaser(struct nxp_serdes_xpcs *xpcs)
{
	return nxp_serdes_xpcs_modify(xpcs, MDIO_MMD_VEND2, DW_VR_MII_DIG_CTRL1,
				      DW_VR_MII_DIG_CTRL1_MAC_AUTO_SW, 0);
}

static int nxp_serdes_xpcs_do_config(struct nxp_serdes_xpcs *xpcs,
				     phy_interface_t interface,
				     const unsigned long *advertising,
				     unsigned int neg_mode)
{
	const struct nxp_serdes_xpcs_compat *compat;
	int ret;

	compat = nxp_serdes_xpcs_find_compat(xpcs, interface);
	if (!compat)
		return -ENODEV;

	switch (compat->an_mode) {
	case NXP_SERDES_10GBASER:
		ret = nxp_serdes_xpcs_config_10gbaser(xpcs);
		if (ret)
			return ret;
		break;
	case NXP_SERDES_AN_C37_USXGMII:
		break;
	case NXP_SERDES_AN_C37_SGMII:
		ret = nxp_serdes_xpcs_config_aneg_c37_sgmii(xpcs, neg_mode);
		if (ret)
			return ret;
		break;
	case NXP_SERDES_2500BASEX:
		ret = nxp_serdes_xpcs_config_2500basex(xpcs);
		if (ret)
			return ret;
		break;
	default:
		return -EINVAL;
	}

	if (compat->pma_config) {
		ret = compat->pma_config(xpcs);
		if (ret)
			return ret;
	}

	return 0;
}

static int nxp_serdes_xpcs_config(struct phylink_pcs *pcs,
				  unsigned int neg_mode,
				  phy_interface_t interface,
				  const unsigned long *advertising,
				  bool permit_pause_to_mac)
{
	struct nxp_serdes_xpcs *xpcs = phylink_pcs_to_nxp_serdes_xpcs(pcs);

	return nxp_serdes_xpcs_do_config(xpcs, interface, advertising, neg_mode);
}

static int nxp_serdes_xpcs_get_state_c37_sgmii(struct nxp_serdes_xpcs *xpcs,
					       unsigned int neg_mode,
					       struct phylink_link_state *state)
{
	int ret;

	state->link = false;
	state->speed = SPEED_UNKNOWN;
	state->duplex = DUPLEX_UNKNOWN;
	state->pause = 0;

	/* Until AN completes, the status still holds the previous SGMII word */
	if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED) {
		ret = nxp_serdes_xpcs_read(xpcs, MDIO_MMD_VEND2, MII_BMSR);
		if (ret < 0)
			return ret;

		if (!(ret & BMSR_ANEGCOMPLETE))
			return 0;
	}

	ret = nxp_serdes_xpcs_read(xpcs, MDIO_MMD_VEND2, DW_VR_MII_AN_INTR_STS);
	if (ret < 0)
		return ret;

	state->an_complete = ret & DW_VR_MII_AN_STS_C37_ANCMPLT_INTR;

	if (ret & DW_VR_MII_C37_ANSGM_SP_LNKSTS) {
		int speed_value;

		state->link = true;

		speed_value = FIELD_GET(DW_VR_MII_AN_STS_C37_ANSGM_SP, ret);
		if (speed_value == DW_VR_MII_C37_ANSGM_SP_1000)
			state->speed = SPEED_1000;
		else if (speed_value == DW_VR_MII_C37_ANSGM_SP_100)
			state->speed = SPEED_100;
		else
			state->speed = SPEED_10;

		if (ret & DW_VR_MII_AN_STS_C37_ANSGM_FD)
			state->duplex = DUPLEX_FULL;
		else
			state->duplex = DUPLEX_HALF;
	} else if (ret == DW_VR_MII_AN_STS_C37_ANCMPLT_INTR) {
		int speed, duplex;

		state->link = true;

		speed = nxp_serdes_xpcs_read(xpcs, MDIO_MMD_VEND2, MII_BMCR);
		if (speed < 0)
			return speed;

		speed &= BMCR_SPEED100 | BMCR_SPEED1000;
		if (speed == BMCR_SPEED1000)
			state->speed = SPEED_1000;
		else if (speed == BMCR_SPEED100)
			state->speed = SPEED_100;
		else if (speed == 0)
			state->speed = SPEED_10;

		duplex = nxp_serdes_xpcs_read(xpcs, MDIO_MMD_VEND2,
					      MII_ADVERTISE);
		if (duplex < 0)
			return duplex;

		if (duplex & ADVERTISE_1000XFULL)
			state->duplex = DUPLEX_FULL;
		else if (duplex & ADVERTISE_1000XHALF)
			state->duplex = DUPLEX_HALF;

		nxp_serdes_xpcs_write(xpcs, MDIO_MMD_VEND2,
				      DW_VR_MII_AN_INTR_STS, 0);
	}

	return 0;
}

static int nxp_serdes_xpcs_get_state_2500basex(struct nxp_serdes_xpcs *xpcs,
					       struct phylink_link_state *state)
{
	int ret;

	ret = nxp_serdes_xpcs_read(xpcs, MDIO_MMD_VEND2, MII_BMSR);
	if (ret < 0) {
		state->link = 0;
		return ret;
	}

	state->link = !!(ret & BMSR_LSTATUS);
	if (!state->link)
		return 0;

	state->speed = SPEED_2500;
	state->pause |= MLO_PAUSE_TX | MLO_PAUSE_RX;
	state->duplex = DUPLEX_FULL;

	return 0;
}

static int nxp_serdes_xpcs_get_state_c37_usxgmii(struct nxp_serdes_xpcs *xpcs,
						 struct phylink_link_state *state)
{
	u16 usxg_an_status;
	int ret;

	state->link = false;
	state->speed = SPEED_UNKNOWN;
	state->duplex = DUPLEX_UNKNOWN;
	state->pause = 0;

	ret = nxp_serdes_xpcs_read(xpcs, MDIO_MMD_VEND2, DW_VR_MII_AN_INTR_STS);
	if (ret < 0)
		return ret;

	usxg_an_status = FIELD_GET(DW_VR_MII_USX_AN_STS_MASK, ret);
	if (!(usxg_an_status & BIT(6)))
		return 0;

	state->link = true;
	state->an_complete = true;
	phylink_decode_usxgmii_word(state, usxg_an_status << 7);

	return 0;
}

static void nxp_serdes_xpcs_get_state(struct phylink_pcs *pcs,
				      unsigned int neg_mode,
				      struct phylink_link_state *state)
{
	struct nxp_serdes_xpcs *xpcs = phylink_pcs_to_nxp_serdes_xpcs(pcs);
	const struct nxp_serdes_xpcs_compat *compat;
	int stat1;
	int ret;

	compat = nxp_serdes_xpcs_find_compat(xpcs, state->interface);
	if (!compat)
		return;

	switch (compat->an_mode) {
	case NXP_SERDES_10GBASER:
		stat1 = nxp_serdes_xpcs_read(xpcs, MDIO_MMD_PCS, MDIO_STAT1);
		if (stat1 < 0) {
			state->link = false;
			break;
		}

		if (stat1 & MDIO_STAT1_FAULT)
			nxp_serdes_xpcs_do_config(xpcs, state->interface, NULL,
						  PHYLINK_PCS_NEG_NONE);

		state->link = !!(stat1 & MDIO_STAT1_LSTATUS);
		if (state->link &&
		    state->interface == PHY_INTERFACE_MODE_10GBASER) {
			state->speed = SPEED_10000;
			state->duplex = DUPLEX_FULL;
		}
		break;
	case NXP_SERDES_AN_C37_USXGMII:
		ret = nxp_serdes_xpcs_get_state_c37_usxgmii(xpcs, state);
		if (ret)
			dev_err(xpcs->dev, "%s returned %pe\n",
				"nxp_serdes_xpcs_get_state_c37_usxgmii",
				ERR_PTR(ret));
		break;
	case NXP_SERDES_AN_C37_SGMII:
		ret = nxp_serdes_xpcs_get_state_c37_sgmii(xpcs, neg_mode, state);
		if (ret)
			dev_err(xpcs->dev, "%s returned %pe\n",
				"nxp_serdes_xpcs_get_state_c37_sgmii",
				ERR_PTR(ret));
		break;
	case NXP_SERDES_2500BASEX:
		ret = nxp_serdes_xpcs_get_state_2500basex(xpcs, state);
		if (ret)
			dev_err(xpcs->dev, "%s returned %pe\n",
				"nxp_serdes_xpcs_get_state_2500basex",
				ERR_PTR(ret));
		break;
	default:
		return;
	}
}

static void nxp_serdes_xpcs_link_up_sgmii_1000basex(struct nxp_serdes_xpcs *xpcs,
						    unsigned int neg_mode,
						    phy_interface_t interface,
						    int speed, int duplex)
{
	int ret;

	if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED)
		return;

	if (interface == PHY_INTERFACE_MODE_1000BASEX) {
		if (speed != SPEED_1000) {
			dev_err(xpcs->dev, "%s: speed %dMbps not supported\n",
				__func__, speed);
			return;
		}

		if (duplex != DUPLEX_FULL)
			dev_err(xpcs->dev, "%s: half duplex not supported\n",
				__func__);
	}

	ret = nxp_serdes_xpcs_write(xpcs, MDIO_MMD_VEND2, MII_BMCR,
				    mii_bmcr_encode_fixed(speed, duplex));
	if (ret)
		dev_err(xpcs->dev, "%s: write returned %pe\n",
			__func__, ERR_PTR(ret));
}

static void nxp_serdes_xpcs_link_up_usxgmii(struct nxp_serdes_xpcs *xpcs,
					    int speed)
{
	int ret, speed_sel;

	switch (speed) {
	case SPEED_10:
		speed_sel = DW_USXGMII_10;
		break;
	case SPEED_100:
		speed_sel = DW_USXGMII_100;
		break;
	case SPEED_1000:
		speed_sel = DW_USXGMII_1000;
		break;
	case SPEED_2500:
		speed_sel = DW_USXGMII_2500;
		break;
	case SPEED_5000:
		speed_sel = DW_USXGMII_5000;
		break;
	case SPEED_10000:
		speed_sel = DW_USXGMII_10000;
		break;
	default:
		return;
	}

	ret = nxp_serdes_xpcs_modify(xpcs, MDIO_MMD_PCS, BIT(15) | MDIO_CTRL1,
				     DW_USXGMII_EN, DW_USXGMII_EN);
	if (ret < 0)
		goto out;

	ret = nxp_serdes_xpcs_modify(xpcs, MDIO_MMD_VEND2, MII_BMCR,
				     DW_USXGMII_SS_MASK,
				     speed_sel | DW_USXGMII_FULL);
	if (ret < 0)
		goto out;

	ret = nxp_serdes_xpcs_modify(xpcs, MDIO_MMD_PCS, BIT(15) | MDIO_CTRL1,
				     DW_USXGMII_RST, DW_USXGMII_RST);
	if (ret < 0)
		goto out;

	return;

out:
	dev_err(xpcs->dev, "%s: XPCS access returned %pe\n",
		__func__, ERR_PTR(ret));
}

static void nxp_serdes_xpcs_link_up(struct phylink_pcs *pcs,
				    unsigned int neg_mode,
				    phy_interface_t interface, int speed,
				    int duplex)
{
	struct nxp_serdes_xpcs *xpcs = phylink_pcs_to_nxp_serdes_xpcs(pcs);
	const struct nxp_serdes_xpcs_compat *compat;
	int ret;

	switch (interface) {
	case PHY_INTERFACE_MODE_USXGMII:
		nxp_serdes_xpcs_link_up_usxgmii(xpcs, speed);
		break;

	case PHY_INTERFACE_MODE_SGMII:
	case PHY_INTERFACE_MODE_1000BASEX:
		nxp_serdes_xpcs_link_up_sgmii_1000basex(xpcs, neg_mode,
							interface, speed,
							duplex);
		break;

	default:
		break;
	}

	compat = nxp_serdes_xpcs_find_compat(xpcs, interface);
	if (compat && compat->pma_link_up) {
		ret = compat->pma_link_up(xpcs, speed, duplex);
		if (ret)
			dev_err(xpcs->dev, "%s: pma_link_up returned %pe\n",
				__func__, ERR_PTR(ret));
	}
}

static void nxp_serdes_xpcs_an_restart(struct phylink_pcs *pcs)
{
	struct nxp_serdes_xpcs *xpcs = phylink_pcs_to_nxp_serdes_xpcs(pcs);

	nxp_serdes_xpcs_modify(xpcs, MDIO_MMD_VEND2, MII_BMCR, BMCR_ANRESTART,
			       BMCR_ANRESTART);
}

static int nxp_serdes_xpcs_config_eee(struct nxp_serdes_xpcs *xpcs,
				      bool enable)
{
	u16 mask, val;
	int ret;

	mask = DW_VR_MII_EEE_LTX_EN | DW_VR_MII_EEE_LRX_EN |
	       DW_VR_MII_EEE_TX_QUIET_EN | DW_VR_MII_EEE_RX_QUIET_EN |
	       DW_VR_MII_EEE_TX_EN_CTRL | DW_VR_MII_EEE_RX_EN_CTRL |
	       DW_VR_MII_EEE_MULT_FACT_100NS;

	if (enable)
		val = DW_VR_MII_EEE_LTX_EN | DW_VR_MII_EEE_LRX_EN |
		      DW_VR_MII_EEE_TX_QUIET_EN | DW_VR_MII_EEE_RX_QUIET_EN |
		      DW_VR_MII_EEE_TX_EN_CTRL | DW_VR_MII_EEE_RX_EN_CTRL |
		      /* No platform needs a non-default multiply factor */
		      FIELD_PREP(DW_VR_MII_EEE_MULT_FACT_100NS, 0);
	else
		val = 0;

	ret = nxp_serdes_xpcs_modify(xpcs, MDIO_MMD_VEND2,
				     DW_VR_MII_EEE_MCTRL0, mask, val);
	if (ret < 0)
		return ret;

	return nxp_serdes_xpcs_modify(xpcs, MDIO_MMD_VEND2,
				      DW_VR_MII_EEE_MCTRL1,
				      DW_VR_MII_EEE_TRN_LPI,
				      enable ? DW_VR_MII_EEE_TRN_LPI : 0);
}

static void nxp_serdes_xpcs_disable_eee(struct phylink_pcs *pcs)
{
	struct nxp_serdes_xpcs *xpcs = phylink_pcs_to_nxp_serdes_xpcs(pcs);

	if (xpcs->desc->quirks & NXP_SERDES_QUIRK_NO_EEE)
		return;

	nxp_serdes_xpcs_config_eee(xpcs, false);
}

static void nxp_serdes_xpcs_enable_eee(struct phylink_pcs *pcs)
{
	struct nxp_serdes_xpcs *xpcs = phylink_pcs_to_nxp_serdes_xpcs(pcs);

	if (xpcs->desc->quirks & NXP_SERDES_QUIRK_NO_EEE)
		return;

	nxp_serdes_xpcs_config_eee(xpcs, true);
}

static const struct phylink_pcs_ops nxp_serdes_xpcs_phylink_ops = {
	.pcs_validate = nxp_serdes_xpcs_validate,
	.pcs_inband_caps = nxp_serdes_xpcs_inband_caps,
	.pcs_pre_config = nxp_serdes_xpcs_pre_config,
	.pcs_disable = nxp_serdes_xpcs_disable,
	.pcs_config = nxp_serdes_xpcs_config,
	.pcs_get_state = nxp_serdes_xpcs_get_state,
	.pcs_an_restart = nxp_serdes_xpcs_an_restart,
	.pcs_link_up = nxp_serdes_xpcs_link_up,
	.pcs_disable_eee = nxp_serdes_xpcs_disable_eee,
	.pcs_enable_eee = nxp_serdes_xpcs_enable_eee,
};

/**
 * nxp_serdes_xpcs_create() - create a NXP SerDes XPCS phylink PCS
 * @dev: device the PCS belongs to, used for error reporting
 * @xpcs_regmap: regmap of the XPCS registers
 * @phy_regmap: regmap of the XPCS-PHY registers, may be NULL
 * @desc: revision descriptor, see &struct nxp_serdes_xpcs_desc
 * @portid: index of the XPCS port, used to select the shared MPLL
 * @interface: PHY interface mode used for the link
 *
 * The regmaps must stay valid until nxp_serdes_xpcs_destroy() has been called.
 *
 * Return: a pointer to a &struct phylink_pcs, or an ERR_PTR() on failure.
 */
struct phylink_pcs *nxp_serdes_xpcs_create(struct device *dev,
					   struct regmap *xpcs_regmap,
					   struct regmap *phy_regmap,
					   const struct nxp_serdes_xpcs_desc *desc,
					   u8 portid, phy_interface_t interface)
{
	struct nxp_serdes_xpcs *xpcs;

	if (!desc || !desc->compat)
		return ERR_PTR(-EINVAL);

	xpcs = kzalloc_obj(struct nxp_serdes_xpcs, GFP_KERNEL);
	if (!xpcs)
		return ERR_PTR(-ENOMEM);

	xpcs->dev = dev;
	xpcs->xpcs_regmap = xpcs_regmap;
	xpcs->phy_regmap = phy_regmap;
	xpcs->desc = desc;
	xpcs->pma_id = desc->pma_id;
	xpcs->portid = portid;
	xpcs->interface = interface;
	xpcs->pcs.ops = &nxp_serdes_xpcs_phylink_ops;
	xpcs->pcs.poll = true;
	nxp_serdes_xpcs_get_interfaces(xpcs, xpcs->pcs.supported_interfaces);
	xpcs->need_reset = true;

	return &xpcs->pcs;
}
EXPORT_SYMBOL_GPL(nxp_serdes_xpcs_create);

void nxp_serdes_xpcs_destroy(struct phylink_pcs *pcs)
{
	struct nxp_serdes_xpcs *xpcs = phylink_pcs_to_nxp_serdes_xpcs(pcs);

	if (!xpcs)
		return;

	kfree(xpcs);
}
EXPORT_SYMBOL_GPL(nxp_serdes_xpcs_destroy);

MODULE_DESCRIPTION("NXP SerDes XPCS shared core");
MODULE_AUTHOR("Clark Wang <xiaoning.wang@nxp.com>");
MODULE_LICENSE("GPL");
