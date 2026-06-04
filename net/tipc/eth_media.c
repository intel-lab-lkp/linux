// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/*
 * net/tipc/eth_media.c: Ethernet bearer support for TIPC
 *
 * Copyright (c) 2001-2007, 2013-2014, Ericsson AB
 * Copyright (c) 2005-2008, 2011-2013, Wind River Systems
 * All rights reserved.
 */

#include "core.h"
#include "bearer.h"

/* Convert Ethernet address (media address format) to string */
static int tipc_eth_addr2str(struct tipc_media_addr *addr,
			     char *strbuf, int bufsz)
{
	if (bufsz < 18)	/* 18 = strlen("aa:bb:cc:dd:ee:ff\0") */
		return 1;

	sprintf(strbuf, "%pM", addr->value);
	return 0;
}

/* Convert from media address format to discovery message addr format */
static int tipc_eth_addr2msg(char *msg, struct tipc_media_addr *addr)
{
	memset(msg, 0, TIPC_MEDIA_INFO_SIZE);
	msg[TIPC_MEDIA_TYPE_OFFSET] = TIPC_MEDIA_TYPE_ETH;
	memcpy(msg + TIPC_MEDIA_ADDR_OFFSET, addr->value, ETH_ALEN);
	return 0;
}

/* Convert raw mac address format to media addr format */
static int tipc_eth_raw2addr(struct tipc_bearer *b,
			     struct tipc_media_addr *addr,
			     const char *msg)
{
	memset(addr, 0, sizeof(*addr));
	ether_addr_copy(addr->value, msg);
	addr->media_id = TIPC_MEDIA_TYPE_ETH;
	addr->broadcast = is_broadcast_ether_addr(addr->value);
	return 0;
}

/* Convert discovery msg addr format to Ethernet media addr format */
static int tipc_eth_msg2addr(struct tipc_bearer *b,
			     struct tipc_media_addr *addr,
			     char *msg)
{
	/* Skip past preamble: */
	msg += TIPC_MEDIA_ADDR_OFFSET;
	return tipc_eth_raw2addr(b, addr, msg);
}

/* Ethernet media registration info */
struct tipc_media eth_media_info = {
	.send_msg	= tipc_l2_send_msg,
	.enable_media	= tipc_enable_l2_media,
	.disable_media	= tipc_disable_l2_media,
	.addr2str	= tipc_eth_addr2str,
	.addr2msg	= tipc_eth_addr2msg,
	.msg2addr	= tipc_eth_msg2addr,
	.raw2addr	= tipc_eth_raw2addr,
	.priority	= TIPC_DEF_LINK_PRI,
	.tolerance	= TIPC_DEF_LINK_TOL,
	.min_win	= TIPC_DEF_LINK_WIN,
	.max_win	= TIPC_MAX_LINK_WIN,
	.type_id	= TIPC_MEDIA_TYPE_ETH,
	.hwaddr_len	= ETH_ALEN,
	.name		= "eth"
};
