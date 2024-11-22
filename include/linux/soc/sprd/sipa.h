/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Spreadtrum pin controller driver
 * Copyright (C) 2017 Spreadtrum  - http://www.spreadtrum.com
 */

#ifndef _SIPA_H_
#define _SIPA_H_

/**
 * enum sipa_ep_type - names for the various IPA end points
 * these ids used for rx/tx data with IPA
 * NOTE: one sipa EP may related to more than one sipa_term_types
 */
enum sipa_ep_id {
	SIPA_EP_USB,
	SIPA_EP_AP,
	SIPA_EP_CP,
	SIPA_EP_WIAP,
	SIPA_EP_PCIE,
	SIPA_EP_WIFI,

	SIPA_EP_MAX,
};

/**
 * enum sipa_term_type - names for the various IPA source / destination ID
 */
enum sipa_term_type {
	SIPA_TERM_USB = 0x1,
	SIPA_TERM_WIFI1 = 0x2,
	SIPA_TERM_WIFI2 = 0x3,
	SIPA_TERM_CP0 = 0x4,
	SIPA_TERM_CP1 = 0x5,
	SIPA_TERM_VCP = 0x6,
	SIPA_TERM_VAP0 = 0xc,
	SIPA_TERM_VAP1 = 0xd,
	SIPA_TERM_VAP2 = 0xe,
	SIPA_TERM_PCIE0 = 0x10,
	SIPA_TERM_PCIE1 = 0x11,
	SIPA_TERM_PCIE2 = 0x12,
	SIPA_TERM_AP = 0x19,

	SIPA_TERM_MAX = 0x20, /* max 5-bit register */
};

/* rx/tx fifo attribute of cfifo*/
struct sipa_fifo_attrs {
	dma_addr_t dma_addr;
	u32 fifo_depth;
};

/**
 * struct sipa_comm_fifo_params - information needed to setup an IPA
 * common FIFO, the tx/rx are from the perspective of IPA
 * @tx_intr_delay_us: timeout value for interrupt
 * @tx_intr_threshol: threshold value for interrupt
 * @errcode_intr: enable/disable of errcode interrupt
 * @flowctrl_in_tx_full: enable/disable of tx cfifo full interrupt
 * @flow_ctrl_cfg: flow control config
 * @flow_ctrl_irq_mode: flow control interrupt mode
 */
struct sipa_comm_fifo_params {
	u32 intr_to_ap;

	u32 tx_intr_delay_us;
	u32 tx_intr_threshold;
	bool errcode_intr;
	bool flowctrl_in_tx_full;
	u32 flow_ctrl_cfg;
	u32 flow_ctrl_irq_mode;
	u32 tx_enter_flowctrl_watermark;
	u32 tx_leave_flowctrl_watermark;
	u32 rx_enter_flowctrl_watermark;
	u32 rx_leave_flowctrl_watermark;

	u32 data_ptr_cnt;
	u32 buf_size;
	dma_addr_t data_ptr;
};

/**
 * sipa_get_ctrl_pointer() - get the main structure of th sipa driver.
 */
struct sipa_plat_drv_cfg *sipa_get_ctrl_pointer(void);
#endif //_SIPA_H_
