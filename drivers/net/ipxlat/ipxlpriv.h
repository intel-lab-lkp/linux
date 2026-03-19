/* SPDX-License-Identifier: GPL-2.0 */
/*  IPXLAT - Stateless IP/ICMP Translation (SIIT) virtual device driver
 *
 *  Copyright (C) 2024- Alberto Leiva Popper <ydahhrk@gmail.com>
 *  Copyright (C) 2026- Mandelbit SRL
 *  Copyright (C) 2026- Daniel Gröber <dxld@darkboxed.org>
 *
 *  Author:	Alberto Leiva Popper <ydahhrk@gmail.com>
 *		Antonio Quartulli <antonio@mandelbit.com>
 *		Daniel Gröber <dxld@darkboxed.org>
 *		Ralf Lici <ralf@mandelbit.com>
 */

#ifndef _NET_IPXLAT_IPXLPRIV_H_
#define _NET_IPXLAT_IPXLPRIV_H_

#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <net/gro_cells.h>

/**
 * struct ipv6_prefix - IPv6 prefix definition
 * @addr: prefix address (host bits may be non-zero)
 * @len: prefix length in bits
 */
struct ipv6_prefix {
	struct in6_addr addr;
	u8 len;
};

/**
 * struct ipxlat_priv - private state stored in netdev priv area
 * @dev: owning netdevice
 * @xlat_prefix6: RFC 6052 prefix used for stateless v4<->v6 mapping
 * @lowest_ipv6_mtu: LIM threshold used by 4->6 pre-fragment planning
 * @cfg_lock: serializes control-plane updates
 * @gro_cells: receive-side reinjection queue used by forward path
 *
 * Datapath reads config without taking @cfg_lock to keep per-packet overhead
 * low. Writers serialize updates under @cfg_lock. During reconfiguration,
 * readers may transiently observe mixed old/new values; this may cause a small
 * number of drops and is an accepted tradeoff for a lightweight datapath.
 */
struct ipxlat_priv {
	struct net_device *dev;
	struct ipv6_prefix xlat_prefix6;
	u32 lowest_ipv6_mtu;
	/* serializes control-plane updates */
	struct mutex cfg_lock;
	struct gro_cells gro_cells;
};

#endif /* _NET_IPXLAT_IPXLPRIV_H_ */
