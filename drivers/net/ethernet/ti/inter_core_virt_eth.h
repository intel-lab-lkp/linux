/* SPDX-License-Identifier: GPL-2.0
 * Texas Instruments K3 Inter Core Virtual Ethernet Driver
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 */

#ifndef __INTER_CORE_VIRT_ETH_H__
#define __INTER_CORE_VIRT_ETH_H__

#include <linux/etherdevice.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rpmsg.h>
#include "icve_rpmsg_common.h"

enum icve_state {
	ICVE_STATE_PROBE,
	ICVE_STATE_OPEN,
	ICVE_STATE_CLOSE,
	ICVE_STATE_READY,
	ICVE_STATE_RUNNING,

};

struct icve_port {
	struct icve_shared_mem *tx_buffer; /* Write buffer for data to be consumed remote side */
	struct icve_shared_mem *rx_buffer; /* Read buffer for data to be consumed by this driver */
	struct timer_list rx_timer;
	struct icve_common *common;
	struct napi_struct rx_napi;
	u8 local_mac_addr[ETH_ALEN];
	struct net_device *ndev;
	u32 icve_tx_max_buffers;
	u32 icve_rx_max_buffers;
	u32 port_id;
};

struct icve_common {
	struct rpmsg_device *rpdev;
	spinlock_t send_msg_lock; /* Acquire this lock while sending RPMsg */
	spinlock_t recv_msg_lock; /* Acquire this lock while processing received RPMsg */
	struct message send_msg;
	struct message recv_msg;
	struct icve_port *port;
	struct device *dev;
	enum icve_state	state;
	struct mutex state_lock; /* Lock to be used while changing the interface state */
	struct delayed_work state_work;
	struct completion sync_msg;
};

struct icve_ndev_priv {
	struct icve_port *port;
};


#endif /* __INTER_CORE_VIRT_ETH_H__ */
