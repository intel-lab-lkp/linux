/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2019 MediaTek Inc.
 */

#ifndef __MTK_CMDQ_SEC_MAILBOX_H__
#define __MTK_CMDQ_SEC_MAILBOX_H__

#include <linux/mailbox_controller.h>
#include <linux/types.h>

#include <linux/mailbox/mtk-cmdq-sec-iwc-common.h>
#include <linux/mailbox/mtk-cmdq-sec-tee.h>

#define CMDQ_INVALID_THREAD		(-1)
#define CMDQ_MAX_TASK_IN_SECURE_THREAD	(16)
#define ADDR_METADATA_MAX_COUNT_ORIGIN	(8)

/**
 * CMDQ_MAX_COOKIE_VALUE - max value of CMDQ_THR_EXEC_CNT_PA (value starts from 0)
 */
#define CMDQ_MAX_COOKIE_VALUE           (0xffff)

/**
 * enum cmdq_sec_scenario - scenario settings for cmdq TA.
 * @CMDQ_SEC_SCNR_PRIMARY_DISP: primary display vdo mode enable.
 * @CMDQ_SEC_SCNR_SUB_DISP: external display vdo mode enable.
 * @CMDQ_SEC_SCNR_PRIMARY_DISP_DISABLE: primary display vdo mode disable.
 * @CMDQ_SEC_SCNR_SUB_DISP_DISABLE: external display vdo mode disable.
 * @CMDQ_SEC_SCNR_MAX: the end of enum.
 *
 * These states are used to record the state of IWC message structure.
 */
enum cmdq_sec_scenario {
	CMDQ_SEC_SCNR_PRIMARY_DISP		= 1,
	CMDQ_SEC_SCNR_SUB_DISP			= 4,
	CMDQ_SEC_SCNR_PRIMARY_DISP_DISABLE	= 18,
	CMDQ_SEC_SCNR_SUB_DISP_DISABLE		= 19,
	CMDQ_SEC_SCNR_MAX,
};

/**
 * enum cmdq_iwc_state_enum - state of Inter-world Communication(IWC) message
 * @IWC_INIT: state of initializing tee context, means tee context has not initialized.
 * @IWC_CONTEXT_INITED: tee context has initialized.
 * @IWC_WSM_ALLOCATED: world share memory has allocated.
 * @IWC_SES_OPENED: session to the tee context has opend.
 * @IWC_SES_ON_TRANSACTED: session to the tee context has transacted.
 * @IWC_STATE_MAX: the end of enum.
 *
 * These states are used to record the state of IWC message structure.
 */
enum cmdq_iwc_state_enum {
	IWC_INIT,
	IWC_CONTEXT_INITED,
	IWC_WSM_ALLOCATED,
	IWC_SES_OPENED,
	IWC_SES_ON_TRANSACTED,
	IWC_STATE_MAX,
};

/**
 * struct gce_sec_plat - used to pass platform data from cmdq driver.
 * @mbox: pointer to mbox controller.
 * @base: GCE register base va.
 * @hwid: GCE core id.
 * @secure_thread_nr: number of secure thread.
 * @secure_thread_min: min index of secure thread.
 * @cmdq_event: secure EOF event id.
 * @shift: address shift bit for GCE
 */
struct gce_sec_plat {
	struct mbox_controller *mbox;
	void __iomem *base;
	u32 hwid;
	u8 secure_thread_nr;
	u8 secure_thread_min;
	u32 cmdq_event;
	u8 shift;
};

struct cmdq_sec_mailbox {
	const struct mbox_chan_ops *ops;
};

extern struct cmdq_sec_mailbox cmdq_sec_mbox;

/**
 * struct cmdq_sec_data - used to translate secure buffer PA related instruction
 * @addr_metadata_cnt: count of element in addr_list.
 * @addr_metadatas: array of iwc_cmdq_addr_metadata_t.
 * @addr_metadata_max_cnt: Reserved.
 * @scenario: scenario config for secure world.
 */
struct cmdq_sec_data {
	u32 addr_metadata_cnt;
	u64 addr_metadatas;
	u32 addr_metadata_max_cnt;
	enum cmdq_sec_scenario scenario;
};

u16 cmdq_sec_get_eof_event_id(struct mbox_chan *chan);
dma_addr_t cmdq_sec_get_cookie_addr(struct mbox_chan *chan);
dma_addr_t cmdq_sec_get_exec_cnt_addr(struct mbox_chan *chan);

#endif /* __MTK_CMDQ_SEC_MAILBOX_H__ */
