/* SPDX-License-Identifier: GPL-2.0+ */

#ifndef _RTL83XX_H
#define _RTL83XX_H

struct realtek_interface_info {
	int (*reg_read)(void *ctx, u32 reg, u32 *val);
	int (*reg_write)(void *ctx, u32 reg, u32 val);
};

void rtl83xx_lock(void *ctx);
void rtl83xx_unlock(void *ctx);
int rtl83xx_setup_user_mdio(struct dsa_switch *ds);
struct realtek_priv *
rtl83xx_probe(struct device *dev,
	      const struct realtek_interface_info *interface_info);
int rtl83xx_register_switch(struct realtek_priv *priv);
void rtl83xx_unregister_switch(struct realtek_priv *priv);
void rtl83xx_shutdown(struct realtek_priv *priv);
void rtl83xx_remove(struct realtek_priv *priv);
void rtl83xx_reset_assert(struct realtek_priv *priv);
void rtl83xx_reset_deassert(struct realtek_priv *priv);

int rtl83xx_port_bridge_join(struct dsa_switch *ds, int port,
			     struct dsa_bridge bridge,
			     bool *tx_forward_offload,
			     struct netlink_ext_ack *extack);
void rtl83xx_port_bridge_leave(struct dsa_switch *ds, int port,
			       struct dsa_bridge bridge);

#endif /* _RTL83XX_H */
