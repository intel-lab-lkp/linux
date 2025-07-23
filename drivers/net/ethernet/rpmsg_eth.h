/* SPDX-License-Identifier: GPL-2.0
 * Texas Instruments K3 Inter Core Virtual Ethernet Driver common header
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
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
#define RPMSG_ETH_MIN_PACKET_SIZE ETH_ZLEN
#define RPMSG_ETH_PACKET_BUFFER_SIZE   1540
#define MAX_MTU   (RPMSG_ETH_PACKET_BUFFER_SIZE - (ETH_HLEN + ETH_FCS_LEN + VLAN_HLEN))

#define RPMSG_ETH_MAX_TX_QUEUES 1
#define RPMSG_ETH_MAX_RX_QUEUES 1
#define PKT_LEN_SIZE_TYPE sizeof(u32)
#define MAGIC_NUM_SIZE_TYPE sizeof(u32)

/* 4 bytes to hold packet length and RPMSG_ETH_PACKET_BUFFER_SIZE to hold packet */
#define RPMSG_ETH_BUFFER_SIZE \
	(RPMSG_ETH_PACKET_BUFFER_SIZE + PKT_LEN_SIZE_TYPE + MAGIC_NUM_SIZE_TYPE)

#define RX_POLL_TIMEOUT_JIFFIES usecs_to_jiffies(1000)
#define RX_POLL_JIFFIES (jiffies + RX_POLL_TIMEOUT_JIFFIES)
#define STATE_MACHINE_TIME_JIFFIES msecs_to_jiffies(100)
#define RPMSG_ETH_REQ_TIMEOUT_JIFFIES msecs_to_jiffies(100)

#define rpmsg_eth_ndev_to_priv(ndev) ((struct rpmsg_eth_ndev_priv *)netdev_priv(ndev))
#define rpmsg_eth_ndev_to_port(ndev) (rpmsg_eth_ndev_to_priv(ndev)->port)
#define rpmsg_eth_ndev_to_common(ndev) (rpmsg_eth_ndev_to_port(ndev)->common)

enum rpmsg_eth_msg_type {
	RPMSG_ETH_REQUEST_MSG = 0,
	RPMSG_ETH_RESPONSE_MSG,
	RPMSG_ETH_NOTIFY_MSG,
};

enum rpmsg_eth_rpmsg_type {
	/* Request types */
	RPMSG_ETH_REQ_SHM_INFO = 0,
	RPMSG_ETH_REQ_SET_MAC_ADDR,

	/* Response types */
	RPMSG_ETH_RESP_SHM_INFO,
	RPMSG_ETH_RESP_SET_MAC_ADDR,

	/* Notification types */
	RPMSG_ETH_NOTIFY_PORT_UP,
	RPMSG_ETH_NOTIFY_PORT_DOWN,
	RPMSG_ETH_NOTIFY_PORT_READY,
	RPMSG_ETH_NOTIFY_REMOTE_READY,
};

/**
 * struct rpmsg_eth_shm - Shared memory layout for RPMsg Ethernet
 * @num_pkt_bufs: Number of packet buffers available in the shared memory
 * @buff_slot_size: Size of each buffer slot in bytes
 * @base_addr: Base address of the shared memory region
 * @tx_offset: Offset for the transmit buffer region within the shared memory
 * @rx_offset: Offset for the receive buffer region within the shared memory
 *
 * This structure defines the layout of the shared memory used for
 * communication between the host and the remote processor in an RPMsg
 * Ethernet driver. It specifies the configuration and memory offsets
 * required for transmitting and receiving Ethernet packets.
 */
struct rpmsg_eth_shm {
	u32 num_pkt_bufs;
	u32 buff_slot_size;
	u32 base_addr;
	u32 tx_offset;
	u32 rx_offset;
} __packed;

/**
 * struct rpmsg_eth_mac_addr - MAC address information for RPMSG Ethernet
 * @addr: MAC address
 */
struct rpmsg_eth_mac_addr {
	char addr[ETH_ALEN];
} __packed;

/**
 * struct request_message - request message structure for RPMSG Ethernet
 * @type: Request Type
 * @id: Request ID
 * @mac_addr: MAC address (if request type is MAC address related)
 */
struct request_message {
	u32 type;
	u32 id;
	union {
		struct rpmsg_eth_mac_addr mac_addr;
	};
} __packed;

/**
 * struct response_message - response message structure for RPMSG Ethernet
 * @type: Response Type
 * @id: Response ID
 * @shm_info: rpmsg shared memory info
 */
struct response_message {
	u32 type;
	u32 id;
	union {
		struct rpmsg_eth_shm shm_info;
	};
} __packed;

/**
 * struct notify_message - notification message structure for RPMSG Ethernet
 * @type: Notify Type
 * @id: Notify ID
 */
struct notify_message {
	u32 type;
	u32 id;
} __packed;

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
 * @req_msg: Request message structure contains the request type and ID
 * @resp_msg: Response message structure contains the response type and ID
 * @notify_msg: Notification message structure contains the notify type and ID
 *
 * This structure is used to send and receive messages between the RPMSG
 * Ethernet ports.
 */
struct message {
	struct message_header msg_hdr;
	union {
		struct request_message req_msg;
		struct response_message resp_msg;
		struct notify_message notify_msg;
	};
} __packed;

/*      Shared Memory Layout
 *
 *	---------------------------	*****************
 *	|        MAGIC_NUM        |	 rpmsg_eth_shm_head
 *	|          HEAD           |
 *	---------------------------	*****************
 *	|        MAGIC_NUM        |
 *	|        PKT_1_LEN        |
 *	|          PKT_1          |
 *	---------------------------
 *	|        MAGIC_NUM        |
 *	|        PKT_2_LEN        |	 rpmsg_eth_shm_buf
 *	|          PKT_2          |
 *	---------------------------
 *	|           .             |
 *	|           .             |
 *	---------------------------
 *	|        MAGIC_NUM        |
 *	|        PKT_N_LEN        |
 *	|          PKT_N          |
 *	---------------------------	****************
 *	|        MAGIC_NUM        |      rpmsg_eth_shm_tail
 *	|          TAIL           |
 *	---------------------------	****************
 */

struct rpmsg_eth_shm_index {
	u32 magic_num;
	u32 index;
}  __packed;

/**
 * struct rpmsg_eth_shm_buf - shared memory buffer structure for RPMSG Ethernet
 * @base_addr: Base address of the buffer
 * @magic_num: Magic number for buffer validation
 */
struct rpmsg_eth_shm_buf {
	void __iomem *base_addr;
	u32 magic_num;
} __packed;

/**
 * struct rpmsg_eth_shared_mem - shared memory structure for RPMSG Ethernet
 * @head: Head of the shared memory
 * @buf: Buffer of the shared memory
 * @tail: Tail of the shared memory
 */
struct rpmsg_eth_shared_mem {
	struct rpmsg_eth_shm_index *head;
	struct rpmsg_eth_shm_buf *buf;
	struct rpmsg_eth_shm_index *tail;
} __packed;

enum rpmsg_eth_state {
	RPMSG_ETH_STATE_PROBE,
	RPMSG_ETH_STATE_OPEN,
	RPMSG_ETH_STATE_CLOSE,
	RPMSG_ETH_STATE_READY,
	RPMSG_ETH_STATE_RUNNING,

};

/**
 * struct rpmsg_eth_common - common structure for RPMSG Ethernet
 * @rpdev: RPMSG device
 * @send_msg: Send message
 * @recv_msg: Receive message
 * @port: Ethernet port
 * @dev: Device
 * @state: Interface state
 * @state_work: Delayed work for state machine
 * @sync_msg: Completion for synchronous message
 */
struct rpmsg_eth_common {
	struct rpmsg_device *rpdev;
	/** @send_msg_lock: Lock for sending RPMSG */
	spinlock_t send_msg_lock;
	/** @recv_msg_lock: Lock for receiving RPMSG */
	spinlock_t recv_msg_lock;
	struct message send_msg;
	struct message recv_msg;
	struct rpmsg_eth_port *port;
	struct device *dev;
	enum rpmsg_eth_state state;
	/** @state_lock: Lock for changing interface state */
	struct mutex state_lock;
	struct delayed_work state_work;
	struct completion sync_msg;
};

/**
 * struct rpmsg_eth_ndev_priv - private structure for RPMSG Ethernet net device
 * @port: Ethernet port
 * @dev: Device
 */
struct rpmsg_eth_ndev_priv {
	struct rpmsg_eth_port *port;
	struct device *dev;
};

/**
 * struct rpmsg_eth_port - Ethernet port structure for RPMSG Ethernet
 * @common: Pointer to the common RPMSG Ethernet structure
 * @buf_start_addr: Start address of the shared memory buffer for this port
 * @buf_size: Size (in bytes) of the shared memory buffer for this port
 * @tx_buffer: Write buffer for data to be consumed by remote side
 * @rx_buffer: Read buffer for data to be consumed by this driver
 * @rx_timer: Timer for rx polling
 * @rx_napi: NAPI structure for rx polling
 * @local_mac_addr: Local MAC address
 * @ndev: Network device
 * @rpmsg_eth_tx_max_buffers: Maximum number of tx buffers
 * @rpmsg_eth_rx_max_buffers: Maximum number of rx buffers
 * @port_id: Port ID
 */
struct rpmsg_eth_port {
	struct rpmsg_eth_common *common;
	u32 buf_start_addr;
	u32 buf_size;
	struct rpmsg_eth_shared_mem *tx_buffer;
	struct rpmsg_eth_shared_mem *rx_buffer;
	struct timer_list rx_timer;
	struct napi_struct rx_napi;
	u8 local_mac_addr[ETH_ALEN];
	struct net_device *ndev;
	u32 rpmsg_eth_tx_max_buffers;
	u32 rpmsg_eth_rx_max_buffers;
	u32 port_id;
};

#endif /* __RPMSG_ETH_H__ */
