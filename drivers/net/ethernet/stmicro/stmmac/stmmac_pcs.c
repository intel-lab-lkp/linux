// SPDX-License-Identifier: GPL-2.0-only
#include "stmmac.h"
#include "stmmac_pcs.h"

static unsigned int dwmac_integrated_pcs_inband_caps(struct phylink_pcs *pcs,
						     phy_interface_t interface)
{
	if (interface != PHY_INTERFACE_MODE_SGMII)
		return 0;

	return LINK_INBAND_ENABLE | LINK_INBAND_DISABLE;
}

static int dwmac_integrated_pcs_enable(struct phylink_pcs *pcs)
{
	struct stmmac_pcs *spcs = phylink_pcs_to_stmmac_pcs(pcs);

	stmmac_mac_irq_modify(spcs->priv, 0, spcs->int_mask);

	return 0;
}

static void dwmac_integrated_pcs_disable(struct phylink_pcs *pcs)
{
	struct stmmac_pcs *spcs = phylink_pcs_to_stmmac_pcs(pcs);

	stmmac_mac_irq_modify(spcs->priv, spcs->int_mask, 0);
}

static void dwmac_integrated_pcs_get_state(struct phylink_pcs *pcs,
					   unsigned int neg_mode,
					   struct phylink_link_state *state)
{
	state->link = false;
}

static int dwmac_integrated_pcs_config(struct phylink_pcs *pcs,
				       unsigned int neg_mode,
				       phy_interface_t interface,
				       const unsigned long *advertising,
				       bool permit_pause_to_mac)
{
	struct stmmac_pcs *spcs = phylink_pcs_to_stmmac_pcs(pcs);
	void __iomem *an_control = spcs->base + GMAC_AN_CTRL(0);
	u32 ctrl;

	ctrl = readl(an_control) & ~(GMAC_AN_CTRL_ANE | GMAC_AN_CTRL_SGMRAL);
	if (spcs->priv->hw->reverse_sgmii_enable)
		ctrl |= GMAC_AN_CTRL_SGMRAL | GMAC_AN_CTRL_ANE;
	else if (neg_mode == PHYLINK_PCS_NEG_INBAND_ENABLED)
		ctrl |= GMAC_AN_CTRL_ANE;
	else
		ctrl |= GMAC_AN_CTRL_SGMRAL;
	writel(ctrl, an_control);

	return 0;
}

static const struct phylink_pcs_ops dwmac_integrated_pcs_ops = {
	.pcs_inband_caps = dwmac_integrated_pcs_inband_caps,
	.pcs_enable = dwmac_integrated_pcs_enable,
	.pcs_disable = dwmac_integrated_pcs_disable,
	.pcs_get_state = dwmac_integrated_pcs_get_state,
	.pcs_config = dwmac_integrated_pcs_config,
};

int stmmac_integrated_pcs_init(struct stmmac_priv *priv, unsigned int offset,
			       u32 int_mask)
{
	struct stmmac_pcs *spcs;

	spcs = devm_kzalloc(priv->device, sizeof(*spcs), GFP_KERNEL);
	if (!spcs)
		return -ENOMEM;

	spcs->priv = priv;
	spcs->base = priv->ioaddr + offset;
	spcs->int_mask = int_mask;
	spcs->pcs.ops = &dwmac_integrated_pcs_ops;

	__set_bit(PHY_INTERFACE_MODE_SGMII, spcs->pcs.supported_interfaces);

	priv->integrated_pcs = spcs;

	return 0;
}
