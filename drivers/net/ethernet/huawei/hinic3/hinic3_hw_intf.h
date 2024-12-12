/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved. */

#ifndef HINIC3_HW_INTF_H
#define HINIC3_HW_INTF_H

#include <linux/types.h>
#include <linux/bits.h>

#define MGMT_MSG_CMD_OP_SET   1
#define MGMT_MSG_CMD_OP_GET   0
#define MGMT_CMD_UNSUPPORTED  0xFF

struct mgmt_msg_head {
	u8 status;
	u8 version;
	u8 rsvd0[6];
};

enum cfg_cmd {
	CFG_CMD_GET_DEV_CAP = 0,
};

/* Device capabilities, defined by hw */
struct cfg_cmd_dev_cap {
	struct mgmt_msg_head head;

	u16                  func_id;
	u16                  rsvd1;

	/* Public resources */
	u8                   host_id;
	u8                   ep_id;
	u8                   er_id;
	u8                   port_id;

	u16                  host_total_func;
	u8                   host_pf_num;
	u8                   pf_id_start;
	u16                  host_vf_num;
	u16                  vf_id_start;
	u8                   host_oq_id_mask_val;
	u8                   timer_en;
	u8                   host_valid_bitmap;
	u8                   rsvd_host;

	u16                  svc_cap_en;
	u16                  max_vf;
	u8                   flexq_en;
	u8                   valid_cos_bitmap;
	u8                   port_cos_valid_bitmap;
	u8                   rsvd2[45];

	/* l2nic */
	u16                  nic_max_sq_id;
	u16                  nic_max_rq_id;
	u16                  nic_default_num_queues;

	u8                   rsvd3[250];
};

enum hinic3_service_type {
	SERVICE_T_NIC = 0,
	SERVICE_T_MAX = 1,
	/* Only used for interruption resource management, mark the request module */
	SERVICE_T_INTF = (1 << 15),
};

/* CMDQ MODULE_TYPE */
enum hinic3_mod_type {
	/* HW communication module */
	HINIC3_MOD_COMM   = 0,
	/* L2NIC module */
	HINIC3_MOD_L2NIC  = 1,
	/* Configuration module */
	HINIC3_MOD_CFGM   = 7,
	HINIC3_MOD_HILINK = 14,
};

/* COMM Commands between Driver to fw */
enum hinic3_mgmt_cmd {
	/* Commands for clearing FLR and resources */
	COMM_MGMT_CMD_FUNC_RESET              = 0,
	COMM_MGMT_CMD_FEATURE_NEGO            = 1,
	COMM_MGMT_CMD_FLUSH_DOORBELL          = 2,
	COMM_MGMT_CMD_START_FLUSH             = 3,
	COMM_MGMT_CMD_GET_GLOBAL_ATTR         = 5,
	COMM_MGMT_CMD_SET_FUNC_SVC_USED_STATE = 7,

	/* Driver Configuration Commands */
	COMM_MGMT_CMD_SET_CMDQ_CTXT           = 20,
	COMM_MGMT_CMD_SET_VAT                 = 21,
	COMM_MGMT_CMD_CFG_PAGESIZE            = 22,
	COMM_MGMT_CMD_CFG_MSIX_CTRL_REG       = 23,
	COMM_MGMT_CMD_SET_CEQ_CTRL_REG        = 24,
	COMM_MGMT_CMD_SET_DMA_ATTR            = 25,
};

struct comm_cmd_msix_config {
	struct mgmt_msg_head head;
	u16                  func_id;
	u8                   opcode;
	u8                   rsvd1;
	u16                  msix_index;
	u8                   pending_cnt;
	u8                   coalesce_timer_cnt;
	u8                   resend_timer_cnt;
	u8                   lli_timer_cnt;
	u8                   lli_credit_cnt;
	u8                   rsvd2[5];
};

enum func_reset_type_bits {
	RESET_TYPE_FLUSH        = BIT(0),
	RESET_TYPE_MQM          = BIT(1),
	RESET_TYPE_SMF          = BIT(2),
	RESET_TYPE_PF_BW_CFG    = BIT(3),

	RESET_TYPE_COMM         = BIT(10),
	/* clear mbox and aeq, The RESET_TYPE_COMM bit must be set */
	RESET_TYPE_COMM_MGMT_CH = BIT(11),
	/* clear cmdq and ceq, The RESET_TYPE_COMM bit must be set */
	RESET_TYPE_COMM_CMD_CH  = BIT(12),
	RESET_TYPE_NIC          = BIT(13),
};

#define HINIC3_COMM_RESET \
	(RESET_TYPE_COMM | RESET_TYPE_COMM_CMD_CH | RESET_TYPE_FLUSH | \
	 RESET_TYPE_MQM | RESET_TYPE_SMF | RESET_TYPE_PF_BW_CFG)

struct comm_cmd_func_reset {
	struct mgmt_msg_head head;
	u16                  func_id;
	u16                  rsvd1[3];
	u64                  reset_flag;
};

#define COMM_MAX_FEATURE_QWORD  4
struct comm_cmd_feature_nego {
	struct mgmt_msg_head head;
	u16                  func_id;
	u8                   opcode;
	u8                   rsvd;
	u64                  s_feature[COMM_MAX_FEATURE_QWORD];
};

enum hinic3_cmdq_type {
	HINIC3_CMDQ_SYNC = 0,
	HINIC3_MAX_CMDQ_TYPES = 4
};

struct comm_global_attr {
	u8  max_host_num;
	u8  max_pf_num;
	u16 vf_id_start;
	/* for api cmd to mgmt cpu */
	u8  mgmt_host_node_id;
	u8  cmdq_num;
	u8  rsvd1[34];
};

struct comm_cmd_get_glb_attr {
	struct mgmt_msg_head    head;
	struct comm_global_attr attr;
};

enum hinic3_svc_type {
	SVC_T_COMM = 0,
	SVC_T_NIC  = 1,
};

struct comm_cmd_func_svc_used_state {
	struct mgmt_msg_head head;
	u16                  func_id;
	u16                  svc_type;
	u8                   used_state;
	u8                   rsvd[35];
};

struct comm_cmd_dma_attr_config {
	struct mgmt_msg_head head;
	u16                  func_id;
	u8                   entry_idx;
	u8                   st;
	u8                   at;
	u8                   ph;
	u8                   no_snooping;
	u8                   tph_en;
	u32                  resv1;
};

struct comm_cmd_ceq_ctrl_reg {
	struct mgmt_msg_head head;
	u16                  func_id;
	u16                  q_id;
	u32                  ctrl0;
	u32                  ctrl1;
	u32                  rsvd1;
};

struct comm_cmd_wq_page_size {
	struct mgmt_msg_head head;
	u16                  func_id;
	u8                   opcode;
	/* real_size=4KB*2^page_size, range(0~20) must be checked by driver */
	u8                   page_size;
	u32                  rsvd1;
};

struct comm_cmd_root_ctxt {
	struct mgmt_msg_head head;
	u16                  func_id;
	u8                   set_cmdq_depth;
	u8                   cmdq_depth;
	u16                  rx_buf_sz;
	u8                   lro_en;
	u8                   rsvd1;
	u16                  sq_depth;
	u16                  rq_depth;
	u64                  rsvd2;
};

struct cmdq_ctxt_info {
	u64 curr_wqe_page_pfn;
	u64 wq_block_pfn;
};

struct comm_cmd_cmdq_ctxt {
	struct mgmt_msg_head  head;
	u16                   func_id;
	u8                    cmdq_id;
	u8                    rsvd1[5];
	struct cmdq_ctxt_info ctxt;
};

struct comm_cmd_clear_doorbell {
	struct mgmt_msg_head head;
	u16                  func_id;
	u16                  rsvd1[3];
};

struct comm_cmd_clear_resource {
	struct mgmt_msg_head head;
	u16                  func_id;
	u16                  rsvd1[3];
};

#endif
