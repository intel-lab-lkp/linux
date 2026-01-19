#ifndef STMMAC_SERDES_H
#define STMMAC_SERDES_H

#include <linux/phy.h>

struct stmmac_priv;

int dwmac_serdes_validate(struct stmmac_priv *priv, phy_interface_t interface);
int dwmac_serdes_init(struct stmmac_priv *priv);
int dwmac_serdes_power_on(struct stmmac_priv *priv);
int dwmac_serdes_init_mode(struct stmmac_priv *priv, phy_interface_t interface);
int dwmac_serdes_set_mode(struct stmmac_priv *priv, phy_interface_t interface);
void dwmac_serdes_power_off(struct stmmac_priv *priv);
void dwmac_serdes_exit(struct stmmac_priv *priv);

#endif
