/* SPDX-License-Identifier: GPL-2.0
 * Texas Instruments K3 Inter Core Virtual Ethernet Driver common header
 *
 * Copyright (C) 2024 Texas Instruments Incorporated - https://www.ti.com/
 */

#ifndef __ICVE_RPMSG_COMMON_H__
#define __ICVE_RPMSG_COMMON_H__

#include <linux/if_ether.h>

enum icve_msg_type {
	ICVE_REQUEST_MSG = 0,
	ICVE_RESPONSE_MSG,
	ICVE_NOTIFY_MSG,
};

enum icve_rpmsg_type {
	/* Request types */
	ICVE_REQ_SHM_INFO = 0,
	ICVE_REQ_SET_MAC_ADDR,
	ICVE_REQ_ADD_MC_ADDR,
	ICVE_REQ_DEL_MC_ADDR,

	/* Response types */
	ICVE_RESP_SHM_INFO,
	ICVE_RESP_SET_MAC_ADDR,
	ICVE_RESP_ADD_MC_ADDR,
	ICVE_RESP_DEL_MC_ADDR,

	/* Notification types */
	ICVE_NOTIFY_PORT_UP,
	ICVE_NOTIFY_PORT_DOWN,
	ICVE_NOTIFY_PORT_READY,
	ICVE_NOTIFY_REMOTE_READY,
};

struct icve_shm_info {
	/* Total shared memory size */
	u32 total_shm_size;
	/* Total number of buffers */
	u32 num_pkt_bufs;
	/* Per buff slot size i.e MTU Size + 4 bytes for magic number + 4 bytes
	 * for Pkt len
	 */
	u32 buff_slot_size;
	/* Base Address for Tx or Rx shared memory */
	u32 base_addr;
} __packed;

struct icve_shm {
	struct icve_shm_info shm_info_tx;
	struct icve_shm_info shm_info_rx;
} __packed;

struct icve_mac_addr {
	char addr[ETH_ALEN];
} __packed;

struct request_message {
	u32 type; /* Request Type */
	u32 id;	  /* Request ID */
	union {
		struct icve_mac_addr mac_addr;
	};
} __packed;

struct response_message {
	u32 type;	/* Response Type */
	u32 id;		/* Response ID */
	union {
		struct icve_shm shm_info;
	};
} __packed;

struct notify_message {
	u32 type;	/* Notify Type */
	u32 id;		/* Notify ID */
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

/*      Shared Memory Layout
 *
 *	---------------------------	*****************
 *	|        MAGIC_NUM        |	 icve_shm_head
 *	|          HEAD           |
 *	---------------------------	*****************
 *	|        MAGIC_NUM        |
 *	|        PKT_1_LEN        |
 *	|          PKT_1          |
 *	---------------------------
 *	|        MAGIC_NUM        |
 *	|        PKT_2_LEN        |	 icve_shm_buf
 *	|          PKT_2          |
 *	---------------------------
 *	|           .             |
 *	|           .             |
 *	---------------------------
 *	|        MAGIC_NUM        |
 *	|        PKT_N_LEN        |
 *	|          PKT_N          |
 *	---------------------------	****************
 *	|        MAGIC_NUM        |      icve_shm_tail
 *	|          TAIL           |
 *	---------------------------	****************
 */

struct icve_shm_index {
	u32 magic_num;
	u32 index;
}  __packed;

struct icve_shm_buf {
	char __iomem *base_addr;	/* start addr of first buffer */
	u32 magic_num;
} __packed;

struct icve_shared_mem {
	struct icve_shm_index __iomem *head;
	struct icve_shm_buf __iomem *buf;
	struct icve_shm_index __iomem *tail;
} __packed;

#endif /* __ICVE_RPMSG_COMMON_H__ */
