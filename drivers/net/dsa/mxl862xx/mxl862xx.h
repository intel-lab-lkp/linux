/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __MXL862XX_H
#define __MXL862XX_H

#include <linux/mdio.h>
#include <net/dsa.h>

#define MXL862XX_MAX_PORTS		17
#define MXL862XX_DEFAULT_BRIDGE		0
#define MXL862XX_MAX_BRIDGES		48
#define MXL862XX_MAX_BRIDGE_PORTS	128

/**
 * struct mxl862xx_bridge - per-bridge state tracked by the driver
 * @dsa_bridge_num: DSA framework bridge number, as returned by
 *                  dsa_switch_bridge_join()
 * @bridge_id:      firmware FID assigned to this bridge
 * @portmap:        bitmap of ports belonging to this bridge
 * @list:           entry in &mxl862xx_priv.bridges
 */
struct mxl862xx_bridge {
	unsigned int dsa_bridge_num;
	u16 bridge_id;
	DECLARE_BITMAP(portmap, MXL862XX_MAX_BRIDGE_PORTS);
	struct list_head list;
};

/**
 * struct mxl862xx_port - per-port state tracked by the driver
 * @fid:         firmware FID for the permanent single-port bridge; kept alive
 *               for the lifetime of the port so traffic is never forwarded
 *               while the port is unbridged
 * @bridge:      bridge this port currently belongs to, or %NULL if unbridged
 * @portmap:     bitmap of switch port indices that share the current bridge
 *               with this port (mirrors @bridge->portmap for convenience)
 * @flood_block: bitmask of firmware meter indices that are currently
 *               rate-limiting flood traffic on this port (zero-rate meters
 *               used to block flooding)
 * @learning:    true when address learning is enabled on this port
 */
struct mxl862xx_port {
	u16 fid;
	struct mxl862xx_bridge *bridge;
	DECLARE_BITMAP(portmap, MXL862XX_MAX_BRIDGE_PORTS);
	unsigned long flood_block;
	bool learning;
};

/**
 * struct mxl862xx_priv - driver private data for an MxL862xx switch
 * @ds:          pointer to the DSA switch instance
 * @mdiodev:     MDIO device used to communicate with the switch firmware
 * @drop_meter:  index of the single shared zero-rate firmware meter used
 *               to unconditionally drop traffic (used to block flooding)
 * @ports:       per-port state, indexed by switch port number
 * @bridges:     list of &struct mxl862xx_bridge instances currently offloaded
 *               to the hardware
 */
struct mxl862xx_priv {
	struct dsa_switch *ds;
	struct mdio_device *mdiodev;
	u16 drop_meter;
	struct mxl862xx_port ports[MXL862XX_MAX_PORTS];
	struct list_head bridges;
};

#endif /* __MXL862XX_H */
