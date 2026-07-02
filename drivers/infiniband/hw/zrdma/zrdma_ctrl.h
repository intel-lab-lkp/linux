/* SPDX-License-Identifier: GPL-2.0-only
 *
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#ifndef ZRDMA_CTRL_H
#define ZRDMA_CTRL_H

#include "zrdma_mem.h"

#define ZXDH_HW_SCHEDULE_OFF 0
#define ZXDH_HW_SCHEDULE_ON 1

enum zxdh_cqp_op_type {
	ZXDH_OP_CEQ_DESTROY = 1,
	ZXDH_OP_AEQ_DESTROY = 2,
	ZXDH_OP_CEQ_CREATE = 5,
	ZXDH_OP_QP_MODIFY = 8,
	ZXDH_OP_CQ_CREATE = 10,
	ZXDH_OP_CQ_DESTROY = 11,
	ZXDH_OP_QP_CREATE = 12,
	ZXDH_OP_QP_DESTROY = 13,
	ZXDH_OP_ALLOC_STAG = 14,
	ZXDH_OP_MR_REG_NON_SHARED = 15,
	ZXDH_OP_DEALLOC_STAG = 16,
	ZXDH_OP_MW_ALLOC = 17,
	ZXDH_OP_QP_FLUSH_WQES = 18,
	ZXDH_OP_REQ_CMDS = 28,
	ZXDH_OP_CMPL_CMDS = 29,
	ZXDH_OP_AH_CREATE = 30,
	ZXDH_OP_AH_DESTROY = 32,
	ZXDH_OP_CONFIG_PTE_TAB = 51,
	ZXDH_OP_CONFIG_PBLE_TAB = 53,
	ZXDH_OP_CONFIG_MAILBOX = 54,
	ZXDH_OP_DMA_WRITE = 55,
	ZXDH_OP_DMA_WRITE32 = 56,
	ZXDH_OP_DMA_READ = 58,
	ZXDH_OP_DMA_READ_USE_CQE = 59,
	ZXDH_OP_QUERY_QPC = 60,
	ZXDH_OP_QUERY_CQC = 61,
	ZXDH_OP_QUERY_SRQC = 62,
	ZXDH_OP_QUERY_CEQC = 63,
	ZXDH_OP_QUERY_AEQC = 64,
	ZXDH_OP_SRQ_CREATE = 65,
	ZXDH_OP_SRQ_DESTROY = 66,
	ZXDH_OP_SRQ_MODIFY = 67,
	ZXDH_OP_QUERY_MKEY = 68,
	ZXDH_OP_CQ_MODIFY_MODERATION = 69,
	ZXDH_OP_QUERY_HW_OBJECT_INFO = 71,
	ZXDH_OP_CQ_MODIFY_OVERFLOW_CHECK_EN = 72,
	/* Must be last entry */
	ZXDH_MAX_CQP_OPS = 73,
};

struct zxdh_hw {
	u32 __iomem *hw_addr;
	u32 __iomem *pci_hw_addr;
	struct device *device;
	struct zxdh_hmc_info hmc;
};

struct zxdh_uk_attrs {
	u64 feature_flags;
	u32 max_hw_wq_frags;
	u32 max_hw_read_sges;
	u32 max_hw_inline;
	u32 max_hw_srq_quanta;
	u32 max_hw_rq_quanta;
	u32 max_hw_wq_quanta;
	u32 min_hw_cq_size;
	u32 max_hw_cq_size;
	u16 max_hw_sq_chunk;
	u32 max_hw_srq_wr;
	u8 hw_rev;
};

struct zxdh_hw_attrs {
	struct zxdh_uk_attrs uk_attrs;
	u64 max_hw_outbound_msg_size;
	u64 max_hw_inbound_msg_size;
	u64 max_mr_size;
	u32 min_hw_qp_id;
	u32 min_hw_aeq_size;
	u32 max_hw_aeq_size;
	u32 min_hw_ceq_size;
	u32 max_hw_ceq_size;
	u32 max_hw_device_pages;
	u32 max_hw_vf_fpm_id;
	u32 first_hw_vf_fpm_id;
	u32 max_hw_ird;
	u32 max_hw_ord;
	u32 max_hw_wqes;
	u32 max_hw_pds;
	u32 max_qp_wr;
	u32 max_srq_wr;
	u32 max_pe_ready_count;
	u32 max_done_count;
	u32 max_sleep_count;
	u32 max_cqp_compl_wait_time_ms;
	u16 max_stat_inst;
	u16 max_stat_idx;
	u32 cqp_timeout_threshold;
	u8 skip_hw;
};

struct zxdh_sc_dev {
	struct list_head cqp_cmd_head;
	spinlock_t cqp_lock; /* Protects CQP command queue submission */
	struct zxdh_dma_mem clear_dpu_mem;
	struct zxdh_dma_mem nof_clear_dpu_mem;
	u64 pte_l2d_startpa;
	u32 pte_l2d_len;
	struct zxdh_hw *hw;
	u8 __iomem *db_addr;
	u32 __iomem *wqe_alloc_db;
	u32 __iomem *cq_arm_db;
	u32 __iomem *cqp_db;
	u32 __iomem *cq_ack_db;
	u64 cqp_cmd_stats[ZXDH_MAX_CQP_OPS];
	struct zxdh_hw_attrs hw_attrs;
	struct zxdh_hmc_info *hmc_info;
	struct zxdh_sc_cqp *cqp;
	struct zxdh_sc_cq *ccq;
	u32 max_ceqs;
	u32 base_qpn;
	u32 base_cqn;
	u32 base_srqn;
	u32 base_ceqn;
	u32 max_qp;
	u32 max_cq;
	u32 max_srq;
	u16 num_vfs;
	u16 active_vfs_num;
	u32 hmc_fn_id;
	u16 vf_id;
	u16 vhca_id;
	u16 vhca_id_pf;
	u16 cache_id;
	u8 ep_id;
	u8 hmc_epid;
	u16 ird_size;
	u32 total_vhca;
	u16 vhca_gqp_start;
	u16 vhca_gqp_cnt;
	u16 vhca_8k_index_start;
	u16 vhca_8k_index_cnt;
	u16 vhca_ud_gqp;
	u16 vhca_ud_8k_index;
	u64 nof_ioq_ddr_addr;
	u8 chip_version;
	u64 l2d_pt_addr;
	u32 l2d_pt_l2_offset;
	u32 l2_pt_num;
	u32 l3_pt_num;
	u8 ceq_valid : 1;
	u8 privileged : 1;
	u8 hmc_use_dpu_ddr : 1;
	u8 np_mode_low_lat : 1;
	struct mutex vchnl_mutex; /* protects virtual channel operations */
	u8 driver_load;
};

struct cqp_info {
	union {
		struct {
			struct zxdh_sc_cq *cq;
			u64 scratch;
		} cq_create;

		struct {
			struct zxdh_sc_cq *cq;
			u64 scratch;
		} cq_destroy;
	} u;
};

struct cqp_cmds_info {
	struct list_head cqp_cmd_entry;
	u8 cqp_cmd;
	u8 post_sq;
	struct cqp_info in;
};

struct zxdh_cqp_err_info {
	u16 maj;
	u16 min;
	const char *desc;
};

struct zxdh_cqp_compl_info {
	u64 op_ret_val;
	u16 maj_err_code;
	u16 min_err_code;
	bool error;
	u8 op_code;
	__le64 addrbuf[5];
};

struct zxdh_cqp_request {
	struct cqp_cmds_info info;
	wait_queue_head_t waitq;
	struct list_head list;
	refcount_t refcnt;
	void (*callback_fcn)(struct zxdh_cqp_request *cqp_request);
	void *param;
	struct zxdh_cqp_compl_info compl_info;
	u8 waiting : 1;
	u8 request_done : 1;
	u8 dynamic : 1;
};

struct zxdh_sc_cqp {
	u64 sq_pa;
	u64 host_ctx_pa;
	void *back_cqp;
	struct zxdh_sc_dev *dev;
	struct zxdh_ring sq_ring;
	struct zxdh_cqp_quanta *sq_base;
	__le64 *host_ctx;
	u64 *scratch_array;
	u32 sq_size;
	u32 hw_sq_size;
	u8 polarity;
	u8 state_cfg : 1;
};

struct zxdh_cqp {
	struct zxdh_sc_cqp sc_cqp;
	spinlock_t req_lock; /* protect CQP request list */
	spinlock_t compl_lock; /* protect CQP completion processing */
	wait_queue_head_t waitq;
	wait_queue_head_t remove_wq;
	struct zxdh_dma_mem sq;
	struct zxdh_dma_mem host_ctx;
	u64 *scratch_array;
	struct zxdh_cqp_request *cqp_requests;
	struct list_head cqp_avail_reqs;
	struct list_head cqp_pending_reqs;
};

struct zxdh_ccq {
	struct zxdh_sc_cq sc_cq;
	struct zxdh_dma_mem mem_cq;
	struct zxdh_dma_mem shadow_area;
};

#endif
