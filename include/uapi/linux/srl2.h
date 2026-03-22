/* SPDX-License-Identifier: GPL-2.0-or-later WITH Linux-syscall-note */
/*
 *  SRv6 L2 tunnel device
 *
 *  Author:
 *  Andrea Mayer <andrea.mayer@uniroma2.it>
 */

#ifndef _UAPI_LINUX_SRL2_H
#define _UAPI_LINUX_SRL2_H

enum {
	IFLA_SRL2_UNSPEC,
	IFLA_SRL2_SRH,	/* binary: struct ipv6_sr_hdr + segments */
	__IFLA_SRL2_MAX,
};

#define IFLA_SRL2_MAX (__IFLA_SRL2_MAX - 1)

#endif
