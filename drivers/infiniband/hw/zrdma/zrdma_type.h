/* SPDX-License-Identifier: GPL-2.0-only
 *
 * ZTE DingHai Rdma driver
 * Copyright (c) 2022-2026, ZTE Corporation
 */

#ifndef ZRDMA_TYPE_H
#define ZRDMA_TYPE_H

#include "zrdma_mem.h"

#define ZXDH_CQP_WQE_SIZE 8
#define ZXDH_CQE_SIZE 8

struct zxdh_ring {
	u32 head;
	u32 tail;
	u32 size;
};

struct zxdh_cqp_quanta {
	__le64 elem[ZXDH_CQP_WQE_SIZE];
};

struct zxdh_cqe {
	__le64 buf[ZXDH_CQE_SIZE];
};

struct zxdh_cq_uk {
	struct zxdh_cqe *cq_base;
	u32 __iomem *cqe_alloc_db;
	u32 __iomem *cq_ack_db;
	__le64 *shadow_area;
	u32 cq_id;
	u32 cq_size;
	u32 cq_log_size;
	u32 cqe_rd_cnt;
	bool valid_cq;
	struct zxdh_ring cq_ring;
	u8 polarity;
	u8 armed : 1;
	u8 cqe_size;
};

struct zxdh_sc_cq {
	struct zxdh_cq_uk cq_uk;
	u64 cq_pa;
	u64 shadow_area_pa;
	struct zxdh_sc_dev *dev;
	void *pbl_list;
	void *back_cq;
	u32 ceq_id;
	u32 ceq_index;
	u32 shadow_read_threshold;
	u8 pbl_chunk_size;
	u8 cq_type;
	u8 tph_val;
	u32 first_pm_pbl_idx;
	u8 ceqe_mask : 1;
	u8 virtual_map : 1;
	u8 ceq_id_valid : 1;
	u8 tph_en;
	u8 cq_st;
	u16 is_in_list_cnt;
	u16 cq_max;
	u16 cq_period;
	u8 scqe_break_moderation_en : 1;
	u8 cq_overflow_locked_flag : 1;
};

struct zxdh_ver_info {
	u16 major;
	u16 minor;
	u64 support;
};

enum zxdh_function_type {
	ZXDH_FUNCTION_TYPE_PF,
	ZXDH_FUNCTION_TYPE_VF,
};

struct zxdh_core_dev_info {
	struct pci_dev *pdev;
	struct auxiliary_device *adev;
	u32 __iomem *hw_addr;
	struct zxdh_ver_info ver;
	void *auxiliary_priv;
	enum zxdh_function_type ftype;
	u16 vport_id;
	u16 slot_id;
	struct net_device *netdev;
	struct msix_entry msix_entries;
	u16 msix_count;
	void *dh_dev;
};

struct zxdh_rdma_if {
	void *(*get_rdma_netdev)(void *dh_dev);
};

struct zxdh_auxiliary_dev {
	struct auxiliary_device adev;
	struct zxdh_core_dev_info *zxdh_info;
	struct zxdh_rdma_if *rdma_ops;
	void *ops;
	void *parent;
	void *auxiliary_ops[18];
};

#endif /* ZRDMA_TYPE_H */
