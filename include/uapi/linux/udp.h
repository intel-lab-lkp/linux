/* SPDX-License-Identifier: GPL-2.0+ WITH Linux-syscall-note */
/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Definitions for the UDP protocol.
 *
 * Version:	@(#)udp.h	1.0.2	04/28/93
 *
 * Author:	Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 */
#ifndef _UAPI_LINUX_UDP_H
#define _UAPI_LINUX_UDP_H

#include <linux/types.h>

struct udphdr {
	__be16	source;
	__be16	dest;
	__be16	len;
	__sum16	check;
};

/* UDP socket options */
#define UDP_CORK	1	/* Never send partially complete segments */
#define UDP_ENCAP	100	/* Set the socket to accept encapsulated packets */
#define UDP_NO_CHECK6_TX 101	/* Disable sending checksum for UDP6X */
#define UDP_NO_CHECK6_RX 102	/* Disable accpeting checksum for UDP6 */
#define UDP_SEGMENT	103	/* Set GSO segmentation size */
#define UDP_GRO		104	/* This socket can receive UDP GRO packets */

/* UDP encapsulation types */
#define UDP_ENCAP_NONE		0
#define UDP_ENCAP_ESPINUDP_NON_IKE	1 /* unused  draft-ietf-ipsec-nat-t-ike-00/01 */
#define UDP_ENCAP_ESPINUDP	2 /* draft-ietf-ipsec-udp-encaps-06 */
#define UDP_ENCAP_L2TPINUDP	3 /* rfc2661 */
#define UDP_ENCAP_GTP0		4 /* GSM TS 09.60 */
#define UDP_ENCAP_GTP1U		5 /* 3GPP TS 29.060 */
#define UDP_ENCAP_RXRPC		6
#define TCP_ENCAP_ESPINTCP	7 /* Yikes, this is really xfrm encap types. */
#define UDP_ENCAP_TIPC		8
#define UDP_ENCAP_FOU		9
#define UDP_ENCAP_GUE		10
#define UDP_ENCAP_SCTP		11
#define UDP_ENCAP_RXE		12
#define UDP_ENCAP_PFCP		13
#define UDP_ENCAP_WIREGUARD	14
#define UDP_ENCAP_BAREUDP	15
#define UDP_ENCAP_VXLAN		16
#define UDP_ENCAP_VXLAN_GPE	17
#define UDP_ENCAP_GENEVE	18
#define UDP_ENCAP_AMT		19

#endif /* _UAPI_LINUX_UDP_H */
