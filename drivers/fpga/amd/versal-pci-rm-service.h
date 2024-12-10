/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Driver for Versal PCIe device
 *
 * Copyright (C) 2024 Advanced Micro Devices, Inc. All rights reserved.
 */

#ifndef __RM_SERVICE_H
#define __RM_SERVICE_H

#define RM_HDR_OFF			0x0
#define RM_HDR_MAGIC_NUM		0x564D5230
#define RM_QUEUE_HDR_MAGIC_NUM		0x5847513F
#define RM_PCI_IO_BAR_OFF		0x2010000
#define RM_PCI_IO_SIZE			SZ_4K
#define RM_PCI_SHMEM_BAR_OFF		0x8000000
#define RM_PCI_SHMEM_SIZE		SZ_128M
#define RM_PCI_SHMEM_HDR_SIZE		0x28

#define RM_QUEUE_HDR_MAGIC_NUM_OFF	0x0
#define RM_IO_SQ_PIDX_OFF		0x0
#define RM_IO_CQ_PIDX_OFF		0x100

#define RM_CMD_ID_MIN			1
#define RM_CMD_ID_MAX			(BIT(17) - 1)
#define RM_CMD_SQ_HDR_OPS_MSK		GENMASK(15, 0)
#define RM_CMD_SQ_HDR_SIZE_MSK		GENMASK(14, 0)
#define RM_CMD_SQ_SLOT_SIZE		SZ_512
#define RM_CMD_CQ_SLOT_SIZE		SZ_16
#define RM_CMD_CQ_BUFFER_SIZE		(1024 * 1024)
#define RM_CMD_CQ_BUFFER_OFFSET		0x0
#define RM_CMD_LOG_PAGE_TYPE_MASK	GENMASK(15, 0)
#define RM_CMD_VMR_CONTROL_MSK		GENMASK(10, 8)
#define RM_CMD_VMR_CONTROL_PS_MASK	BIT(9)

#define RM_CMD_WAIT_CONFIG_TIMEOUT	msecs_to_jiffies(10 * 1000)
#define RM_CMD_WAIT_DOWNLOAD_TIMEOUT	msecs_to_jiffies(300 * 1000)

#define RM_COMPLETION_TIMER		(HZ / 10)
#define RM_HEALTH_CHECK_TIMER		(HZ)

#define RM_INVALID_SLOT			0

enum rm_queue_opcode {
	RM_QUEUE_OP_LOAD_XCLBIN		= 0x0,
	RM_QUEUE_OP_GET_LOG_PAGE	= 0x8,
	RM_QUEUE_OP_LOAD_FW		= 0xA,
	RM_QUEUE_OP_LOAD_APU_FW		= 0xD,
	RM_QUEUE_OP_VMR_CONTROL		= 0xE,
	RM_QUEUE_OP_IDENTIFY		= 0x202,
};

struct rm_cmd_sq_hdr {
	__u16 opcode;
	__u16 msg_size;
	__u16 id;
	__u16 reserved;
} __packed;

struct rm_cmd_cq_hdr {
	__u16 id;
	__u16 reserved;
} __packed;

struct rm_cmd_sq_bin {
	__u64			address;
	__u32			size;
	__u32			reserved1;
	__u32			reserved2;
	__u32			reserved3;
	__u64			reserved4;
} __packed;

struct rm_cmd_sq_log_page {
	__u64			address;
	__u32			size;
	__u32			reserved1;
	__u32			type;
	__u32			reserved2;
} __packed;

struct rm_cmd_sq_ctrl {
	__u32			status;
} __packed;

struct rm_cmd_sq_data {
	union {
		struct rm_cmd_sq_log_page	page;
		struct rm_cmd_sq_bin		bin;
		struct rm_cmd_sq_ctrl		ctrl;
	};
} __packed;

struct rm_cmd_cq_identify {
	__u16			major;
	__u16			minor;
	__u32			reserved;
} __packed;

struct rm_cmd_cq_log_page {
	__u32			len;
	__u32			reserved;
} __packed;

struct rm_cmd_cq_control {
	__u16			status;
	__u16			reserved1;
	__u32			reserved2;
} __packed;

struct rm_cmd_cq_data {
	union {
		struct rm_cmd_cq_identify	identify;
		struct rm_cmd_cq_log_page	page;
		struct rm_cmd_cq_control	ctrl;
		__u32				reserved[2];
	};
	__u32			rcode;
} __packed;

struct rm_cmd_sq_msg {
	struct rm_cmd_sq_hdr	hdr;
	struct rm_cmd_sq_data	data;
} __packed;

struct rm_cmd_cq_msg {
	struct rm_cmd_cq_hdr	hdr;
	struct rm_cmd_cq_data	data;
} __packed;

struct rm_cmd {
	struct rm_device	*rdev;
	struct list_head	list;
	struct completion	executed;
	struct rm_cmd_sq_msg	sq_msg;
	struct rm_cmd_cq_msg	cq_msg;
	enum rm_queue_opcode	opcode;
	__u8			*buffer;
	ssize_t			size;
};

enum rm_queue_type {
	RM_QUEUE_SQ,
	RM_QUEUE_CQ
};

enum rm_cmd_log_page_type {
	RM_CMD_LOG_PAGE_AXI_TRIP_STATUS	= 0x0,
	RM_CMD_LOG_PAGE_FW_ID		= 0xA,
};

struct rm_queue {
	enum rm_queue_type	type;
	__u32			pidx;
	__u32			cidx;
	__u32			offset;
	__u32			data_offset;
	__u32			data_size;
	struct semaphore	data_lock;
};

struct rm_queue_header {
	__u32			magic;
	__u32			version;
	__u32			size;
	__u32			sq_off;
	__u32			sq_slot_size;
	__u32			cq_off;
	__u32			sq_cidx;
	__u32			cq_cidx;
};

struct rm_header {
	__u32			magic;
	__u32			queue_base;
	__u32			queue_size;
	__u32			status_off;
	__u32			status_len;
	__u32			log_index;
	__u32			log_off;
	__u32			log_size;
	__u32			data_start;
	__u32			data_end;
};

struct rm_device {
	struct versal_pci_device	*vdev;

	struct rm_header	rm_metadata;
	__u32			queue_buffer_start;
	__u32			queue_buffer_size;
	__u32			queue_base;

	/* Lock to queue access */
	struct mutex		queue;
	struct rm_queue		sq;
	struct rm_queue		cq;
	__u32			queue_size;

	struct timer_list	msg_timer;
	struct work_struct	msg_monitor;
	struct timer_list	health_timer;
	struct work_struct	health_monitor;
	struct list_head	submitted_cmds;

	__u32			firewall_tripped;
};

#endif	/* __RM_SERVICE_H */
