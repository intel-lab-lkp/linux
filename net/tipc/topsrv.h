/* SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause */
/*
 * net/tipc/server.h: Include file for TIPC server code
 *
 * Copyright (c) 2012-2013, Wind River Systems
 * Copyright (c) 2017, Ericsson AB
 * All rights reserved.
 */

#ifndef _TIPC_SERVER_H
#define _TIPC_SERVER_H

#include "core.h"

#define TIPC_SERVER_NAME_LEN	32
#define TIPC_SUB_CLUSTER_SCOPE  0x20
#define TIPC_SUB_NODE_SCOPE     0x40
#define TIPC_SUB_NO_STATUS      0x80

void tipc_topsrv_queue_evt(struct net *net, int conid,
			   u32 event, struct tipc_event *evt);

bool tipc_topsrv_kern_subscr(struct net *net, u32 port, u32 type, u32 lower,
			     u32 upper, u32 filter, int *conid);
void tipc_topsrv_kern_unsubscr(struct net *net, int conid);

#endif
