// SPDX-License-Identifier: GPL-2.0-only
#include "stmmac.h"
#include "stmmac_pcs.h"
#include "stmmac_serdes.h"

/*
 * GMAC_AN_STATUS is equivalent to MII_BMSR
 * GMAC_ANE_ADV is equivalent to 802.3z MII_ADVERTISE
 * GMAC_ANE_LPA is equivalent to 802.3z MII_LPA
 * GMAC_ANE_EXP is equivalent to MII_EXPANSION
 * GMAC_TBI is equivalent to MII_ESTATUS
 *
 * ADV, LPA and EXP are only available for the TBI and RTBI modes.
 */
#define GMAC_AN_STATUS	0x04	/* AN status */
#define GMAC_ANE_ADV	0x08	/* ANE Advertisement */
#define GMAC_ANE_LPA	0x0c	/* ANE link partener ability */
#define GMAC_TBI	0x14	/* TBI extend status */

/*
 * RGSMII status bitfield definitions.
 */
#define GMAC_RGSMII_LNKMOD		BIT(0)
#define GMAC_RGSMII_SPEED_MASK		GENMASK(2, 1)
#define GMAC_RGSMII_SPEED_125		2
#define GMAC_RGSMII_SPEED_25		1
#define GMAC_RGSMII_SPEED_2_5		0
#define GMAC_RGSMII_LNKSTS		BIT(3)

static enum ethtool_link_mode_bit_indices dwmac_hd_mode_bits[] = {
	ETHTOOL_LINK_MODE_10baseT_Half_BIT,
	ETHTOOL_LINK_MODE_100baseT_Half_BIT,
	ETHTOOL_LINK_MODE_1000baseT_Half_BIT,
	ETHTOOL_LINK_MODE_100baseFX_Half_BIT,
	ETHTOOL_LINK_MODE_10baseT1S_Half_BIT,
	ETHTOOL_LINK_MODE_10baseT1S_P2MP_Half_BIT,
};

static int dwmac_integrated_pcs_validate(struct phylink_pcs *pcs,
					 unsigned long *supported,
					 const struct phylink_link_state *state)
{
	struct stmmac_pcs *spcs = phylink_pcs_to_stmmac_pcs(pcs);
	size_t i;
	u32 val;

	if (phy_interface_mode_is_8023z(state->interface)) {
		/* ESTATUS_1000_XFULL is always set, so full duplex is
		 * supported. ESTATUS_1000_XHALF depends on core configuration.
		 */
		val = readl(spcs->base + GMAC_TBI);
		if (~val & ESTATUS_1000_XHALF)
			for (i = 0; i < ARRAY_SIZE(dwmac_hd_mode_bits); i++)
				linkmode_clear_bit(dwmac_hd_mode_bits[i],
						   supported);

		return 0;
	} else if (state->interface == PHY_INTERFACE_MODE_SGMII) {
		return 0;
	}

	return -EINVAL;
}

static unsigned int dwmac_integrated_pcs_inband_caps(struct phylink_pcs *pcs,
						     phy_interface_t interface)
{
	if (phy_interface_mode_is_8023z(interface) ||
	    interface == PHY_INTERFACE_MODE_SGMII)
		return LINK_INBAND_ENABLE | LINK_INBAND_DISABLE;

	return 0;
}

static int dwmac_integrated_pcs_enable(struct phylink_pcs *pcs)
{
	struct stmmac_pcs *spcs = phylink_pcs_to_stmmac_pcs(pcs);
	struct stmmac_priv *priv = spcs->priv;
	int ret;

	ret = dwmac_serdes_power_on(priv);
	if (ret)
		return ret;

	if (spcs->interface != PHY_INTERFACE_MODE_NA) {
		ret = dwmac_serdes_set_mode(priv, spcs->interface);
		if (ret) {
			dwmac_serdes_power_off(priv);
			return ret;
		}
	}

	stmmac_mac_irq_modify(priv, 0, spcs->int_mask);

	return 0;
}

static void dwmac_integrated_pcs_disable(struct phylink_pcs *pcs)
{
	struct stmmac_pcs *spcs = phylink_pcs_to_stmmac_pcs(pcs);
	struct stmmac_priv *priv = spcs->priv;

	stmmac_mac_irq_modify(priv, spcs->int_mask, 0);

	dwmac_serdes_power_off(priv);
}

static void dwmac_integrated_pcs_get_state(struct phylink_pcs *pcs,
					   unsigned int neg_mode,
					   struct phylink_link_state *state)
{
	struct stmmac_pcs *spcs = phylink_pcs_to_stmmac_pcs(pcs);
	u32 status, lpa, rgsmii;

	status = readl(spcs->base + GMAC_AN_STATUS);

	if (phy_interface_mode_is_8023z(state->interface)) {
		/* For 802.3z modes, the PCS block supports the advertisement
		 * and link partner advertisement registers using standard
		 * 802.3 format. The status register also has the link status
		 * and AN complete bits in the same bit location.
		 */
		lpa = readl(spcs->base + GMAC_ANE_LPA);

		phylink_mii_c22_pcs_decode_state(state, neg_mode, status, lpa);
	} else {
		rgsmii = field_get(spcs->rgsmii_status_mask,
				   readl(spcs->rgsmii));

		state->link = status & BMSR_LSTATUS &&
			      rgsmii & GMAC_RGSMII_LNKSTS;

		if (state->link && neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED) {
			/* FIXME: fill in speed and duplex. This requires the
			 * contents of the dwmac1000 GMAC_RGSMIIS or dwmac4
			 * GMAC_PHYIF_CONTROL_STATUS register.
			 */
			state->duplex = rgsmii & GMAC_RGSMII_LNKMOD ?
					DUPLEX_FULL : DUPLEX_HALF;
			switch (FIELD_GET(GMAC_RGSMII_SPEED_MASK, rgsmii)) {
			case GMAC_RGSMII_SPEED_2_5:
				state->speed = SPEED_10;
				break;

			case GMAC_RGSMII_SPEED_25:
				state->speed = SPEED_100;
				break;

			case GMAC_RGSMII_SPEED_125:
				state->speed = SPEED_1000;
				break;

			default:
				state->link = false;
				break;
			}
		}
	}
}

static int dwmac_integrated_pcs_config(struct phylink_pcs *pcs,
				       unsigned int neg_mode,
				       phy_interface_t interface,
				       const unsigned long *advertising,
				       bool permit_pause_to_mac)
{
	struct stmmac_pcs *spcs = phylink_pcs_to_stmmac_pcs(pcs);
	void __iomem *an_control = spcs->base + GMAC_AN_CTRL(0);
	bool changed = false;
	u32 adv, ctrl;
	int ret;

	if (spcs->interface != interface) {
		ret = dwmac_serdes_set_mode(spcs->priv, interface);
		if (ret)
			return ret;

		spcs->interface = interface;
	}

	if (phy_interface_mode_is_8023z(interface)) {
		adv = phylink_mii_c22_pcs_encode_advertisement(interface,
							       advertising);
		if (readl(spcs->base + GMAC_ANE_ADV) != adv)
			changed = true;
		writel(adv, spcs->base + GMAC_ANE_ADV);
	}

	ctrl = readl(an_control) & ~(GMAC_AN_CTRL_ANE | GMAC_AN_CTRL_SGMRAL);
	if (spcs->priv->hw->reverse_sgmii_enable)
		ctrl |= GMAC_AN_CTRL_SGMRAL | GMAC_AN_CTRL_ANE;
	else if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED)
		ctrl |= GMAC_AN_CTRL_ANE;
	else
		ctrl |= GMAC_AN_CTRL_SGMRAL;
	writel(ctrl, an_control);

	return changed;
}

static const struct phylink_pcs_ops dwmac_integrated_pcs_ops = {
	.pcs_validate = dwmac_integrated_pcs_validate,
	.pcs_inband_caps = dwmac_integrated_pcs_inband_caps,
	.pcs_enable = dwmac_integrated_pcs_enable,
	.pcs_disable = dwmac_integrated_pcs_disable,
	.pcs_get_state = dwmac_integrated_pcs_get_state,
	.pcs_config = dwmac_integrated_pcs_config,
};

void stmmac_integrated_pcs_irq(struct stmmac_priv *priv, u32 status,
			       struct stmmac_extra_stats *x)
{
	struct stmmac_pcs *spcs = priv->integrated_pcs;
	u32 val = readl(spcs->base + GMAC_AN_STATUS);

	if (status & PCS_ANE_IRQ) {
		x->irq_pcs_ane_n++;
		if (val & BMSR_ANEGCOMPLETE)
			dev_info(priv->device,
				 "PCS ANE process completed\n");
	}

	if (status & PCS_LINK_IRQ) {
		x->irq_pcs_link_n++;
		dev_info(priv->device, "PCS Link %s\n",
			 val & BMSR_LSTATUS ? "Up" : "Down");

		phylink_pcs_change(&spcs->pcs, val & BMSR_LSTATUS);
	}
}

int stmmac_integrated_pcs_get_phy_intf_sel(struct stmmac_priv *priv,
					   phy_interface_t interface)
{
	if (interface == PHY_INTERFACE_MODE_SGMII)
		return PHY_INTF_SEL_SGMII;

	if (phy_interface_mode_is_8023z(interface))
		return PHY_INTF_SEL_TBI;

	return -EINVAL;
}

int stmmac_integrated_pcs_init(struct stmmac_priv *priv,
			       const struct stmmac_pcs_info *pcs_info)
{
	struct stmmac_pcs *spcs;
	int ret;

	spcs = devm_kzalloc(priv->device, sizeof(*spcs), GFP_KERNEL);
	if (!spcs)
		return -ENOMEM;

	spcs->priv = priv;
	spcs->base = priv->ioaddr + pcs_info->pcs_offset;
	spcs->rgsmii = priv->ioaddr + pcs_info->rgsmii_offset;
	spcs->rgsmii_status_mask = pcs_info->rgsmii_status_mask;
	spcs->int_mask = pcs_info->int_mask;
	spcs->pcs.ops = &dwmac_integrated_pcs_ops;

	if (priv->plat->serdes) {
		ret = dwmac_serdes_validate(priv, PHY_INTERFACE_MODE_SGMII);
		if (ret)
			dev_warn(priv->device,
				 "serdes does not support SGMII: %pe\n",
				 ERR_PTR(ret));
	}

	__set_bit(PHY_INTERFACE_MODE_SGMII, spcs->pcs.supported_interfaces);

	if (readl(spcs->base + GMAC_AN_STATUS) & BMSR_ESTATEN) {
		__set_bit(PHY_INTERFACE_MODE_1000BASEX,
			  spcs->pcs.supported_interfaces);

		/* Only allow 2500Base-X if the SerDes has support. */
		ret = dwmac_serdes_validate(priv, PHY_INTERFACE_MODE_2500BASEX);
		if (ret == 0)
			__set_bit(PHY_INTERFACE_MODE_2500BASEX,
				  spcs->pcs.supported_interfaces);
	}

	priv->integrated_pcs = spcs;

	return 0;
}
