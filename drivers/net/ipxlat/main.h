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

#ifndef _NET_IPXLAT_MAIN_H_
#define _NET_IPXLAT_MAIN_H_

#include <linux/netdevice.h>

/**
 * ipxlat_dev_is_valid - tell whether a netdev is an ipxlat interface
 * @dev: netdevice to inspect
 *
 * Return: true if @dev was created with ipxlat link ops.
 */
bool ipxlat_dev_is_valid(const struct net_device *dev);

#endif /* _NET_IPXLAT_MAIN_H_ */
