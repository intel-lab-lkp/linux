/* SPDX-License-Identifier: GPL-2.0-or-later */

#define MXL862XX_MAX_PHY_PORT_NUM	8
#define MXL862XX_MAX_EXT_PORT_NUM	7
#define MXL862XX_MAX_PORT_NUM		(MXL862XX_MAX_PHY_PORT_NUM + \
					 MXL862XX_MAX_EXT_PORT_NUM)

#define MXL86252_PHY_PORT_NUM		5
#define MXL86282_PHY_PORT_NUM		8

#define MXL86252_EXT_PORT_NUM		2
#define MXL86282_EXT_PORT_NUM		2

struct mxl862xx_hw_info {
	u8 max_ports;
	u8 phy_ports;
	u8 ext_ports;
};

struct mxl862xx_priv {
	struct dsa_switch *ds;
	struct mii_bus *bus;
	struct device *dev;
	int sw_addr;
	const struct mxl862xx_hw_info *hw_info;
	u8 cpu_port;
};
