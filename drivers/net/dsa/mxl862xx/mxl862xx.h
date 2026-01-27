/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __MXL862XX_H
#define __MXL862XX_H

#include <linux/mdio.h>
#include <net/dsa.h>

#define MXL862XX_MAX_PHY_PORT_NUM	8
#define MXL862XX_MAX_EXT_PORT_NUM	7
#define MXL862XX_MAX_PORT_NUM		(MXL862XX_MAX_PHY_PORT_NUM + \
					 MXL862XX_MAX_EXT_PORT_NUM)

struct mxl862xx_priv {
	struct dsa_switch *ds;
	struct mdio_device *mdiodev;
};

#endif /* __MXL862XX_H */
