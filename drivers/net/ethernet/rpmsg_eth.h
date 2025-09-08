/* SPDX-License-Identifier: GPL-2.0
 * RPMsg Based Virtual Ethernet Driver common header
 *
 * Copyright (C) 2025 Texas Instruments Incorporated - https://www.ti.com/
 */

#ifndef __RPMSG_ETH_H__
#define __RPMSG_ETH_H__

#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/rpmsg.h>

#define RPMSG_ETH_SHM_MAGIC_NUM 0xABCDABCD

enum rpmsg_eth_msg_type {
	RPMSG_ETH_REQUEST_MSG = 0,
	RPMSG_ETH_RESPONSE_MSG,
	RPMSG_ETH_NOTIFY_MSG,
};

/**
 * struct message_header - message header structure for RPMSG Ethernet
 * @src_id: Source endpoint ID
 * @msg_type: Message type
 */
struct message_header {
	u32 src_id;
	u32 msg_type;
} __packed;

/**
 * struct message - RPMSG Ethernet message structure
 *
 * @msg_hdr: Message header contains source and destination endpoint and
 *          the type of message
 *
 * This structure is used to send and receive messages between the RPMSG
 * Ethernet ports.
 */
struct message {
	struct message_header msg_hdr;
} __packed;

/**
 * struct prueth_pdata - RPMSG ETH device data
 * @shm_region_index: Shared memory region index
 */
struct rpmsg_eth_data {
	u8 shm_region_index;
};

/**
 * struct rpmsg_eth_common - common structure for RPMSG Ethernet
 * @rpdev: RPMSG device
 * @port: Ethernet port
 * @dev: Device
 * @data: Vendor specific data
 */
struct rpmsg_eth_common {
	struct rpmsg_device *rpdev;
	struct rpmsg_eth_port *port;
	struct device *dev;
	struct rpmsg_eth_data data;
};

/**
 * struct rpmsg_eth_port - Ethernet port structure for RPMSG Ethernet
 * @common: Pointer to the common RPMSG Ethernet structure
 * @shm: Shared memory region mapping
 * @buf_size: Size (in bytes) of the shared memory buffer for this port
 */
struct rpmsg_eth_port {
	struct rpmsg_eth_common *common;
	void __iomem *shm;
	phys_addr_t buf_size;
};

#endif /* __RPMSG_ETH_H__ */
