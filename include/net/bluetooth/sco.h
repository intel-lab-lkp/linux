/* SPDX-License-Identifier: GPL-2.0 */
/*
   BlueZ - Bluetooth protocol stack for Linux
   Copyright (C) 2000-2001 Qualcomm Incorporated

   Written 2000,2001 by Maxim Krasnyansky <maxk@qualcomm.com>
*/

#ifndef __SCO_H
#define __SCO_H

/* SCO defaults */
#define SCO_DEFAULT_MTU		500

/* SCO socket address */
struct sockaddr_sco {
	sa_family_t	sco_family;
	bdaddr_t	sco_bdaddr;
};

/* SCO socket options */
#define SCO_OPTIONS	0x01
struct sco_options {
	__u16 mtu;
};

#define SCO_CONNINFO	0x02
struct sco_conninfo {
	__u16 hci_handle;
	__u8  dev_class[3];
};

#endif /* __SCO_H */
