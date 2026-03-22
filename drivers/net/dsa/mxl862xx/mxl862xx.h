/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef __MXL862XX_H
#define __MXL862XX_H

#include <linux/mdio.h>
#include <net/dsa.h>

#define MXL862XX_MAX_PORTS		17
#define MXL862XX_DEFAULT_BRIDGE		0
#define MXL862XX_MAX_BRIDGES		48
#define MXL862XX_MAX_BRIDGE_PORTS	128

/* Number of __le16 words in a firmware portmap (128-bit bitmap). */
#define MXL862XX_FW_PORTMAP_WORDS	(MXL862XX_MAX_BRIDGE_PORTS / 16)

/**
 * mxl862xx_fw_portmap_from_bitmap - convert a kernel bitmap to a firmware
 *                                   portmap (__le16[8])
 * @dst: firmware portmap array (MXL862XX_FW_PORTMAP_WORDS entries)
 * @src: kernel bitmap of at least MXL862XX_MAX_BRIDGE_PORTS bits
 */
static inline void
mxl862xx_fw_portmap_from_bitmap(__le16 *dst, const unsigned long *src)
{
	int i;

	for (i = 0; i < MXL862XX_FW_PORTMAP_WORDS; i++)
		dst[i] = cpu_to_le16(bitmap_read(src, i * 16, 16));
}

/**
 * mxl862xx_fw_portmap_to_bitmap - convert a firmware portmap (__le16[8]) to
 *                                 a kernel bitmap
 * @dst: kernel bitmap of at least MXL862XX_MAX_BRIDGE_PORTS bits
 * @src: firmware portmap array (MXL862XX_FW_PORTMAP_WORDS entries)
 */
static inline void
mxl862xx_fw_portmap_to_bitmap(unsigned long *dst, const __le16 *src)
{
	int i;

	bitmap_zero(dst, MXL862XX_MAX_BRIDGE_PORTS);
	for (i = 0; i < MXL862XX_FW_PORTMAP_WORDS; i++)
		bitmap_write(dst, le16_to_cpu(src[i]), i * 16, 16);
}

/**
 * mxl862xx_fw_portmap_set_bit - set a single port bit in a firmware portmap
 * @map: firmware portmap array (MXL862XX_FW_PORTMAP_WORDS entries)
 * @port: port index (0..MXL862XX_MAX_BRIDGE_PORTS-1)
 */
static inline void mxl862xx_fw_portmap_set_bit(__le16 *map, int port)
{
	map[port / 16] |= cpu_to_le16(BIT(port % 16));
}

/**
 * mxl862xx_fw_portmap_clear_bit - clear a single port bit in a firmware portmap
 * @map: firmware portmap array (MXL862XX_FW_PORTMAP_WORDS entries)
 * @port: port index (0..MXL862XX_MAX_BRIDGE_PORTS-1)
 */
static inline void mxl862xx_fw_portmap_clear_bit(__le16 *map, int port)
{
	map[port / 16] &= ~cpu_to_le16(BIT(port % 16));
}

/**
 * mxl862xx_fw_portmap_is_empty - check whether a firmware portmap has no
 *                                bits set
 * @map: firmware portmap array (MXL862XX_FW_PORTMAP_WORDS entries)
 *
 * Return: true if every word in @map is zero.
 */
static inline bool mxl862xx_fw_portmap_is_empty(const __le16 *map)
{
	int i;

	for (i = 0; i < MXL862XX_FW_PORTMAP_WORDS; i++)
		if (map[i])
			return false;
	return true;
}

/**
 * struct mxl862xx_port - per-port state tracked by the driver
 * @fid:         firmware FID for the permanent single-port bridge; kept alive
 *               for the lifetime of the port so traffic is never forwarded
 *               while the port is unbridged
 * @portmap:     bitmap of switch port indices that share the current bridge
 *               with this port
 * @flood_block: bitmask of firmware meter indices that are currently
 *               rate-limiting flood traffic on this port (zero-rate meters
 *               used to block flooding)
 * @learning:    true when address learning is enabled on this port
 */
struct mxl862xx_port {
	u16 fid;
	DECLARE_BITMAP(portmap, MXL862XX_MAX_BRIDGE_PORTS);
	unsigned long flood_block;
	bool learning;
};

/**
 * struct mxl862xx_priv - driver private data for an MxL862xx switch
 * @ds:            pointer to the DSA switch instance
 * @mdiodev:       MDIO device used to communicate with the switch firmware
 * @drop_meter:    index of the single shared zero-rate firmware meter used
 *                 to unconditionally drop traffic (used to block flooding)
 * @ports:         per-port state, indexed by switch port number
 */
struct mxl862xx_priv {
	struct dsa_switch *ds;
	struct mdio_device *mdiodev;
	u16 drop_meter;
	struct mxl862xx_port ports[MXL862XX_MAX_PORTS];
};

#endif /* __MXL862XX_H */
