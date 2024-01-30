/* SPDX-License-Identifier: GPL-2.0 */
/* Texas Instruments K3 Inter Core Virtual Ethernet Driver
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 */

#ifndef __INTER_CORE_VIRT_ETH_H__
#define __INTER_CORE_VIRT_ETH_H__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/rpmsg.h>

enum icve_msg_type {
	ICVE_REQUEST_MSG = 0,
	ICVE_RESPONSE_MSG,
};

enum icve_rpmsg_type {
	/* Request types */
	ICVE_REQ_SHM_INFO = 0,

	/* Response types */
	ICVE_RESP_SHM_INFO,
};

struct icve_shm_info {
	void *tx_buffer;
	void *tx_buffer_base_addr;
	void *rx_buffer;
	void *rx_buffer_base_addr;
	u32 max_buffers;
} __packed;

struct request_message {
	u32 type; /* Request Type */
} __packed;

struct response_message {
	u32 type;
	union {
		struct icve_shm_info shm_info;
	};
} __packed;

struct notify_message {
	u32 type;
} __packed;

struct message_header {
	u32 src_id;
	u32 msg_type; /* Do not use enum type, as enum size is compiler dependent */
} __packed;

struct message {
	struct message_header msg_hdr;
	union {
		struct request_message req_msg;
		struct response_message resp_msg;
		struct notify_message notify_msg;
	};
} __packed;

struct shared_mem {
	u32 head;
	u32 tail;
	void *base_addr;
} __packed;

struct icve_port {
	struct shared_mem *tx_buffer; /* Write buffer for data to be consumed remote side */
	struct shared_mem *rx_buffer; /* Read buffer for data to be consumed by this driver */
	struct icve_common *common;
	u32 icve_max_buffers;
	u32 port_id; /* Unique ID for the port : TODO: Define range for use by Linux and non linux */

} __packed;

struct icve_common {
	struct rpmsg_device *rpdev;
	spinlock_t send_msg_lock;
	spinlock_t recv_msg_lock;
	struct message send_msg;
	struct message recv_msg;
	struct icve_port *port;
	struct device *dev;
} __packed;

#endif /* __INTER_CORE_VIRT_ETH_H__ */
