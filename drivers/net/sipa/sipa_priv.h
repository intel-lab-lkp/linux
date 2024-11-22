/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Spreadtrum pin controller driver
 * Copyright (C) 2017 Spreadtrum  - http://www.spreadtrum.com
 */

#ifndef _SIPA_PRIV_H_
#define _SIPA_PRIV_H_

#include <linux/alarmtimer.h>
#include <linux/skbuff.h>
#include <linux/interrupt.h>
#include <linux/cdev.h>
#include <linux/regmap.h>
#include <linux/kfifo.h>
#include <linux/soc/sprd/sipa.h>
#include <linux/spinlock.h>

/* common fifo id */
enum sipa_cmn_fifo_index {
	SIPA_CFIFO_USB_UL,
	SIPA_CFIFO_WIFI_UL,
	SIPA_CFIFO_PCIE_UL,
	SIPA_CFIFO_WIAP_DL,
	SIPA_CFIFO_MAP_IN,
	SIPA_CFIFO_USB_DL,
	SIPA_CFIFO_WIFI_DL,
	SIPA_CFIFO_PCIE_DL,
	SIPA_CFIFO_WIAP_UL,
	SIPA_CFIFO_MAP0_OUT,
	SIPA_CFIFO_MAP1_OUT,
	SIPA_CFIFO_MAP2_OUT,
	SIPA_CFIFO_MAP3_OUT,
	SIPA_CFIFO_MAP4_OUT,
	SIPA_CFIFO_MAP5_OUT,
	SIPA_CFIFO_MAP6_OUT,
	SIPA_CFIFO_MAP7_OUT,

	SIPA_CFIFO_MAX,
};

struct sipa_common_fifo {
	enum sipa_cmn_fifo_index idx;

	struct sipa_fifo_attrs tx_fifo;
	struct sipa_fifo_attrs rx_fifo;

	enum sipa_term_type dst_id;
	enum sipa_term_type src_id;

	bool is_receiver;
	bool is_pam;
};

struct sipa_cmn_fifo_tag {
	u32 depth;
	u32 wr;
	u32 rd;
	bool in_iram;

	u32 fifo_base_addr_l;
	u32 fifo_base_addr_h;

	void *virt_addr;
};

struct sipa_cmn_fifo_cfg_tag {
	const char *fifo_name;

	enum sipa_cmn_fifo_index fifo_id;
	enum sipa_cmn_fifo_index fifo_id_dst;

	bool is_recv;
	bool is_pam;

	u32 state;
	u32 pending;
	u32 dst;
	u32 src;

	u32 irq_eb;

	u64 fifo_phy_addr;

	void *priv;
	void __iomem *fifo_reg_base;

	struct kfifo rx_priv_fifo;
	struct kfifo tx_priv_fifo;
	struct sipa_cmn_fifo_tag rx_fifo;
	struct sipa_cmn_fifo_tag tx_fifo;

	u32 enter_flow_ctrl_cnt;
	u32 exit_flow_ctrl_cnt;
};

/* common fifo information */
struct sipa_cmn_fifo_info {
	const char *cfifo_name;
	const char *tx_fifo;
	const char *rx_fifo;

	enum sipa_ep_id relate_ep;
	enum sipa_term_type src_id;
	enum sipa_term_type dst_id;

	/* centered on IPA*/
	bool is_to_ipa;
	bool is_pam;
};

/* ipa hw information */
struct sipa_hw_data {
	const u32 ahb_regnum;
	const struct ipa_register_map *ahb_reg;
	const bool standalone_subsys;
	const u64 dma_mask;
};

/* endpoint which access to IPA */
struct sipa_endpoint {
	enum sipa_ep_id id;

	struct device *dev;

	/* Centered on CPU/PAM */
	struct sipa_common_fifo send_fifo;
	struct sipa_common_fifo recv_fifo;

	struct sipa_comm_fifo_params send_fifo_param;
	struct sipa_comm_fifo_params recv_fifo_param;

	/* private data for sipa_notify_cb */
	void *send_priv;
	void *recv_priv;

	bool inited;
	bool connected;
	bool suspended;
};

/* structure of IPA platform driver */
struct sipa_plat_drv_cfg {
	struct device *dev;

	struct sipa_endpoint *eps[SIPA_EP_MAX];

	/* avoid pam connect and power_wq race */
	struct mutex resume_lock;

	struct delayed_work power_work;
	struct workqueue_struct *power_wq;

	/* ipa power status */
	bool power_flag;

	u32 enable_cnt;
	bool udp_frag;
	bool udp_port;
	atomic_t udp_port_num;

	int is_bypass;

	phys_addr_t glb_phy;
	resource_size_t glb_size;
	void  __iomem *glb_virt_base;

	phys_addr_t iram_phy;
	resource_size_t iram_size;
	void  __iomem *iram_virt_base;

	const struct sipa_hw_data *hw_data;
	u32 suspend_cnt;
	u32 resume_cnt;
	struct sipa_cmn_fifo_cfg_tag cmn_fifo_cfg[SIPA_CFIFO_MAX];
};
#endif //_SIPA_PRIV_H_
