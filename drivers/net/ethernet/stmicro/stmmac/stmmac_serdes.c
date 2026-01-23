#include <linux/phy/phy.h>

#include "stmmac.h"
#include "stmmac_serdes.h"

static phy_interface_t dwmac_serdes_phy_modes[] = {
	PHY_INTERFACE_MODE_SGMII,
	PHY_INTERFACE_MODE_1000BASEX,
	PHY_INTERFACE_MODE_2500BASEX
};

int dwmac_serdes_validate(struct stmmac_priv *priv, phy_interface_t interface)
{
	return phy_validate(priv->plat->serdes, PHY_MODE_ETHERNET, interface,
			    NULL);
}

int dwmac_serdes_init(struct stmmac_priv *priv)
{
	size_t i;
	int ret;

	if (!priv->plat->serdes)
		return 0;

	/* Encourage good implementation of the SerDes PHY driver, so that
	 * we can discover which Ethernet modes the SerDes supports.
	 * Unfortunately, some implementations are noisy (bad), others
	 * require phy_set_speed() to select the correct speed first
	 * (which then reprograms the SerDes, negating the whole point of
	 * phy_validate().) Weed out these incompatible implementations.
	 */
	for (i = 0; i < ARRAY_SIZE(dwmac_serdes_phy_modes); i++) {
		ret = phy_validate(priv->plat->serdes, PHY_MODE_ETHERNET,
				   dwmac_serdes_phy_modes[i], NULL);
		if (ret == 0 || ret == -EOPNOTSUPP)
			break;
	}

	if (ret == -EOPNOTSUPP)
		dev_warn(priv->device,
			 "SerDes driver does not implement phy_validate()\n");
	if (ret) {
		/* The SerDes PHY failed validation, refuse to use it. */
		dev_warn(priv->device,
			 "SerDes driver fails to validate SGMII, 1000BASE-X nor 2500BASE-X\n");
		return -EINVAL;
	}

	ret = phy_init(priv->plat->serdes);
	if (ret)
		dev_err(priv->device, "failed to initialize SerDes: %pe\n",
			ERR_PTR(ret));

	return ret;
}

int dwmac_serdes_power_on(struct stmmac_priv *priv)
{
	int ret;

	ret = phy_power_on(priv->plat->serdes);
	if (ret)
		dev_err(priv->device, "failed to power on SerDes: %pe\n",
			ERR_PTR(ret));

	return ret;
}

int dwmac_serdes_init_mode(struct stmmac_priv *priv, phy_interface_t interface)
{
	struct phy *serdes = priv->plat->serdes;

	if (!serdes || phy_get_mode(serdes) == PHY_MODE_ETHERNET)
		return 0;

	return dwmac_serdes_set_mode(priv, interface);
}

int dwmac_serdes_set_mode(struct stmmac_priv *priv, phy_interface_t interface)
{
	struct phy *serdes = priv->plat->serdes;
	int ret;

	ret = phy_set_mode_ext(serdes, PHY_MODE_ETHERNET, interface);
	if (ret)
		dev_err(priv->device,
			"failed to set SerDes mode %s: %pe\n",
			phy_modes(interface), ERR_PTR(ret));

	return ret;
}

void dwmac_serdes_power_off(struct stmmac_priv *priv)
{
	int ret;

	ret = phy_power_off(priv->plat->serdes);
	if (ret)
		dev_err(priv->device, "failed to power off SerDes: %pe\n",
			ERR_PTR(ret));
}

void dwmac_serdes_exit(struct stmmac_priv *priv)
{
	int ret = phy_exit(priv->plat->serdes);

	if (ret)
		dev_err(priv->device, "failed to shutdown SerDes: %pe\n",
			ERR_PTR(ret));
}
