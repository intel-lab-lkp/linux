/* SPDX-License-Identifier: GPL-2.0-or-later */

#define MXL862XX_MAX_PHY_PORT_NUM	8
#define MXL862XX_MAX_EXT_PORT_NUM	7
#define MXL862XX_MAX_PORT_NUM		(MXL862XX_MAX_PHY_PORT_NUM + \
					 MXL862XX_MAX_EXT_PORT_NUM)

#define MXL86252_PHY_PORT_NUM		5
#define MXL86282_PHY_PORT_NUM		8

#define MXL862XX_EXT_PORT_NUM		2

#define MXL862XX_MAX_BRIDGES		17

struct mxl862xx_hw_info {
	u8 max_ports;
	u8 phy_ports;
	u8 ext_ports;
};

struct mxl862xx_priv {
	struct dsa_switch *ds;
	struct mdio_device *mdiodev;
	const struct mxl862xx_hw_info *hw_info;
};
