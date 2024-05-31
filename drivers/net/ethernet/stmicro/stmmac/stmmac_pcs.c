#include "common.h"
#include "stmmac_pcs.h"

int dwmac_pcs_config(struct mac_device_info *hw, unsigned int neg_mode,
		     phy_interface_t interface,
		     const unsigned long *advertising,
		     unsigned int reg_base)
{
	u32 val;

	val = readl(hw->pcsr + GMAC_AN_CTRL(reg_base));

	val |= GMAC_AN_CTRL_ANE | GMAC_AN_CTRL_RAN;

	if (hw->ps)
		val |= GMAC_AN_CTRL_SGMRAL;

	writel(val, hw->pcsr + GMAC_AN_CTRL(reg_base));

	return 0;
}

void dwmac_pcs_get_state(struct mac_device_info *hw,
			 struct phylink_link_state *state,
			 unsigned int reg_base)
{
	u32 val;

	val = readl(hw->pcsr + GMAC_ANE_LPA(reg_base));

	linkmode_set_bit(ETHTOOL_LINK_MODE_Autoneg_BIT,
			 state->lp_advertising);

	if (val & GMAC_ANE_FD) {
		linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Full_BIT,
				 state->lp_advertising);
		linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Full_BIT,
				 state->lp_advertising);
		linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Full_BIT,
				 state->lp_advertising);
	}

	if (val & GMAC_ANE_HD) {
		linkmode_set_bit(ETHTOOL_LINK_MODE_1000baseT_Half_BIT,
				 state->lp_advertising);
		linkmode_set_bit(ETHTOOL_LINK_MODE_100baseT_Half_BIT,
				 state->lp_advertising);
		linkmode_set_bit(ETHTOOL_LINK_MODE_10baseT_Half_BIT,
				 state->lp_advertising);
	}

	linkmode_mod_bit(ETHTOOL_LINK_MODE_Pause_BIT,
			 state->lp_advertising,
			 FIELD_GET(GMAC_ANE_PSE, val) & STMMAC_PCS_PAUSE);
	linkmode_mod_bit(ETHTOOL_LINK_MODE_Asym_Pause_BIT,
			 state->lp_advertising,
			 FIELD_GET(GMAC_ANE_PSE, val) & STMMAC_PCS_ASYM_PAUSE);
}
