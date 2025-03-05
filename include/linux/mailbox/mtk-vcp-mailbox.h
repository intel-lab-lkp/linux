/* SPDX-License-Identifier: (GPL-2.0 OR MIT) */
/*
 * Copyright (c) 2024 MediaTek Inc.
 */

#ifndef __MTK_VCP_MAILBOX_H__
#define __MTK_VCP_MAILBOX_H__

#define MBOX_SLOT_MAX_SIZE	0x100 /* mbox max slot size */
#define MAX_SLOT_NUM	64

/**
 * struct mtk_ipi_info - channel table that belong to mtk_ipi_device
 * @msg: The share buffer between IPC and mailbox driver
 * @len: Message length
 * @id: IPI number
 * @recv_opt: Recv option,  0:receive ,1: response
 * @index: The pin groups number of the mailbox channel
 * @slot_ofs: Slot offset of the mailbox channel
 * @irq_status: Indicate which pin groups triggered the interrupt
 *
 * It is used between IPC with mailbox driver.
 */
struct mtk_ipi_info {
	void *msg;
	u32 len;
	u32 id;
	u32 recv_opt;
	u32 index;
	u32 slot_ofs;
	u32 irq_status;
};

#endif
