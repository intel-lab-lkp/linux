/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#ifndef __MTK_VCP_IPC_H__
#define __MTK_VCP_IPC_H__

#include <linux/completion.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox/mtk-vcp-mailbox.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

/* IPI result definition */
#define IPI_ACTION_DONE	  0
#define IPI_DEV_ILLEGAL	 -1 /* ipi device is not initialized */
#define IPI_ALREADY_USED	 -2 /* the ipi has be registered */
#define IPI_UNAVAILABLE	 -3 /* the ipi can't be found */
#define IPI_NO_MSGBUF		 -4 /* receiver doesn't have message buffer */
#define IPI_MSG_TOO_BIG		 -5 /* message length is larger than defined */
#define IPI_MBOX_ERR		-99 /* some error from rpmsg layer */

/* mbox recv action definition */
enum mtk_ipi_recv_opt {
	MBOX_RECV_MESSAGE  = 0,
	MBOX_RECV_ACK      = 1,
};

/* mbox table item number definition */
#define send_item_num	3
#define recv_item_num	4
#define VCP_MBOX_NUM	5

/* mbox slot size definition: 1 slot for 4 bytes */
#define MBOX_SLOT_SIZE	0x4
#define MBOX_MAX_PIN	32
#define VCP_MBOX_NUM	5
#define MBOX_SLOT_ALIGN	2

struct mtk_vcp_ipc;
struct mtk_ipi_chan_table;

typedef int (*mbox_pin_cb_t)(u32 ipi_id, void *prdata, void *data, u32 len);

/**
 * mbox pin structure, this is for send definition,
 * @offset: message offset in the slots of a mbox
 * @msg_size: message used slots in the mbox, 4 bytes alignment
 * @pin_index: bit offset in the mbox
 * @ipi_id: ipi enum number
 * @mbox_id: mbox number id
 */
struct mtk_mbox_send_table {
	u32 offset;
	u32 msg_size;
	u32 pin_index;
	u32 ipi_id;
	u32 mbox_id;
};

/**
 * mbox pin structure, this is for receive definition,
 * @offset: message offset in the slots of a mbox
 * @recv_opt: recv option,  0:receive ,1: response
 * @msg_size: message used slots in the mbox, 4 bytes alignment
 * @pin_index: bit offset in the mbox
 * @ipi_id: ipi enum number
 * @mbox_id: mbox number id
 */
struct mtk_mbox_recv_table {
	u32 offset;
	u32 recv_opt;
	u32 msg_size;
	u32 pin_index;
	u32 ipi_id;
	u32 mbox_id;
};

/**
 * struct mtk_ipi_device - device for represent the tinysys using mtk ipi
 * @name: name of tinysys device
 * @id: device id (used to match between rpmsg drivers and devices)
 * @vcp_ipc: vcp ipc structure for tinysys device
 * @table: channel table with endpoint & channel_info & mbox_pin info
 * @prdata: private data for the callback use
 * @ipi_inited: set when vcp_ipi_device_register() done
 */
struct mtk_ipi_device  {
	const char *name;
	struct mtk_vcp_ipc *vcp_ipc;
	struct mtk_ipi_chan_table *table;
	void *prdata;
	int ipi_inited;
};

/**
 * The mtk_mbox_table is a structure used to record the send
 * table and recv table. The send table is used to record
 * the feature ID and size of the sent data. The recv table
 * is used to record the feature ID and size of the received
 * data, and whether a callback needs to be invoked.
 *
 * Following are platform specific interfacer
 * @recv_table: structure mtk_mbox_recv_table
 * @send_table: structure mtk_mbox_send_table
 * @recv_count: receive feature number in this channel
 * @send_count: send feature number in this channel
 */
struct mtk_mbox_table {
	struct mtk_mbox_recv_table recv_table[32];
	struct mtk_mbox_send_table send_table[32];
	u32 recv_count;
	u32 send_count;
};

/**
 * Mbox is a dedicate hardware of a tinysys consists of:
 * 1) a share memory tightly coupled to the tinysys
 * 2) several IRQs
 *
 * Following are platform specific interface
 * @dev: vcp device
 * @name: identity of the device
 * @info_table: mbox info structure
 * @ipi_priv: private data for synchronization layer
 * @mbox_id: mbox number
 * @mbdev: mtk_mbox_table structure
 */
struct mtk_vcp_ipc {
	struct device *dev;
	const char *name;
	struct mtk_mbox_info *info_table;
	void *ipi_priv;
	void *mbox_id;
	struct mtk_mbox_table *mbdev;
};

int mtk_vcp_ipc_device_register(struct mtk_ipi_device *ipidev,
				u32 ipi_chan_count,
				struct mtk_vcp_ipc *vcp_ipc);
int mtk_vcp_ipc_send(struct mtk_ipi_device *ipidev, u32 ipi_id,
		     void *data, u32 len);
int mtk_vcp_ipc_send_compl(struct mtk_ipi_device *ipidev, u32 ipi_id,
			   void *data, u32 len, u32 timeout_ms);
int mtk_vcp_mbox_ipc_register(struct mtk_ipi_device *ipidev, int ipi_id,
			      mbox_pin_cb_t cb, void *prdata, void *msg);
int mtk_vcp_mbox_ipc_unregister(struct mtk_ipi_device *ipidev, int ipi_id);

#endif
